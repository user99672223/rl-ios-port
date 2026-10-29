/* ASTC compression through the astc-encoder 5.7.0 library API.
 *
 * Every call below was checked against external/astc-encoder/Source/astcenc.h:
 *   astcenc_config_init(profile, block_x, block_y, block_z, quality, flags, &config)  :725
 *   astcenc_context_alloc(&config, thread_count, &context, parent)                    :761
 *   astcenc_compress_image(ctx, &image, &swizzle, out, out_len, thread_index)         :785
 *   astcenc_compress_reset(ctx)                                                       :806
 *   astcenc_context_free(ctx)                                                         :864
 *   struct astcenc_image { dim_x, dim_y, dim_z, data_type, void **data }              :613
 * and against how the CLI drives them (Source/astcenccli_toplevel.cpp:2073 context_alloc,
 * :2160 buffer_size = blocks_x*blocks_y*blocks_z*16, :2194 compress_image).
 * The -ssw CLI option is implemented as cw_*_weight = 1 or 0 (astcenccli_toplevel.cpp:792-845),
 * which is what is done here.
 */
#include "rltex.h"

#include <stdlib.h>
#include <string.h>

#ifdef RLTEX_WITH_ASTCENC

#include "astcenc.h"

struct rltex_astc_encoder {
	struct astcenc_context *ctx;
	int have;
	rltex_astc_profile profile;
	uint32_t bx, by;
	float quality;
	char ssw[5];
};

rltex_astc_encoder *rltex_astc_encoder_create(void)
{
	return (rltex_astc_encoder *)calloc(1, sizeof(rltex_astc_encoder));
}

void rltex_astc_encoder_destroy(rltex_astc_encoder *e)
{
	if (!e) return;
	if (e->ctx) astcenc_context_free(e->ctx);
	free(e);
}

static int to_swz(char c, enum astcenc_swz *o)
{
	switch (c) {
	case 'r': *o = ASTCENC_SWZ_R; return 0;
	case 'g': *o = ASTCENC_SWZ_G; return 0;
	case 'b': *o = ASTCENC_SWZ_B; return 0;
	case 'a': *o = ASTCENC_SWZ_A; return 0;
	case '0': *o = ASTCENC_SWZ_0; return 0;
	case '1': *o = ASTCENC_SWZ_1; return 0;
	}
	return -1;
}

static enum astcenc_profile to_profile(rltex_astc_profile p)
{
	switch (p) {
	case RLTEX_ASTC_LDR: return ASTCENC_PRF_LDR;
	case RLTEX_ASTC_LDR_SRGB: return ASTCENC_PRF_LDR_SRGB;
	case RLTEX_ASTC_HDR_RGB_LDR_A: return ASTCENC_PRF_HDR_RGB_LDR_A;
	}
	return ASTCENC_PRF_LDR;
}

static int ensure_ctx(rltex_astc_encoder *e, const rltex_astc_params *p, float quality)
{
	struct astcenc_config cfg;
	enum astcenc_error err;
	if (e->have && e->profile == p->profile && e->bx == p->block_x && e->by == p->block_y &&
	    e->quality == quality && !strcmp(e->ssw, p->sample_weights))
		return RLTEX_OK;
	if (e->ctx) {
		astcenc_context_free(e->ctx);
		e->ctx = NULL;
		e->have = 0;
	}
	err = astcenc_config_init(to_profile(p->profile), p->block_x, p->block_y, 1, quality,
	                          ASTCENC_FLG_SELF_DECOMPRESS_ONLY, &cfg);
	if (err != ASTCENC_SUCCESS) return RLTEX_E_ASTC;
	cfg.cw_r_weight = strchr(p->sample_weights, 'r') ? 1.0f : 0.0f;
	cfg.cw_g_weight = strchr(p->sample_weights, 'g') ? 1.0f : 0.0f;
	cfg.cw_b_weight = strchr(p->sample_weights, 'b') ? 1.0f : 0.0f;
	cfg.cw_a_weight = strchr(p->sample_weights, 'a') ? 1.0f : 0.0f;
	err = astcenc_context_alloc(&cfg, 1, &e->ctx, NULL);
	if (err != ASTCENC_SUCCESS) {
		e->ctx = NULL;
		return err == ASTCENC_ERR_OUT_OF_MEM ? RLTEX_E_NOMEM : RLTEX_E_ASTC;
	}
	e->have = 1;
	e->profile = p->profile;
	e->bx = p->block_x;
	e->by = p->block_y;
	e->quality = quality;
	strcpy(e->ssw, p->sample_weights);
	return RLTEX_OK;
}

int rltex_astc_encoder_encode(rltex_astc_encoder *e, const rltex_astc_params *p, float quality,
                              const void *pixels, uint32_t w, uint32_t h, uint8_t **out,
                              size_t *out_size)
{
	struct astcenc_image img;
	struct astcenc_swizzle swz;
	void *slice = (void *)pixels;
	uint8_t *buf;
	size_t payload;
	rltex_astc_header hdr;
	int r;
	enum astcenc_error err;

	if (!e || !p || !pixels || !out || !w || !h) return RLTEX_E_ARG;
	if (to_swz(p->encode_swizzle[0], &swz.r) || to_swz(p->encode_swizzle[1], &swz.g) ||
	    to_swz(p->encode_swizzle[2], &swz.b) || to_swz(p->encode_swizzle[3], &swz.a))
		return RLTEX_E_ARG;
	r = ensure_ctx(e, p, quality);
	if (r) return r;

	payload = (size_t)((w + p->block_x - 1) / p->block_x) * ((h + p->block_y - 1) / p->block_y) * 16;
	buf = (uint8_t *)malloc(RLTEX_ASTC_HEADER_SIZE + payload);
	if (!buf) return RLTEX_E_NOMEM;

	img.dim_x = w;
	img.dim_y = h;
	img.dim_z = 1;
	img.data_type = p->hdr ? ASTCENC_TYPE_F16 : ASTCENC_TYPE_U8;
	img.data = &slice;
	err = astcenc_compress_image(e->ctx, &img, &swz, buf + RLTEX_ASTC_HEADER_SIZE, payload, 0);
	astcenc_compress_reset(e->ctx);
	if (err != ASTCENC_SUCCESS) {
		free(buf);
		return RLTEX_E_ASTC;
	}
	hdr.block_x = p->block_x;
	hdr.block_y = p->block_y;
	hdr.block_z = 1;
	hdr.dim_x = w;
	hdr.dim_y = h;
	hdr.dim_z = 1;
	rltex_astc_header_write(&hdr, buf);
	*out = buf;
	*out_size = RLTEX_ASTC_HEADER_SIZE + payload;
	return RLTEX_OK;
}

#else /* !RLTEX_WITH_ASTCENC */

rltex_astc_encoder *rltex_astc_encoder_create(void) { return NULL; }
void rltex_astc_encoder_destroy(rltex_astc_encoder *e) { (void)e; }
int rltex_astc_encoder_encode(rltex_astc_encoder *e, const rltex_astc_params *p, float quality,
                              const void *pixels, uint32_t w, uint32_t h, uint8_t **out,
                              size_t *out_size)
{
	(void)e; (void)p; (void)quality; (void)pixels; (void)w; (void)h; (void)out; (void)out_size;
	return RLTEX_E_UNSUPPORTED;
}

#endif
