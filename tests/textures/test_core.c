/* Core tests: SHA-256, key header/key golden values, format tables, hand-built BC blocks with
 * exactly known decoded colours, config parsing, BC->ASTC mapping, .astc header, .bc file.
 * Synthetic data only. */
#define _XOPEN_SOURCE 700
#include "astcenc.h"
#include "rltex.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int g_checks, g_fail;
#define CHECK(c)                                                                         \
	do {                                                                                 \
		g_checks++;                                                                      \
		if (!(c)) {                                                                      \
			g_fail++;                                                                    \
			fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);                 \
		}                                                                                \
	} while (0)

static void hex(const uint8_t *d, size_t n, char *out)
{
	for (size_t i = 0; i < n; i++) sprintf(out + 2 * i, "%02x", d[i]);
}

static void sha_hex(const void *data, size_t n, char out[65])
{
	uint8_t d[32];
	rltex_sha256(data, n, d);
	hex(d, 32, out);
}

static void test_sha256(void)
{
	char h[65];
	/* NIST FIPS 180-4 / CAVS example messages */
	sha_hex("", 0, h);
	CHECK(!strcmp(h, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
	sha_hex("abc", 3, h);
	CHECK(!strcmp(h, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
	{
		const char *m = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
		sha_hex(m, strlen(m), h);
		CHECK(!strcmp(h, "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"));
	}
	{
		/* one million 'a', fed in awkward chunk sizes to exercise buffering */
		rltex_sha256_ctx c;
		uint8_t d[32], chunk[1000];
		size_t left = 1000000, sizes[] = {1, 63, 64, 65, 127, 1000, 3};
		size_t si = 0;
		memset(chunk, 'a', sizeof chunk);
		rltex_sha256_init(&c);
		while (left) {
			size_t n = sizes[si++ % 7];
			if (n > left) n = left;
			rltex_sha256_update(&c, chunk, n);
			left -= n;
		}
		rltex_sha256_final(&c, d);
		hex(d, 32, h);
		CHECK(!strcmp(h, "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0"));
	}
	/* padding boundary lengths 55,56,57,63,64 against known hashes computed with hashlib */
	{
		static const struct { size_t n; const char *h; } v[] = {
			{55, "9f4390f8d30c2dd92ec9f095b65e2b9ae9b0a925a5258e241c9f1e910f734318"},
			{56, "b35439a4ac6f0948b6d6f9e3c6af0f5f590ce20f1bde7090ef7970686ec6738a"},
			{57, "f13b2d724659eb3bf47f2dd6af1accc87b81f09f59f2b75e5c0bed6589dfe8c6"},
			{63, "7d3e74a05d7db15bce4ad9ec0658ea98e3f06eeecf16b4c6fff2da457ddc2f34"},
			{64, "ffe054fe7ae0cb6dc65c3af9b61d5209f439851db43d0ba5997337df154668eb"}};
		char buf[80];
		memset(buf, 'a', sizeof buf);
		for (size_t i = 0; i < 5; i++) {
			sha_hex(buf, v[i].n, h);
			CHECK(!strcmp(h, v[i].h));
		}
	}
}

static void test_key(void)
{
	/* Golden values computed independently with Python hashlib/struct (see docs/notes). */
	rltex_desc d = {RLTEX_DXGI_BC1_UNORM, 8, 4, 0, 0};
	uint8_t hdr[RLTEX_KEY_HEADER_SIZE], blob[16], blob2[256];
	char hh[65], key[RLTEX_KEY_HEX_SIZE];
	rltex_key_header(&d, 16, hdr);
	hex(hdr, 32, hh);
	CHECK(!strcmp(hh, "524c4b3147000000080000000400000000000000000000001000000000000000"));
	for (int i = 0; i < 16; i++) blob[i] = (uint8_t)i;
	rltex_key_compute(&d, blob, 16, key);
	CHECK(!strcmp(key, "7a4946eadfd85f1cb288b73b7d5263cf0308dc4c9f5c66aa5ee59d14628b69c6"));

	{
		rltex_desc d2 = {RLTEX_DXGI_BC3_UNORM, 16, 16, 3, 5};
		for (int i = 0; i < 256; i++) blob2[i] = (uint8_t)(i * 7 + 3);
		rltex_key_compute(&d2, blob2, 256, key);
		CHECK(!strcmp(key, "9f0b7f6a22c6640ab7bdd141d9ac78a3272b0e9cc0274ee51b93b7ca432e00f2"));
		/* every header field participates in the key */
		{
			char k2[RLTEX_KEY_HEX_SIZE];
			rltex_desc v;
			v = d2; v.dxgi_format = RLTEX_DXGI_BC3_UNORM_SRGB; rltex_key_compute(&v, blob2, 256, k2); CHECK(strcmp(k2, key));
			v = d2; v.mip_level = 4; rltex_key_compute(&v, blob2, 256, k2); CHECK(strcmp(k2, key));
			v = d2; v.array_slice = 6; rltex_key_compute(&v, blob2, 256, k2); CHECK(strcmp(k2, key));
			v = d2; v.width = 17; rltex_key_compute(&v, blob2, 256, k2); CHECK(strcmp(k2, key));
			v = d2; v.height = 17; rltex_key_compute(&v, blob2, 256, k2); CHECK(strcmp(k2, key));
			blob2[255] ^= 1; rltex_key_compute(&d2, blob2, 256, k2); CHECK(strcmp(k2, key));
		}
		/* same input twice -> same key (stability) */
		{
			char a[RLTEX_KEY_HEX_SIZE], b[RLTEX_KEY_HEX_SIZE];
			rltex_key_compute(&d, blob, 16, a);
			rltex_key_compute(&d, blob, 16, b);
			CHECK(!strcmp(a, b));
		}
	}
}

static void test_formats(void)
{
	static const struct { uint32_t f; rltex_bc_kind k; int srgb, sgn; uint32_t bytes; } t[] = {
		{0x46, RLTEX_BC1, 0, 0, 8},  {0x47, RLTEX_BC1, 0, 0, 8},  {0x48, RLTEX_BC1, 1, 0, 8},
		{0x49, RLTEX_BC2, 0, 0, 16}, {0x4a, RLTEX_BC2, 0, 0, 16}, {0x4b, RLTEX_BC2, 1, 0, 16},
		{0x4c, RLTEX_BC3, 0, 0, 16}, {0x4d, RLTEX_BC3, 0, 0, 16}, {0x4e, RLTEX_BC3, 1, 0, 16},
		{0x4f, RLTEX_BC4, 0, 0, 8},  {0x50, RLTEX_BC4, 0, 0, 8},  {0x51, RLTEX_BC4, 0, 1, 8},
		{0x52, RLTEX_BC5, 0, 0, 16}, {0x53, RLTEX_BC5, 0, 0, 16}, {0x54, RLTEX_BC5, 0, 1, 16},
		{0x5e, RLTEX_BC6H, 0, 0, 16}, {0x5f, RLTEX_BC6H, 0, 0, 16}, {0x60, RLTEX_BC6H, 0, 1, 16},
		{0x61, RLTEX_BC7, 0, 0, 16}, {0x62, RLTEX_BC7, 0, 0, 16}, {0x63, RLTEX_BC7, 1, 0, 16}};
	rltex_bc_info i;
	size_t sz;
	for (size_t n = 0; n < sizeof t / sizeof t[0]; n++) {
		CHECK(rltex_bc_info_for_dxgi(t[n].f, &i) == RLTEX_OK);
		CHECK(i.kind == t[n].k && i.is_srgb == t[n].srgb && i.is_signed == t[n].sgn &&
		      i.block_bytes == t[n].bytes);
	}
	CHECK(rltex_bc_info_for_dxgi(28 /* R8G8B8A8_UNORM */, &i) == RLTEX_E_FORMAT);
	CHECK(rltex_bc_info_for_dxgi(0, &i) == RLTEX_E_FORMAT);
	CHECK(rltex_bc_info_for_dxgi(0x55, &i) == RLTEX_E_FORMAT);
	CHECK(rltex_bc_blob_size(RLTEX_DXGI_BC1_UNORM, 4, 4, &sz) == 0 && sz == 8);
	CHECK(rltex_bc_blob_size(RLTEX_DXGI_BC1_UNORM, 5, 5, &sz) == 0 && sz == 32);
	CHECK(rltex_bc_blob_size(RLTEX_DXGI_BC1_UNORM, 1, 1, &sz) == 0 && sz == 8);
	CHECK(rltex_bc_blob_size(RLTEX_DXGI_BC7_UNORM, 256, 128, &sz) == 0 && sz == 64 * 32 * 16);
	CHECK(rltex_bc_blob_size(RLTEX_DXGI_BC7_UNORM, 0, 128, &sz) == RLTEX_E_ARG);
}

/* ---- hand-built blocks -------------------------------------------------------------- */

typedef struct { uint8_t r, g, b, a; } px;

static void dec8(uint32_t fmt, uint32_t w, uint32_t h, const uint8_t *blob, size_t bn, uint8_t *out,
                 size_t cap, rltex_pixfmt *pf)
{
	rltex_desc d = {fmt, w, h, 0, 0};
	int r = rltex_bc_decode(&d, blob, bn, out, cap, pf);
	CHECK(r == RLTEX_OK);
}

static void pack3(uint8_t *dst, const uint8_t idx[16])
{
	uint64_t bits = 0;
	for (int i = 0; i < 16; i++) bits |= (uint64_t)(idx[i] & 7) << (3 * i);
	for (int i = 0; i < 6; i++) dst[i] = (uint8_t)(bits >> (8 * i));
}

static void test_bc1_hand(void)
{
	/* Block A: 4-colour mode, c0 = 0xF800 (255,0,0) > c1 = 0x001F (0,0,255).
	 * D3D11 block-compression spec: c2 = 2/3 c0 + 1/3 c1 = (170,0,85), c3 = (85,0,170). */
	const uint8_t A[8] = {0x00, 0xF8, 0x1F, 0x00, 0xE4, 0x1B, 0x50, 0xFA};
	/* Block B: 3-colour mode (c0 = 0x0000 < c1 = 0x8410): c1 expands to (132,130,132),
	 * c2 = midpoint (66,65,66) and index 3 is transparent black. */
	const uint8_t B[8] = {0x00, 0x00, 0x10, 0x84, 0xE4, 0xAA, 0xFF, 0x55};
	px o[16];
	rltex_pixfmt pf;
	static const px red = {255, 0, 0, 255}, blue = {0, 0, 255, 255}, m1 = {170, 0, 85, 255},
	                m2 = {85, 0, 170, 255};
	/* expected index per pixel from the index bytes: row0 0,1,2,3  row1 3,2,1,0 ... */
	const px *exp[16] = {&red, &blue, &m1, &m2, &m2, &m1, &blue, &red,
	                     &red, &red,  &blue, &blue, &m1, &m1, &m2, &m2};
	dec8(RLTEX_DXGI_BC1_UNORM, 4, 4, A, 8, (uint8_t *)o, sizeof o, &pf);
	CHECK(pf == RLTEX_PIX_RGBA8_UNORM);
	for (int i = 0; i < 16; i++) CHECK(!memcmp(&o[i], exp[i], 4));

	dec8(RLTEX_DXGI_BC1_UNORM, 4, 4, B, 8, (uint8_t *)o, sizeof o, &pf);
	{
		static const px c0 = {0, 0, 0, 255}, c1 = {132, 130, 132, 255}, c2 = {66, 65, 66, 255},
		                t = {0, 0, 0, 0};
		CHECK(!memcmp(&o[0], &c0, 4) && !memcmp(&o[1], &c1, 4) && !memcmp(&o[2], &c2, 4) &&
		      !memcmp(&o[3], &t, 4));
		for (int i = 4; i < 8; i++) CHECK(!memcmp(&o[i], &c2, 4));
		for (int i = 8; i < 12; i++) CHECK(!memcmp(&o[i], &t, 4));
		for (int i = 12; i < 16; i++) CHECK(!memcmp(&o[i], &c1, 4));
	}
	/* sRGB variant decodes identically (conversion is the sampler's job) */
	{
		px s[16];
		dec8(RLTEX_DXGI_BC1_UNORM_SRGB, 4, 4, A, 8, (uint8_t *)s, sizeof s, &pf);
		dec8(RLTEX_DXGI_BC1_UNORM, 4, 4, A, 8, (uint8_t *)o, sizeof o, &pf);
		CHECK(!memcmp(s, o, sizeof s));
	}
}

static void test_crop(void)
{
	/* 5x3 -> 2x1 blocks, both solid red; output buffer is exactly 5*3*4 with guard bytes. */
	const uint8_t blob[16] = {0x00, 0xF8, 0x00, 0xF8, 0, 0, 0, 0, 0x00, 0xF8, 0x00, 0xF8, 0, 0, 0, 0};
	uint8_t buf[5 * 3 * 4 + 16];
	rltex_desc d = {RLTEX_DXGI_BC1_UNORM, 5, 3, 0, 0};
	rltex_pixfmt pf;
	memset(buf, 0xAB, sizeof buf);
	CHECK(rltex_bc_decode(&d, blob, 16, buf, 5 * 3 * 4, &pf) == RLTEX_OK);
	for (int i = 0; i < 15; i++) {
		CHECK(buf[4 * i] == 255 && buf[4 * i + 1] == 0 && buf[4 * i + 2] == 0 && buf[4 * i + 3] == 255);
	}
	for (size_t i = 60; i < sizeof buf; i++) CHECK(buf[i] == 0xAB);
	/* error paths */
	CHECK(rltex_bc_decode(&d, blob, 8, buf, sizeof buf, &pf) == RLTEX_E_SIZE);
	CHECK(rltex_bc_decode(&d, blob, 16, buf, 59, &pf) == RLTEX_E_ARG);
	d.dxgi_format = 28;
	CHECK(rltex_bc_decode(&d, blob, 16, buf, sizeof buf, &pf) == RLTEX_E_FORMAT);
}

static void test_bc2_bc3_bc4_bc5(void)
{
	/* colour part for BC2/BC3: c0 = 0x0000, c1 = 0xF81F (255,0,255); always 4-colour mode:
	 * x0 (0,0,0)  x1 (255,0,255)  x2 (85,0,85)  x3 (170,0,170), every row indices 0,1,2,3. */
	const uint8_t color[8] = {0x00, 0x00, 0x1F, 0xF8, 0xE4, 0xE4, 0xE4, 0xE4};
	static const uint8_t cx[4][3] = {{0, 0, 0}, {255, 0, 255}, {85, 0, 85}, {170, 0, 170}};
	uint8_t blk[16];
	px o[16];
	rltex_pixfmt pf;

	/* BC2: explicit 4-bit alpha, nibble n -> 17*n */
	{
		static const uint8_t ya[4][4] = {{0, 85, 170, 255}, {255, 255, 255, 255},
		                                 {17, 17, 17, 17}, {136, 136, 136, 136}};
		memcpy(blk, (uint8_t[8]){0x50, 0xFA, 0xFF, 0xFF, 0x11, 0x11, 0x88, 0x88}, 8);
		memcpy(blk + 8, color, 8);
		dec8(RLTEX_DXGI_BC2_UNORM, 4, 4, blk, 16, (uint8_t *)o, sizeof o, &pf);
		for (int y = 0; y < 4; y++)
			for (int x = 0; x < 4; x++) {
				const px *p = &o[y * 4 + x];
				CHECK(p->r == cx[x][0] && p->g == cx[x][1] && p->b == cx[x][2] && p->a == ya[y][x]);
			}
	}
	/* BC3: alpha a0=252 > a1=0 -> 6 interpolants 216,180,144,108,72,36 (exact: 252*k/7) */
	{
		static const uint8_t t6[8] = {252, 0, 216, 180, 144, 108, 72, 36};
		uint8_t idx[16];
		blk[0] = 252; blk[1] = 0;
		for (int i = 0; i < 16; i++) idx[i] = (uint8_t)(i % 8);
		pack3(blk + 2, idx);
		memcpy(blk + 8, color, 8);
		dec8(RLTEX_DXGI_BC3_UNORM, 4, 4, blk, 16, (uint8_t *)o, sizeof o, &pf);
		for (int i = 0; i < 16; i++) CHECK(o[i].a == t6[i % 8] && o[i].r == cx[i % 4][0]);
	}
	/* BC3: a0=0 <= a1=50 -> 4 interpolants 10,20,30,40 then 0 and 255 */
	{
		static const uint8_t t4[8] = {0, 50, 10, 20, 30, 40, 0, 255};
		uint8_t idx[16];
		blk[0] = 0; blk[1] = 50;
		for (int i = 0; i < 16; i++) idx[i] = (uint8_t)(i % 8);
		pack3(blk + 2, idx);
		memcpy(blk + 8, color, 8);
		dec8(RLTEX_DXGI_BC3_UNORM, 4, 4, blk, 16, (uint8_t *)o, sizeof o, &pf);
		for (int i = 0; i < 16; i++) CHECK(o[i].a == t4[i % 8]);
	}
	/* BC4 UNORM (R8) with the same two alpha tables */
	{
		static const uint8_t t6[8] = {252, 0, 216, 180, 144, 108, 72, 36};
		uint8_t idx[16], r[16];
		blk[0] = 252; blk[1] = 0;
		for (int i = 0; i < 16; i++) idx[i] = (uint8_t)(i % 8);
		pack3(blk + 2, idx);
		dec8(RLTEX_DXGI_BC4_UNORM, 4, 4, blk, 8, r, sizeof r, &pf);
		CHECK(pf == RLTEX_PIX_R8_UNORM);
		for (int i = 0; i < 16; i++) CHECK(r[i] == t6[i % 8]);
	}
	/* BC5 UNORM (RG8): R from the 6-interpolant block, G from the 4-interpolant block */
	{
		static const uint8_t t6[8] = {252, 0, 216, 180, 144, 108, 72, 36};
		static const uint8_t t4[8] = {0, 50, 10, 20, 30, 40, 0, 255};
		uint8_t idx[16], rg[32];
		blk[0] = 252; blk[1] = 0;
		blk[8] = 0; blk[9] = 50;
		for (int i = 0; i < 16; i++) idx[i] = (uint8_t)(7 - i % 8);
		pack3(blk + 2, idx);
		pack3(blk + 10, idx);
		dec8(RLTEX_DXGI_BC5_UNORM, 4, 4, blk, 16, rg, sizeof rg, &pf);
		CHECK(pf == RLTEX_PIX_RG8_UNORM);
		for (int i = 0; i < 16; i++) CHECK(rg[2 * i] == t6[7 - i % 8] && rg[2 * i + 1] == t4[7 - i % 8]);
	}
	/* BC4 SNORM (R8_SNORM): a0=70 > a1=0 -> 70,0,60,50,40,30,20,10 ;
	 * a0=-128 clamps to -127 (bcdec.h:289) and equal endpoints take the 4-interp mode:
	 * index 6 -> -127, index 7 -> 127 */
	{
		static const int8_t s6[8] = {70, 0, 60, 50, 40, 30, 20, 10};
		uint8_t idx[16];
		int8_t r[16];
		blk[0] = 70; blk[1] = 0;
		for (int i = 0; i < 16; i++) idx[i] = (uint8_t)(i % 8);
		pack3(blk + 2, idx);
		dec8(RLTEX_DXGI_BC4_SNORM, 4, 4, blk, 8, (uint8_t *)r, sizeof r, &pf);
		CHECK(pf == RLTEX_PIX_R8_SNORM);
		for (int i = 0; i < 16; i++) CHECK(r[i] == s6[i % 8]);
		blk[0] = 0x80; blk[1] = (uint8_t)(int8_t)-127;
		dec8(RLTEX_DXGI_BC4_SNORM, 4, 4, blk, 8, (uint8_t *)r, sizeof r, &pf);
		CHECK(r[0] == -127 && r[6] == -127 && r[7] == 127 && r[1] == -127);
	}
}

static void put_bits(uint8_t *b, int pos, int n, unsigned v)
{
	for (int i = 0; i < n; i++)
		if ((v >> i) & 1) b[(pos + i) / 8] |= (uint8_t)(1u << ((pos + i) % 8));
}

static void test_bc7_hand(void)
{
	/* Mode 6 (one subset, 7-bit RGBA endpoints + 1 p-bit each, 4-bit indices): the mode field is
	 * six 0 bits then a 1 (bit 6). All indices 0 -> every pixel equals endpoint 0.
	 * R0=100 G0=50 B0=25 A0=127, P0=1 -> (201,101,51,255). BC7 spec: endpoint = (v<<1)|p. */
	uint8_t blk[16] = {0};
	px o[16];
	rltex_pixfmt pf;
	put_bits(blk, 6, 1, 1);
	put_bits(blk, 7, 7, 100);   /* R0 */
	put_bits(blk, 14, 7, 0);    /* R1 */
	put_bits(blk, 21, 7, 50);   /* G0 */
	put_bits(blk, 35, 7, 25);   /* B0 */
	put_bits(blk, 49, 7, 127);  /* A0 */
	put_bits(blk, 63, 1, 1);    /* P0 */
	dec8(RLTEX_DXGI_BC7_UNORM, 4, 4, blk, 16, (uint8_t *)o, sizeof o, &pf);
	for (int i = 0; i < 16; i++) CHECK(o[i].r == 201 && o[i].g == 101 && o[i].b == 51 && o[i].a == 255);
}

static void test_bc6h_shape(void)
{
	/* An all-zero BC6H block is mode 0 with all endpoints 0 -> black; alpha must be 1.0 (0x3C00). */
	uint8_t blk[16] = {0};
	uint16_t o[16 * 4];
	rltex_pixfmt pf;
	rltex_desc d = {RLTEX_DXGI_BC6H_UF16, 4, 4, 0, 0};
	CHECK(rltex_bc_decode(&d, blk, 16, o, sizeof o, &pf) == RLTEX_OK);
	CHECK(pf == RLTEX_PIX_RGBA16_FLOAT);
	for (int i = 0; i < 16; i++) CHECK(o[4 * i] == 0 && o[4 * i + 1] == 0 && o[4 * i + 2] == 0 && o[4 * i + 3] == 0x3C00);
}

static void test_expand(void)
{
	const uint8_t r8[3] = {1, 2, 3}, rg8[6] = {1, 2, 3, 4, 5, 6};
	uint8_t o[12];
	CHECK(rltex_expand_to_rgba8(RLTEX_PIX_R8_UNORM, r8, 3, 1, o) == 0);
	CHECK(o[0] == 1 && o[1] == 0 && o[2] == 0 && o[3] == 255 && o[8] == 3);
	CHECK(rltex_expand_to_rgba8(RLTEX_PIX_RG8_UNORM, rg8, 3, 1, o) == 0);
	CHECK(o[4] == 3 && o[5] == 4 && o[6] == 0 && o[7] == 255);
	CHECK(rltex_expand_to_rgba8(RLTEX_PIX_R8_SNORM, r8, 3, 1, o) == RLTEX_E_UNSUPPORTED);
}

/* ---- config and mapping ------------------------------------------------------------- */

static void test_config(void)
{
	rltex_config c;
	char err[256];
	rltex_block_dim b;

	rltex_config_defaults(&c);
	/* spec 6.5 defaults */
	CHECK(c.bc1.x == 6 && c.bc1.y == 6);
	CHECK(c.bc3.x == 4 && c.bc5.x == 4 && c.bc7.x == 4 && c.bc2.x == 4);
	CHECK(c.bc4.x == 6 && c.bc4.y == 6);
	CHECK(c.bc6h.x == 4 && c.bc6h_astc_hdr == 1);
	CHECK(c.encode_max_pixels == 2048u * 2048u && c.queue_capacity == 256 && c.background_encode == 1);
	CHECK(c.encode_qos == RLTEX_QOS_BACKGROUND && c.runtime_quality == RLTEX_QUALITY_FAST);

	CHECK(rltex_parse_block_string("6x6", &b) == 0 && b.x == 6 && b.y == 6);
	CHECK(rltex_parse_block_string("12x10", &b) == 0 && b.x == 12 && b.y == 10);
	CHECK(rltex_parse_block_string("10x5", &b) == 0 && b.x == 10 && b.y == 5);
	CHECK(rltex_parse_block_string("5x6", &b) != 0);   /* not a legal ASTC size */
	CHECK(rltex_parse_block_string("3x3", &b) != 0);
	CHECK(rltex_parse_block_string("6x6x6", &b) != 0); /* 3D sizes are not used */
	CHECK(rltex_parse_block_string("6 x 6", &b) != 0);
	CHECK(rltex_parse_block_string("", &b) != 0);

	/* spec 6.9 sample file */
	rltex_config_defaults(&c);
	CHECK(rltex_config_parse_toml(&c,
		"render_scale = 0.67\ndrawable_mode = \"auto\"\nmax_fps = 60\nthermal_scaling = true\n"
		"allow_aot_only = false\nastc_bc1_block = \"6x6\"\nastc_bc3_block = \"4x4\"\n"
		"log_level = \"warn\"\n", err, sizeof err) == 0);
	CHECK(c.bc1.x == 6 && c.bc3.x == 4);

	/* group key sets BC2/BC3/BC5/BC7; explicit overrides win regardless of order */
	rltex_config_defaults(&c);
	CHECK(rltex_config_parse_toml(&c, "astc_bc5_block = \"5x5\"  # inline comment\n"
	                              "astc_bc3_block = \"8x8\"\nastc_bc4_block=\"5x4\"\n"
	                              "astc_bc6h_mode = \"rgba16f\"\n", err, sizeof err) == 0);
	CHECK(c.bc3.x == 8 && c.bc2.x == 8 && c.bc7.x == 8 && c.bc5.x == 5 && c.bc5.y == 5);
	CHECK(c.bc4.x == 5 && c.bc4.y == 4 && c.bc6h_astc_hdr == 0 && c.bc1.x == 6);

	/* invalid values are rejected, others still applied, previous value kept */
	rltex_config_defaults(&c);
	CHECK(rltex_config_parse_toml(&c, "astc_bc1_block = \"7x7\"\nastc_bc7_block = \"10x10\"\n"
	                              "astc_runtime_quality = \"bogus\"\nastc_encode_qos = \"high\"\n",
	                              err, sizeof err) == 3);
	CHECK(c.bc1.x == 6 && c.bc7.x == 10 && c.runtime_quality == RLTEX_QUALITY_FAST);
	CHECK(strstr(err, "astc_bc1_block") && strstr(err, "7x7"));

	/* quality names and numbers; keys inside [tables] are ignored */
	rltex_config_defaults(&c);
	CHECK(rltex_config_parse_toml(&c, "astc_runtime_quality = \"medium\"\n[other]\nastc_bc1_block = \"4x4\"\n",
	                              err, sizeof err) == 0);
	CHECK(c.runtime_quality == RLTEX_QUALITY_MEDIUM && c.bc1.x == 6);
	CHECK(rltex_config_parse_toml(&c, "astc_runtime_quality = 42.5\nastc_encode_qos = \"utility\"\n"
	                              "astc_encode_queue = 8\nastc_background_encode = false\n", err, sizeof err) == 0);
	CHECK(c.runtime_quality == 42.5f && c.encode_qos == RLTEX_QOS_UTILITY && c.queue_capacity == 8 &&
	      c.background_encode == 0);
	CHECK(rltex_config_parse_toml(&c, "astc_encode_max_pixels = 1000000\n", err, sizeof err) == 0 &&
	      c.encode_max_pixels == 1000000u);
	CHECK(rltex_config_parse_toml(&c, "astc_encode_max_pixels = 0\n", err, sizeof err) == 1);

	/* the quality constants must equal astcenc.h's presets */
	CHECK(RLTEX_QUALITY_FASTEST == ASTCENC_PRE_FASTEST && RLTEX_QUALITY_FAST == ASTCENC_PRE_FAST &&
	      RLTEX_QUALITY_MEDIUM == ASTCENC_PRE_MEDIUM && RLTEX_QUALITY_THOROUGH == ASTCENC_PRE_THOROUGH &&
	      RLTEX_QUALITY_VERYTHOROUGH == ASTCENC_PRE_VERYTHOROUGH &&
	      RLTEX_QUALITY_EXHAUSTIVE == ASTCENC_PRE_EXHAUSTIVE);
}

static void test_mapping(void)
{
	static const struct {
		uint32_t f; int ok; unsigned bx; rltex_astc_profile prof; const char *esw, *ssw, *view;
	} t[] = {
		{0x47, 1, 6, RLTEX_ASTC_LDR, "rgba", "rgba", "rgba"},
		{0x48, 1, 6, RLTEX_ASTC_LDR_SRGB, "rgba", "rgba", "rgba"},
		{0x4a, 1, 4, RLTEX_ASTC_LDR, "rgba", "rgba", "rgba"},
		{0x4b, 1, 4, RLTEX_ASTC_LDR_SRGB, "rgba", "rgba", "rgba"},
		{0x4d, 1, 4, RLTEX_ASTC_LDR, "rgba", "rgba", "rgba"},
		{0x4e, 1, 4, RLTEX_ASTC_LDR_SRGB, "rgba", "rgba", "rgba"},
		{0x50, 1, 6, RLTEX_ASTC_LDR, "rrr1", "r", "r001"},
		{0x51, 0, 0, 0, "", "", ""},
		{0x53, 1, 4, RLTEX_ASTC_LDR, "rrrg", "ra", "ra01"},
		{0x54, 0, 0, 0, "", "", ""},
		{0x5f, 1, 4, RLTEX_ASTC_HDR_RGB_LDR_A, "rgba", "rgba", "rgba"},
		{0x60, 0, 0, 0, "", "", ""},
		{0x62, 1, 4, RLTEX_ASTC_LDR, "rgba", "rgba", "rgba"},
		{0x63, 1, 4, RLTEX_ASTC_LDR_SRGB, "rgba", "rgba", "rgba"}};
	rltex_config c;
	rltex_bc_info i;
	rltex_astc_params p;
	rltex_config_defaults(&c);
	for (size_t n = 0; n < sizeof t / sizeof t[0]; n++) {
		rltex_bc_info_for_dxgi(t[n].f, &i);
		int r = rltex_astc_params_for(&c, &i, &p);
		if (!t[n].ok) { CHECK(r == RLTEX_E_UNSUPPORTED); continue; }
		CHECK(r == RLTEX_OK);
		CHECK(p.block_x == t[n].bx && p.block_y == t[n].bx && p.profile == t[n].prof);
		CHECK(!strcmp(p.encode_swizzle, t[n].esw) && !strcmp(p.sample_weights, t[n].ssw) &&
		      !strcmp(p.view_swizzle, t[n].view));
		CHECK(p.hdr == (t[n].f == 0x5f));
	}
	/* BC6H -> RGBA16F mode disables ASTC */
	c.bc6h_astc_hdr = 0;
	rltex_bc_info_for_dxgi(0x5f, &i);
	CHECK(rltex_astc_params_for(&c, &i, &p) == RLTEX_E_UNSUPPORTED);
	CHECK(!strcmp(rltex_astc_profile_cli_flag(RLTEX_ASTC_LDR), "-cl"));
	CHECK(!strcmp(rltex_astc_profile_cli_flag(RLTEX_ASTC_LDR_SRGB), "-cs"));
	CHECK(!strcmp(rltex_astc_profile_cli_flag(RLTEX_ASTC_HDR_RGB_LDR_A), "-ch"));
}

static void test_astc_header_and_bcfile(void)
{
	uint8_t h[16];
	rltex_astc_header a = {6, 6, 1, 1920, 1080, 1}, b;
	rltex_astc_header_write(&a, h);
	/* FileFormat.md: magic 13 AB A1 5C, block xyz, then 24-bit LE dims */
	{
		static const uint8_t exp[16] = {0x13, 0xAB, 0xA1, 0x5C, 6, 6, 1, 0x80, 0x07, 0x00, 0x38, 0x04, 0x00, 1, 0, 0};
		CHECK(!memcmp(h, exp, 16));
	}
	CHECK(rltex_astc_header_parse(h, 16, &b) == 0 && b.dim_x == 1920 && b.dim_y == 1080 && b.block_x == 6);
	a.dim_x = 70000; /* needs the third byte */
	rltex_astc_header_write(&a, h);
	CHECK(h[7] == (70000 & 255) && h[8] == ((70000 >> 8) & 255) && h[9] == 1);
	CHECK(rltex_astc_header_parse(h, 16, &b) == 0 && b.dim_x == 70000);
	h[0] ^= 1;
	CHECK(rltex_astc_header_parse(h, 16, &b) == RLTEX_E_CORRUPT);
	CHECK(rltex_astc_header_parse(h, 15, &b) == RLTEX_E_CORRUPT);

	/* the .bc file */
	{
		rltex_desc d = {RLTEX_DXGI_BC3_UNORM, 8, 8, 2, 1}, d2;
		uint8_t blob[64], *f;
		const uint8_t *pb;
		size_t fn, bn;
		char k1[65];
		for (int i = 0; i < 64; i++) blob[i] = (uint8_t)(i * 3);
		CHECK(rltex_bc_file_build(&d, blob, 64, &f, &fn) == 0 && fn == 8 + 32 + 64);
		CHECK(!memcmp(f, "RLBC", 4) && f[4] == 1 && !memcmp(f + 8, "RLK1", 4));
		CHECK(rltex_bc_file_parse(f, fn, &d2, &pb, &bn) == 0);
		CHECK(!memcmp(&d, &d2, sizeof d) && bn == 64 && !memcmp(pb, blob, 64));
		/* the key is SHA-256 of bytes [8, end) of the .bc file */
		{
			uint8_t dig[32];
			char kh[65];
			rltex_sha256(f + 8, fn - 8, dig);
			hex(dig, 32, kh);
			rltex_key_compute(&d, blob, 64, k1);
			CHECK(!strcmp(kh, k1));
		}
		CHECK(rltex_bc_file_parse(f, fn - 1, &d2, &pb, &bn) == RLTEX_E_CORRUPT);
		f[4] = 2;
		CHECK(rltex_bc_file_parse(f, fn, &d2, &pb, &bn) == RLTEX_E_CORRUPT);
		f[4] = 1;
		f[8 + 4] = 28; /* not a BC format */
		CHECK(rltex_bc_file_parse(f, fn, &d2, &pb, &bn) == RLTEX_E_CORRUPT);
		free(f);
		CHECK(rltex_bc_file_build(&d, blob, 63, &f, &fn) == RLTEX_E_SIZE);
	}
}

static void test_fs(void)
{
	const char *base = getenv("TMPDIR");
	char dir[400], path[500], nested[500];
	snprintf(dir, sizeof dir, "%s/rltex-fs-XXXXXX", base && *base ? base : "/tmp");
	uint8_t *data;
	size_t n;
	CHECK(mkdtemp(dir) != NULL);
	snprintf(nested, sizeof nested, "%s/a/b/c", dir);
	CHECK(rltex_fs_mkdir_p(nested) == 0);
	CHECK(rltex_fs_mkdir_p(nested) == 0); /* idempotent */
	snprintf(path, sizeof path, "%s/a/b/c/f.bin", dir);
	CHECK(rltex_fs_write_atomic(path, "hello", 5) == 0);
	CHECK(rltex_fs_write_atomic(path, "hello!", 6) == 0);
	CHECK(rltex_fs_read_all(path, &data, &n) == 0 && n == 6 && !memcmp(data, "hello!", 6));
	free(data);
	CHECK(rltex_fs_exists(path) && !rltex_fs_exists(nested + 1));
	{
		char cmd[500];
		snprintf(cmd, sizeof cmd, "rm -rf '%s'", dir);
		CHECK(system(cmd) == 0);
	}
}

int main(void)
{
	test_sha256();
	test_key();
	test_formats();
	test_bc1_hand();
	test_crop();
	test_bc2_bc3_bc4_bc5();
	test_bc7_hand();
	test_bc6h_shape();
	test_expand();
	test_config();
	test_mapping();
	test_astc_header_and_bcfile();
	test_fs();
	printf("test_core: %d checks, %d failures\n", g_checks, g_fail);
	return g_fail ? 1 : 0;
}
