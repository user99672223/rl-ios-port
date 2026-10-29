/*
 * Host-side check of vectors.h against the project's decoder: every pattern / flat subresource
 * is decoded with rltex_bc_decode (bcdec) and compared with the independent Python reference
 * the D3D11 test uses as its expectation.
 *   cc -I runtime/textures/include -I tests/d3d11/bc_textures tests/d3d11/bc_textures/check_vectors.c \
 *      runtime/textures/src/{sha256,format,bc_decode,config,fs,astc_encode,ctx,platform}.c \
 *      -I external/bcdec -Iruntime/textures/src -lpthread -lm -o check_vectors
 * Prints one line per format and exits with the number of mismatching subresources.
 */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rltex.h"

enum {
	DXGI_FORMAT_BC1_UNORM = 0x47, DXGI_FORMAT_BC1_UNORM_SRGB = 0x48, DXGI_FORMAT_BC1_TYPELESS = 0x46,
	DXGI_FORMAT_BC2_UNORM = 0x4a, DXGI_FORMAT_BC3_UNORM = 0x4d, DXGI_FORMAT_BC4_UNORM = 0x50,
	DXGI_FORMAT_BC5_UNORM = 0x53, DXGI_FORMAT_BC6H_UF16 = 0x5f, DXGI_FORMAT_BC7_UNORM = 0x62,
	DXGI_FORMAT_BC7_UNORM_SRGB = 0x63
};
#include "vectors.h"

static float half_to_float(uint16_t h)
{
	int e = (h >> 10) & 31, m = h & 1023;
	float s = (h & 0x8000) ? -1.f : 1.f;
	if (e == 0) return s * ldexpf((float)m, -24);
	if (e == 31) return s * 65504.f;
	return s * ldexpf((float)(m + 1024), e - 25);
}

int main(void)
{
	int bad = 0, total = 0;
	for (unsigned f = 0; f < NUM_VEC_FORMATS; f++) {
		int worst = 0, fails = 0;
		for (int mode = 0; mode < 2; mode++)
			for (int v = 0; v < 3; v++)
				for (int mip = 0; mip < 4; mip++) {
					const SubVec *s = &g_vec[f][mode][v][mip];
					rltex_desc d;
					rltex_pixfmt pf;
					uint8_t pix[16 * 16 * 8];
					if (!s->blocks) continue;
					d.dxgi_format = g_formats[f].tex_format == DXGI_FORMAT_BC1_TYPELESS ? DXGI_FORMAT_BC1_UNORM : g_formats[f].tex_format;
					d.width = s->w; d.height = s->h; d.mip_level = mip; d.array_slice = 0;
					if (g_formats[f].srgb) continue; /* same blocks as the linear twin; gamma is checked on the GPU */
					int r = rltex_bc_decode(&d, s->blocks, s->blocks_size, pix, sizeof pix, &pf);
					total++;
					if (r) { printf("%s: decode failed %d\n", g_formats[f].name, r); bad++; continue; }
					int maxd = 0;
					for (unsigned y = 0; y < s->h; y++)
						for (unsigned x = 0; x < s->w; x++) {
							int got[4] = {0, 0, 0, 255};
							const uint8_t *e = s->expect + (y * s->w + x) * 4;
							switch (pf) {
							case RLTEX_PIX_RGBA8_UNORM: for (int c = 0; c < 4; c++) got[c] = pix[(y * s->w + x) * 4 + c]; break;
							case RLTEX_PIX_R8_UNORM: got[0] = pix[y * s->w + x]; break;
							case RLTEX_PIX_RG8_UNORM: got[0] = pix[(y * s->w + x) * 2]; got[1] = pix[(y * s->w + x) * 2 + 1]; break;
							case RLTEX_PIX_RGBA16_FLOAT:
								for (int c = 0; c < 3; c++) {
									uint16_t h; memcpy(&h, pix + ((y * s->w + x) * 4 + c) * 2, 2);
									float fv = half_to_float(h); fv = fv < 0 ? 0 : fv > 1 ? 1 : fv;
									got[c] = (int)floorf(fv * 255.f + 0.5f);
								}
								break;
							default: printf("unexpected pixfmt\n"); bad++;
							}
							for (int c = 0; c < 4; c++) { int dd = abs(got[c] - e[c]); if (dd > maxd) maxd = dd; }
						}
					if (maxd > worst) worst = maxd;
					if (maxd > 2) fails++;
				}
		if (g_formats[f].srgb) continue;
		printf("%-24s max |diff| %d  %s\n", g_formats[f].name, worst, fails ? "MISMATCH" : "ok");
		bad += fails;
	}
	printf("%d subresources checked, %d mismatches\n", total, bad);
	return bad;
}
