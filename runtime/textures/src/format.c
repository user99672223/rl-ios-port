/* Format tables, key header, and the .bc cache file. */
#include "rltex.h"

#include <stdlib.h>
#include <string.h>

const char *rltex_status_string(int s)
{
	switch (s) {
	case RLTEX_OK: return "ok";
	case RLTEX_E_ARG: return "bad argument";
	case RLTEX_E_FORMAT: return "not a supported BC DXGI format";
	case RLTEX_E_SIZE: return "blob size does not match format and dimensions";
	case RLTEX_E_NOMEM: return "out of memory";
	case RLTEX_E_IO: return "I/O error";
	case RLTEX_E_CORRUPT: return "corrupt cache file";
	case RLTEX_E_ASTC: return "astc-encoder error";
	case RLTEX_E_UNSUPPORTED: return "no ASTC mapping for this format";
	default: return "unknown status";
	}
}

int rltex_bc_info_for_dxgi(uint32_t f, rltex_bc_info *o)
{
	if (!o) return RLTEX_E_ARG;
	memset(o, 0, sizeof *o);
	switch (f) {
	case RLTEX_DXGI_BC1_TYPELESS: case RLTEX_DXGI_BC1_UNORM:
		o->kind = RLTEX_BC1; o->block_bytes = 8; return RLTEX_OK;
	case RLTEX_DXGI_BC1_UNORM_SRGB:
		o->kind = RLTEX_BC1; o->block_bytes = 8; o->is_srgb = 1; return RLTEX_OK;
	case RLTEX_DXGI_BC2_TYPELESS: case RLTEX_DXGI_BC2_UNORM:
		o->kind = RLTEX_BC2; o->block_bytes = 16; return RLTEX_OK;
	case RLTEX_DXGI_BC2_UNORM_SRGB:
		o->kind = RLTEX_BC2; o->block_bytes = 16; o->is_srgb = 1; return RLTEX_OK;
	case RLTEX_DXGI_BC3_TYPELESS: case RLTEX_DXGI_BC3_UNORM:
		o->kind = RLTEX_BC3; o->block_bytes = 16; return RLTEX_OK;
	case RLTEX_DXGI_BC3_UNORM_SRGB:
		o->kind = RLTEX_BC3; o->block_bytes = 16; o->is_srgb = 1; return RLTEX_OK;
	case RLTEX_DXGI_BC4_TYPELESS: case RLTEX_DXGI_BC4_UNORM:
		o->kind = RLTEX_BC4; o->block_bytes = 8; return RLTEX_OK;
	case RLTEX_DXGI_BC4_SNORM:
		o->kind = RLTEX_BC4; o->block_bytes = 8; o->is_signed = 1; return RLTEX_OK;
	case RLTEX_DXGI_BC5_TYPELESS: case RLTEX_DXGI_BC5_UNORM:
		o->kind = RLTEX_BC5; o->block_bytes = 16; return RLTEX_OK;
	case RLTEX_DXGI_BC5_SNORM:
		o->kind = RLTEX_BC5; o->block_bytes = 16; o->is_signed = 1; return RLTEX_OK;
	case RLTEX_DXGI_BC6H_TYPELESS: case RLTEX_DXGI_BC6H_UF16:
		o->kind = RLTEX_BC6H; o->block_bytes = 16; return RLTEX_OK;
	case RLTEX_DXGI_BC6H_SF16:
		o->kind = RLTEX_BC6H; o->block_bytes = 16; o->is_signed = 1; return RLTEX_OK;
	case RLTEX_DXGI_BC7_TYPELESS: case RLTEX_DXGI_BC7_UNORM:
		o->kind = RLTEX_BC7; o->block_bytes = 16; return RLTEX_OK;
	case RLTEX_DXGI_BC7_UNORM_SRGB:
		o->kind = RLTEX_BC7; o->block_bytes = 16; o->is_srgb = 1; return RLTEX_OK;
	default: return RLTEX_E_FORMAT;
	}
}

const char *rltex_bc_kind_name(rltex_bc_kind k)
{
	switch (k) {
	case RLTEX_BC1: return "BC1";
	case RLTEX_BC2: return "BC2";
	case RLTEX_BC3: return "BC3";
	case RLTEX_BC4: return "BC4";
	case RLTEX_BC5: return "BC5";
	case RLTEX_BC6H: return "BC6H";
	case RLTEX_BC7: return "BC7";
	}
	return "?";
}

int rltex_bc_blob_size(uint32_t dxgi, uint32_t w, uint32_t h, size_t *out)
{
	rltex_bc_info info;
	int r = rltex_bc_info_for_dxgi(dxgi, &info);
	if (r) return r;
	if (!w || !h || !out) return RLTEX_E_ARG;
	*out = (size_t)((w + 3) / 4) * (size_t)((h + 3) / 4) * info.block_bytes;
	return RLTEX_OK;
}

size_t rltex_pixfmt_bytes(rltex_pixfmt f)
{
	switch (f) {
	case RLTEX_PIX_R8_UNORM: case RLTEX_PIX_R8_SNORM: return 1;
	case RLTEX_PIX_RG8_UNORM: case RLTEX_PIX_RG8_SNORM: return 2;
	case RLTEX_PIX_RGBA8_UNORM: return 4;
	case RLTEX_PIX_RGBA16_FLOAT: return 8;
	}
	return 0;
}

const char *rltex_pixfmt_name(rltex_pixfmt f)
{
	switch (f) {
	case RLTEX_PIX_R8_UNORM: return "R8_UNORM";
	case RLTEX_PIX_RG8_UNORM: return "RG8_UNORM";
	case RLTEX_PIX_RGBA8_UNORM: return "RGBA8_UNORM";
	case RLTEX_PIX_R8_SNORM: return "R8_SNORM";
	case RLTEX_PIX_RG8_SNORM: return "RG8_SNORM";
	case RLTEX_PIX_RGBA16_FLOAT: return "RGBA16_FLOAT";
	}
	return "?";
}

rltex_pixfmt rltex_decoded_pixfmt(const rltex_bc_info *i)
{
	switch (i->kind) {
	case RLTEX_BC4: return i->is_signed ? RLTEX_PIX_R8_SNORM : RLTEX_PIX_R8_UNORM;
	case RLTEX_BC5: return i->is_signed ? RLTEX_PIX_RG8_SNORM : RLTEX_PIX_RG8_UNORM;
	case RLTEX_BC6H: return RLTEX_PIX_RGBA16_FLOAT;
	default: return RLTEX_PIX_RGBA8_UNORM;
	}
}

/* ---- key ------------------------------------------------------------------------------- */

static void put32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

static uint32_t get32(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void put64(uint8_t *p, uint64_t v)
{
	put32(p, (uint32_t)v);
	put32(p + 4, (uint32_t)(v >> 32));
}

static uint64_t get64(const uint8_t *p)
{
	return (uint64_t)get32(p) | ((uint64_t)get32(p + 4) << 32);
}

void rltex_key_header(const rltex_desc *d, uint64_t blob_size, uint8_t out[RLTEX_KEY_HEADER_SIZE])
{
	out[0] = 'R'; out[1] = 'L'; out[2] = 'K'; out[3] = '1';
	put32(out + 4, d->dxgi_format);
	put32(out + 8, d->width);
	put32(out + 12, d->height);
	put32(out + 16, d->mip_level);
	put32(out + 20, d->array_slice);
	put64(out + 24, blob_size);
}

void rltex_key_compute(const rltex_desc *d, const void *blob, size_t blob_size,
                       char out_hex[RLTEX_KEY_HEX_SIZE])
{
	static const char hex[] = "0123456789abcdef";
	uint8_t hdr[RLTEX_KEY_HEADER_SIZE], dig[32];
	rltex_sha256_ctx c;
	rltex_key_header(d, (uint64_t)blob_size, hdr);
	rltex_sha256_init(&c);
	rltex_sha256_update(&c, hdr, sizeof hdr);
	rltex_sha256_update(&c, blob, blob_size);
	rltex_sha256_final(&c, dig);
	for (int i = 0; i < 32; i++) {
		out_hex[2 * i] = hex[dig[i] >> 4];
		out_hex[2 * i + 1] = hex[dig[i] & 15];
	}
	out_hex[64] = 0;
}

/* ---- .bc file ---------------------------------------------------------------------------
 *   0  "RLBC"
 *   4  u32le version (1)
 *   8  key header (32 bytes, identical to the bytes hashed to form the key)
 *  40  blob (blob_size bytes, taken from the key header)
 */
int rltex_bc_file_build(const rltex_desc *d, const void *blob, size_t blob_size, uint8_t **out,
                        size_t *out_size)
{
	size_t expect;
	uint8_t *f;
	int r = rltex_bc_blob_size(d->dxgi_format, d->width, d->height, &expect);
	if (r) return r;
	if (expect != blob_size) return RLTEX_E_SIZE;
	f = (uint8_t *)malloc(RLTEX_BC_FILE_PREFIX_SIZE + RLTEX_KEY_HEADER_SIZE + blob_size);
	if (!f) return RLTEX_E_NOMEM;
	memcpy(f, "RLBC", 4);
	put32(f + 4, 1);
	rltex_key_header(d, blob_size, f + RLTEX_BC_FILE_PREFIX_SIZE);
	memcpy(f + RLTEX_BC_FILE_PREFIX_SIZE + RLTEX_KEY_HEADER_SIZE, blob, blob_size);
	*out = f;
	*out_size = RLTEX_BC_FILE_PREFIX_SIZE + RLTEX_KEY_HEADER_SIZE + blob_size;
	return RLTEX_OK;
}

int rltex_bc_file_parse(const uint8_t *f, size_t n, rltex_desc *d, const uint8_t **blob,
                        size_t *blob_size)
{
	const uint8_t *k = f + RLTEX_BC_FILE_PREFIX_SIZE;
	uint64_t sz;
	size_t expect;
	if (n < RLTEX_BC_FILE_PREFIX_SIZE + RLTEX_KEY_HEADER_SIZE) return RLTEX_E_CORRUPT;
	if (memcmp(f, "RLBC", 4) != 0 || get32(f + 4) != 1) return RLTEX_E_CORRUPT;
	if (memcmp(k, "RLK1", 4) != 0) return RLTEX_E_CORRUPT;
	d->dxgi_format = get32(k + 4);
	d->width = get32(k + 8);
	d->height = get32(k + 12);
	d->mip_level = get32(k + 16);
	d->array_slice = get32(k + 20);
	sz = get64(k + 24);
	if (rltex_bc_blob_size(d->dxgi_format, d->width, d->height, &expect) != RLTEX_OK) return RLTEX_E_CORRUPT;
	if (sz != expect || n != RLTEX_BC_FILE_PREFIX_SIZE + RLTEX_KEY_HEADER_SIZE + expect) return RLTEX_E_CORRUPT;
	*blob = f + RLTEX_BC_FILE_PREFIX_SIZE + RLTEX_KEY_HEADER_SIZE;
	*blob_size = expect;
	return RLTEX_OK;
}
