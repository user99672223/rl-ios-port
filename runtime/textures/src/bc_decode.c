/* BC1-BC7 -> uncompressed pixels via bcdec (external/bcdec, pinned 80859ed, v0.985).
 *
 * bcdec API notes, verified in external/bcdec/bcdec.h:
 *  - destinationPitch is in bytes for the 8-bit decoders (bcdec__color_block advances a
 *    `unsigned char*`), and in ELEMENTS for bcdec_bc6h_half (it advances an `unsigned short*`).
 *  - With BCDEC_BC4BC5_PRECISE the signatures become bcdec_bc4/bc5(..., pitch, isSigned) and the
 *    interpolation uses the fixed-point weights (bcdec.h:273-334) instead of integer division.
 *  - BC1 in 3-colour mode (c0 <= c1) writes index 3 as 0x00000000 (transparent black).
 * Each block is decoded to a small tile, then copied with cropping, so non-multiple-of-4 sizes
 * need no special casing.
 */
#include "rltex.h"

#include <string.h>

#define BCDEC_STATIC
#define BCDEC_BC4BC5_PRECISE
#define BCDEC_IMPLEMENTATION
#include "bcdec.h"

#define HALF_ONE 0x3C00u

int rltex_bc_decode(const rltex_desc *d, const void *blob, size_t blob_size, void *out,
                    size_t out_cap, rltex_pixfmt *out_fmt)
{
	rltex_bc_info info;
	size_t expect;
	int r;
	rltex_pixfmt fmt;
	size_t bpp;
	uint32_t bw, bh;

	if (!d || !blob || !out) return RLTEX_E_ARG;
	r = rltex_bc_info_for_dxgi(d->dxgi_format, &info);
	if (r) return r;
	r = rltex_bc_blob_size(d->dxgi_format, d->width, d->height, &expect);
	if (r) return r;
	if (blob_size != expect) return RLTEX_E_SIZE;
	fmt = rltex_decoded_pixfmt(&info);
	bpp = rltex_pixfmt_bytes(fmt);
	if (out_cap < (size_t)d->width * d->height * bpp) return RLTEX_E_ARG;
	if (out_fmt) *out_fmt = fmt;

	bw = (d->width + 3) / 4;
	bh = (d->height + 3) / 4;
	{
		const uint8_t *src = (const uint8_t *)blob;
		uint8_t *dst = (uint8_t *)out;
		size_t row_bytes = (size_t)d->width * bpp;
		for (uint32_t by = 0; by < bh; by++) {
			for (uint32_t bx = 0; bx < bw; bx++) {
				_Alignas(8) uint8_t tile[16 * 8];
				unsigned short half[16 * 3];
				const void *tsrc = tile;
				uint32_t cw = d->width - bx * 4 < 4 ? d->width - bx * 4 : 4;
				uint32_t ch = d->height - by * 4 < 4 ? d->height - by * 4 : 4;
				switch (info.kind) {
				case RLTEX_BC1: bcdec_bc1(src, tile, 4 * 4); break;
				case RLTEX_BC2: bcdec_bc2(src, tile, 4 * 4); break;
				case RLTEX_BC3: bcdec_bc3(src, tile, 4 * 4); break;
				case RLTEX_BC4: bcdec_bc4(src, tile, 4, info.is_signed); break;
				case RLTEX_BC5: bcdec_bc5(src, tile, 4 * 2, info.is_signed); break;
				case RLTEX_BC7: bcdec_bc7(src, tile, 4 * 4); break;
				case RLTEX_BC6H: {
					uint16_t *t16 = (uint16_t *)(void *)tile;
					bcdec_bc6h_half(src, half, 4 * 3, info.is_signed);
					for (int i = 0; i < 16; i++) {
						t16[4 * i + 0] = half[3 * i + 0];
						t16[4 * i + 1] = half[3 * i + 1];
						t16[4 * i + 2] = half[3 * i + 2];
						t16[4 * i + 3] = HALF_ONE;
					}
					break;
				}
				}
				src += info.block_bytes;
				for (uint32_t y = 0; y < ch; y++) {
					memcpy(dst + (size_t)(by * 4 + y) * row_bytes + (size_t)bx * 4 * bpp,
					       (const uint8_t *)tsrc + (size_t)y * 4 * bpp, (size_t)cw * bpp);
				}
			}
		}
	}
	return RLTEX_OK;
}

int rltex_expand_to_rgba8(rltex_pixfmt fmt, const void *src, uint32_t w, uint32_t h, uint8_t *dst)
{
	const uint8_t *s = (const uint8_t *)src;
	size_t n = (size_t)w * h;
	switch (fmt) {
	case RLTEX_PIX_RGBA8_UNORM: memcpy(dst, s, n * 4); return RLTEX_OK;
	case RLTEX_PIX_R8_UNORM:
		for (size_t i = 0; i < n; i++) {
			dst[4 * i] = s[i]; dst[4 * i + 1] = 0; dst[4 * i + 2] = 0; dst[4 * i + 3] = 255;
		}
		return RLTEX_OK;
	case RLTEX_PIX_RG8_UNORM:
		for (size_t i = 0; i < n; i++) {
			dst[4 * i] = s[2 * i]; dst[4 * i + 1] = s[2 * i + 1]; dst[4 * i + 2] = 0; dst[4 * i + 3] = 255;
		}
		return RLTEX_OK;
	default: return RLTEX_E_UNSUPPORTED;
	}
}
