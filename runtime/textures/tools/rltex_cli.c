/* rltex - command line front end to the texture library.
 *
 *   rltex config  [--config port.toml]             validate and print the resolved block sizes
 *   rltex key     FILE.bc                          print the cache key and header fields
 *   rltex plan    [--config port.toml] FILE.bc     print the BC->ASTC plan as one JSON line
 *   rltex decode  [--rgba8] FILE.bc OUT.{ktx|raw}  bcdec decode (ktx = KTX1 RGBA8 / RGBA16F)
 *   rltex prepare [--config port.toml] FILE.bc OUT.ktx
 *                                                  decode to the exact astcenc CLI input
 *                                                  (KTX RGBA8 or RGBA16F) and print the plan
 *   rltex lookup  [--config port.toml] --root DOCUMENTS FILE.bc
 *                                                  run the runtime's rltex_acquire() against a
 *                                                  Documents tree: prints "hit" (ASTC cache) or
 *                                                  "miss" (decoded; also saves .bc + miss log, as
 *                                                  the device does). Background encode is off.
 *   rltex encode  [--config port.toml] [--quality NAME|NUM] FILE.bc OUT.astc
 *                                                  library-path ASTC encode (same code as the
 *                                                  on-device background encoder)
 *
 * KTX1 layout and the fields astcenc requires are from astc-encoder
 * Source/astcenccli_image_load_store.cpp:871-1000 (ktx_header, load_ktx_uncompressed_image).
 */
#define _POSIX_C_SOURCE 200809L
#include "rltex.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void put32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

static int die(const char *what, int status)
{
	fprintf(stderr, "rltex: %s: %s\n", what, rltex_status_string(status));
	return 1;
}

static int load_bc(const char *path, uint8_t **file, rltex_desc *d, const uint8_t **blob, size_t *bsz)
{
	size_t n;
	int r = rltex_fs_read_all(path, file, &n);
	if (r) return r;
	r = rltex_bc_file_parse(*file, n, d, blob, bsz);
	if (r) { free(*file); *file = NULL; }
	return r;
}

static int write_ktx(const char *path, rltex_pixfmt fmt, uint32_t w, uint32_t h, const void *data)
{
	static const uint8_t magic[12] = {0xAB, 0x4B, 0x54, 0x58, 0x20, 0x31, 0x31, 0xBB, 0x0D, 0x0A, 0x1A, 0x0A};
	size_t bpp = rltex_pixfmt_bytes(fmt), n = (size_t)w * h * bpp;
	uint8_t *f = (uint8_t *)malloc(64 + 4 + n);
	uint32_t type, tsize, ifmt;
	int r;
	if (!f) return RLTEX_E_NOMEM;
	if (fmt == RLTEX_PIX_RGBA8_UNORM) { type = 0x1401; tsize = 1; ifmt = 0x8058; }
	else if (fmt == RLTEX_PIX_RGBA16_FLOAT) { type = 0x140B; tsize = 2; ifmt = 0x881A; }
	else { free(f); return RLTEX_E_UNSUPPORTED; }
	memcpy(f, magic, 12);
	put32(f + 12, 0x04030201);
	put32(f + 16, type);
	put32(f + 20, tsize);
	put32(f + 24, 0x1908);  /* GL_RGBA */
	put32(f + 28, ifmt);
	put32(f + 32, 0x1908);  /* base internal format */
	put32(f + 36, w);
	put32(f + 40, h);
	put32(f + 44, 0);       /* depth */
	put32(f + 48, 0);       /* array elements */
	put32(f + 52, 1);       /* faces */
	put32(f + 56, 1);       /* mip levels */
	put32(f + 60, 0);       /* key/value bytes */
	put32(f + 64, (uint32_t)n);
	memcpy(f + 68, data, n);
	r = rltex_fs_write_atomic(path, f, 68 + n);
	free(f);
	return r;
}

static const char *ext_of(const char *p)
{
	const char *d = strrchr(p, '.');
	return d ? d : "";
}

static void print_plan(const rltex_desc *d, const char *key, const rltex_bc_info *info,
                       const rltex_config *cfg)
{
	rltex_astc_params p;
	int r = rltex_astc_params_for(cfg, info, &p);
	printf("{\"key\":\"%s\",\"dxgi\":%u,\"format\":\"%s\",\"srgb\":%s,\"signed\":%s,"
	       "\"width\":%u,\"height\":%u,\"mip\":%u,\"slice\":%u,\"decoded\":\"%s\",",
	       key, d->dxgi_format, rltex_bc_kind_name(info->kind), info->is_srgb ? "true" : "false",
	       info->is_signed ? "true" : "false", d->width, d->height, d->mip_level, d->array_slice,
	       rltex_pixfmt_name(rltex_decoded_pixfmt(info)));
	if (r) {
		printf("\"astc\":false}\n");
		return;
	}
	printf("\"astc\":true,\"block\":\"%ux%u\",\"astcenc_mode\":\"%s\",\"esw\":\"%s\",\"ssw\":\"%s\","
	       "\"view_swizzle\":\"%s\",\"input\":\"%s\"}\n",
	       p.block_x, p.block_y, rltex_astc_profile_cli_flag(p.profile), p.encode_swizzle,
	       p.sample_weights, p.view_swizzle, p.hdr ? "rgba16f" : "rgba8");
}

int main(int argc, char **argv)
{
	const char *cmd = argc > 1 ? argv[1] : "";
	const char *config_path = NULL, *quality = NULL, *root = NULL;
	int rgba8 = 0, ai = 2;
	const char *pos[4];
	int npos = 0;
	rltex_config cfg;
	char err[512] = "";
	uint8_t *file = NULL;
	const uint8_t *blob;
	size_t bsz;
	rltex_desc d;
	rltex_bc_info info;
	char key[RLTEX_KEY_HEX_SIZE];
	int r;

	rltex_config_defaults(&cfg);
	for (; ai < argc; ai++) {
		if (!strcmp(argv[ai], "--config") && ai + 1 < argc) config_path = argv[++ai];
		else if (!strcmp(argv[ai], "--quality") && ai + 1 < argc) quality = argv[++ai];
		else if (!strcmp(argv[ai], "--root") && ai + 1 < argc) root = argv[++ai];
		else if (!strcmp(argv[ai], "--rgba8")) rgba8 = 1;
		else if (npos < 4) pos[npos++] = argv[ai];
		else { fprintf(stderr, "rltex: too many arguments\n"); return 2; }
	}
	if (config_path) {
		r = rltex_config_load_file(&cfg, config_path, err, sizeof err);
		if (r < 0) return die(config_path, r);
		if (r > 0) { fprintf(stderr, "rltex: invalid config values:\n%s", err); return 2; }
	}
	if (quality) {
		char line[128];
		snprintf(line, sizeof line, "astc_runtime_quality = \"%s\"\n", quality);
		if (rltex_config_parse_toml(&cfg, line, err, sizeof err)) {
			fprintf(stderr, "rltex: %s", err);
			return 2;
		}
	}
	if (!strcmp(cmd, "config")) {
		printf("bc1=%ux%u bc2=%ux%u bc3=%ux%u bc4=%ux%u bc5=%ux%u bc6h=%ux%u bc7=%ux%u bc6h_mode=%s "
		       "runtime_quality=%g\n",
		       cfg.bc1.x, cfg.bc1.y, cfg.bc2.x, cfg.bc2.y, cfg.bc3.x, cfg.bc3.y, cfg.bc4.x, cfg.bc4.y,
		       cfg.bc5.x, cfg.bc5.y, cfg.bc6h.x, cfg.bc6h.y, cfg.bc7.x, cfg.bc7.y,
		       cfg.bc6h_astc_hdr ? "astc_hdr" : "rgba16f", (double)cfg.runtime_quality);
		return 0;
	}
	if (!strcmp(cmd, "key") || !strcmp(cmd, "plan") || !strcmp(cmd, "decode") ||
	    !strcmp(cmd, "prepare") || !strcmp(cmd, "encode") || !strcmp(cmd, "lookup")) {
		int need = (!strcmp(cmd, "key") || !strcmp(cmd, "plan") || !strcmp(cmd, "lookup")) ? 1 : 2;
		if (npos != need) { fprintf(stderr, "rltex %s: expected %d file argument(s)\n", cmd, need); return 2; }
		if ((r = load_bc(pos[0], &file, &d, &blob, &bsz)) != RLTEX_OK) return die(pos[0], r);
		rltex_bc_info_for_dxgi(d.dxgi_format, &info);
		rltex_key_compute(&d, blob, bsz, key);
	} else {
		fprintf(stderr, "usage: rltex {config|key|plan|decode|prepare|encode|lookup} [options] FILE.bc [OUT]\n");
		return 2;
	}

	if (!strcmp(cmd, "key")) {
		printf("%s dxgi=%u %ux%u mip=%u slice=%u blob_bytes=%zu\n", key, d.dxgi_format, d.width,
		       d.height, d.mip_level, d.array_slice, bsz);
	} else if (!strcmp(cmd, "plan")) {
		print_plan(&d, key, &info, &cfg);
	} else if (!strcmp(cmd, "decode") || !strcmp(cmd, "prepare")) {
		rltex_pixfmt pf;
		size_t n = (size_t)d.width * d.height * rltex_pixfmt_bytes(rltex_decoded_pixfmt(&info));
		void *pix = malloc(n);
		const char *out = pos[1];
		int prepare = !strcmp(cmd, "prepare");
		if (!pix) return die("alloc", RLTEX_E_NOMEM);
		if (prepare) {
			rltex_astc_params probe;
			if (rltex_astc_params_for(&cfg, &info, &probe) != RLTEX_OK) {
				/* nothing to hand to astcenc (SNORM, BC6H SF16, BC6H kept as RGBA16F) */
				print_plan(&d, key, &info, &cfg);
				free(pix);
				free(file);
				return 0;
			}
		}
		if ((r = rltex_bc_decode(&d, blob, bsz, pix, n, &pf)) != RLTEX_OK) return die("decode", r);
		if (prepare || rgba8) {
			if (pf != RLTEX_PIX_RGBA16_FLOAT) {
				uint8_t *rgba = (uint8_t *)malloc((size_t)d.width * d.height * 4);
				if (!rgba) return die("alloc", RLTEX_E_NOMEM);
				if ((r = rltex_expand_to_rgba8(pf, pix, d.width, d.height, rgba)) != RLTEX_OK)
					return die("expand", r);
				free(pix);
				pix = rgba;
				pf = RLTEX_PIX_RGBA8_UNORM;
			}
		}
		if (!strcmp(ext_of(out), ".ktx")) r = write_ktx(out, pf, d.width, d.height, pix);
		else r = rltex_fs_write_atomic(out, pix, (size_t)d.width * d.height * rltex_pixfmt_bytes(pf));
		if (r) return die(out, r);
		if (prepare) print_plan(&d, key, &info, &cfg);
		else printf("%s %s %ux%u\n", key, rltex_pixfmt_name(pf), d.width, d.height);
		free(pix);
	} else if (!strcmp(cmd, "lookup")) {
		rltex_ctx *ctx;
		rltex_tex t;
		if (!root) {
			fprintf(stderr, "rltex lookup: --root DOCUMENTS required\n");
			return 2;
		}
		cfg.background_encode = 0;
		if ((r = rltex_ctx_create(root, &cfg, &ctx)) != RLTEX_OK) return die(root, r);
		if ((r = rltex_acquire(ctx, &d, blob, bsz, &t)) != RLTEX_OK) return die("acquire", r);
		if (t.source == RLTEX_SRC_ASTC_CACHE)
			printf("hit %s block=%ux%u bytes=%zu view=%s\n", t.key, t.astc_block_x, t.astc_block_y,
			       t.astc_size, t.view_swizzle);
		else
			printf("miss %s decoded=%s\n", t.key, rltex_pixfmt_name(t.pixfmt));
		rltex_tex_release(&t);
		rltex_ctx_destroy(ctx);
	} else { /* encode */
		rltex_astc_params p;
		rltex_astc_encoder *enc;
		uint8_t *astc;
		size_t astc_sz, n;
		void *pix, *rgba = NULL;
		rltex_pixfmt pf;
		if ((r = rltex_astc_params_for(&cfg, &info, &p)) != RLTEX_OK) return die("mapping", r);
		n = (size_t)d.width * d.height * rltex_pixfmt_bytes(rltex_decoded_pixfmt(&info));
		pix = malloc(n);
		if (!pix) return die("alloc", RLTEX_E_NOMEM);
		if ((r = rltex_bc_decode(&d, blob, bsz, pix, n, &pf)) != RLTEX_OK) return die("decode", r);
		if (!p.hdr && pf != RLTEX_PIX_RGBA8_UNORM) {
			rgba = malloc((size_t)d.width * d.height * 4);
			if (!rgba) return die("alloc", RLTEX_E_NOMEM);
			if ((r = rltex_expand_to_rgba8(pf, pix, d.width, d.height, (uint8_t *)rgba)) != RLTEX_OK)
				return die("expand", r);
		}
		enc = rltex_astc_encoder_create();
		if (!enc) return die("astc encoder", RLTEX_E_UNSUPPORTED);
		r = rltex_astc_encoder_encode(enc, &p, cfg.runtime_quality, rgba ? rgba : pix, d.width,
		                              d.height, &astc, &astc_sz);
		if (r) return die("astc encode", r);
		if ((r = rltex_fs_write_atomic(pos[1], astc, astc_sz)) != RLTEX_OK) return die(pos[1], r);
		printf("%s %ux%u block=%ux%u bytes=%zu\n", key, d.width, d.height, p.block_x, p.block_y, astc_sz);
		rltex_astc_encoder_destroy(enc);
		free(astc);
		free(rgba);
		free(pix);
	}
	free(file);
	return 0;
}
