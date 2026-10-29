/* Cache hit/miss/path tests, miss log, background encode queue, worker priority, ASTC output
 * header/magic. Synthetic data only. */
#define _XOPEN_SOURCE 700
#include "astcenc.h"
#include "rltex.h"

#include <dirent.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
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

static uint32_t rng_state = 12345;
static uint32_t rnd(void)
{
	rng_state = rng_state * 1664525u + 1013904223u;
	return rng_state >> 8;
}

static void put_bits(uint8_t *b, int pos, int n, unsigned v)
{
	for (int i = 0; i < n; i++)
		if ((v >> i) & 1) b[(pos + i) / 8] |= (uint8_t)(1u << ((pos + i) % 8));
}

/* Builds a valid synthetic blob for a format. Random bytes are valid for BC1-BC5; BC7 uses mode 6
 * blocks with varying endpoints and indices; BC6H is all-zero (black). */
static uint8_t *make_blob(uint32_t fmt, uint32_t w, uint32_t h, size_t *n)
{
	rltex_bc_info info;
	uint8_t *b;
	size_t blocks;
	rltex_bc_info_for_dxgi(fmt, &info);
	rltex_bc_blob_size(fmt, w, h, n);
	b = (uint8_t *)calloc(*n, 1);
	blocks = *n / info.block_bytes;
	for (size_t i = 0; i < blocks; i++) {
		uint8_t *p = b + i * info.block_bytes;
		if (info.kind == RLTEX_BC7) {
			put_bits(p, 6, 1, 1);
			for (int e = 0; e < 8; e++) put_bits(p, 7 + 7 * e, 7, rnd() & 127);
			put_bits(p, 63, 2, rnd() & 3);
			for (int k = 65; k < 128; k += 7) put_bits(p, k, 7 < 128 - k ? 7 : 128 - k, rnd() & 127);
		} else if (info.kind != RLTEX_BC6H) {
			for (uint32_t k = 0; k < info.block_bytes; k++) p[k] = (uint8_t)rnd();
		}
	}
	return b;
}

static char *mk_root(void)
{
	const char *base = getenv("TMPDIR");
	char *t = (char *)malloc(512);
	snprintf(t, 512, "%s/rltex-cache-XXXXXX", base && *base ? base : "/tmp");
	if (!mkdtemp(t)) abort();
	return t;
}

static void rm_rf(const char *dir)
{
	char cmd[300];
	snprintf(cmd, sizeof cmd, "rm -rf '%s'", dir);
	if (system(cmd) != 0) abort();
}

static int count_lines(const char *path)
{
	FILE *f = fopen(path, "r");
	int n = 0, c;
	if (!f) return -1;
	while ((c = fgetc(f)) != EOF) n += (c == '\n');
	fclose(f);
	return n;
}

static int count_ext(const char *dir, const char *ext)
{
	DIR *d = opendir(dir);
	struct dirent *e;
	int n = 0;
	if (!d) return -1;
	while ((e = readdir(d))) {
		size_t l = strlen(e->d_name), x = strlen(ext);
		if (l > x && !strcmp(e->d_name + l - x, ext)) n++;
	}
	closedir(d);
	return n;
}

static double psnr_u8(const uint8_t *a, const uint8_t *b, size_t n)
{
	double se = 0;
	for (size_t i = 0; i < n; i++) {
		double d = (double)a[i] - (double)b[i];
		se += d * d;
	}
	if (se == 0) return 99.0;
	return 10.0 * log10(255.0 * 255.0 / (se / (double)n));
}

/* decode an ASTC payload with the astc-encoder library (LDR profiles) */
static int astc_decode_u8(const rltex_tex *t, uint8_t *rgba)
{
	struct astcenc_config cfg;
	struct astcenc_context *ctx;
	struct astcenc_image img;
	struct astcenc_swizzle swz = {ASTCENC_SWZ_R, ASTCENC_SWZ_G, ASTCENC_SWZ_B, ASTCENC_SWZ_A};
	void *slice = rgba;
	enum astcenc_profile pr = t->astc_profile == RLTEX_ASTC_LDR_SRGB ? ASTCENC_PRF_LDR_SRGB : ASTCENC_PRF_LDR;
	enum astcenc_error e;
	if (astcenc_config_init(pr, t->astc_block_x, t->astc_block_y, 1, ASTCENC_PRE_FAST,
	                        ASTCENC_FLG_DECOMPRESS_ONLY, &cfg) != ASTCENC_SUCCESS)
		return -1;
	if (astcenc_context_alloc(&cfg, 1, &ctx, NULL) != ASTCENC_SUCCESS) return -1;
	img.dim_x = t->width; img.dim_y = t->height; img.dim_z = 1;
	img.data_type = ASTCENC_TYPE_U8; img.data = &slice;
	e = astcenc_decompress_image(ctx, t->astc_data, t->astc_size, &img, &swz, 0);
	astcenc_context_free(ctx);
	return e == ASTCENC_SUCCESS ? 0 : -1;
}

static void test_miss_log_and_paths(void)
{
	char *root = mk_root();
	rltex_config cfg;
	rltex_ctx *c;
	rltex_tex t, t2;
	rltex_desc d = {RLTEX_DXGI_BC1_UNORM, 16, 16, 0, 0};
	size_t n;
	uint8_t *blob = make_blob(d.dxgi_format, 16, 16, &n);
	char key[RLTEX_KEY_HEX_SIZE], p1[300], p2[300], plog[300], expect[400];

	rltex_config_defaults(&cfg);
	cfg.background_encode = 0;
	CHECK(rltex_ctx_create(root, &cfg, &c) == 0);
	rltex_key_compute(&d, blob, n, key);

	CHECK(rltex_acquire(c, &d, blob, n, &t) == 0);
	CHECK(t.source == RLTEX_SRC_DECODED && !strcmp(t.key, key));
	CHECK(t.pixfmt == RLTEX_PIX_RGBA8_UNORM && t.pixels_size == 16 * 16 * 4 && !t.queued_for_encode);
	{
		uint8_t ref[16 * 16 * 4];
		rltex_pixfmt pf;
		CHECK(rltex_bc_decode(&d, blob, n, ref, sizeof ref, &pf) == 0);
		CHECK(!memcmp(ref, t.pixels, sizeof ref));
	}
	/* paths from spec: Documents/cache/textures/<key>.astc, .../bc/<key>.bc */
	CHECK(rltex_path_astc(c, key, p1, sizeof p1) == 0);
	snprintf(expect, sizeof expect, "%s/cache/textures/%s.astc", root, key);
	CHECK(!strcmp(p1, expect));
	CHECK(rltex_path_bc(c, key, p2, sizeof p2) == 0);
	snprintf(expect, sizeof expect, "%s/cache/textures/bc/%s.bc", root, key);
	CHECK(!strcmp(p2, expect));
	CHECK(rltex_path_astc(c, key, p1, 10) == RLTEX_E_ARG);
	CHECK(rltex_path_miss_log(c, plog, sizeof plog) == 0);
	snprintf(expect, sizeof expect, "%s/logs/texture_miss.log", root);
	CHECK(!strcmp(plog, expect));

	/* saved .bc parses back to the same texture */
	{
		uint8_t *f;
		size_t fn, bn;
		const uint8_t *pb;
		rltex_desc d2;
		CHECK(rltex_fs_read_all(p2, &f, &fn) == 0);
		CHECK(rltex_bc_file_parse(f, fn, &d2, &pb, &bn) == 0 && !memcmp(&d, &d2, sizeof d) &&
		      bn == n && !memcmp(pb, blob, n));
		free(f);
	}
	CHECK(count_lines(plog) == 1);
	{
		FILE *f = fopen(plog, "r");
		char line[256];
		snprintf(expect, sizeof expect, "%s,71,16,16,0,0,128,1\n", key);
		CHECK(f && fgets(line, sizeof line, f) && !strcmp(line, expect));
		if (f) fclose(f);
	}

	/* second miss for the same key: no second log line, .bc untouched */
	{
		struct stat s1, s2;
		stat(p2, &s1);
		CHECK(rltex_acquire(c, &d, blob, n, &t2) == 0 && t2.source == RLTEX_SRC_DECODED);
		stat(p2, &s2);
		CHECK(s1.st_mtime == s2.st_mtime && s1.st_ino == s2.st_ino);
		CHECK(count_lines(plog) == 1);
		rltex_tex_release(&t2);
	}
	/* a different mip level of the same bytes is a different texture -> new key, new line */
	{
		rltex_desc dm = d;
		dm.mip_level = 1;
		CHECK(rltex_acquire(c, &dm, blob, n, &t2) == 0 && strcmp(t2.key, key) != 0);
		snprintf(expect, sizeof expect, "%s/cache/textures/bc", root);
		CHECK(count_lines(plog) == 2 && count_ext(expect, ".bc") == 2);
		rltex_tex_release(&t2);
	}
	/* argument errors */
	CHECK(rltex_acquire(c, &d, blob, n - 1, &t2) == RLTEX_E_SIZE);
	{
		rltex_desc bad = d;
		bad.dxgi_format = 28;
		CHECK(rltex_acquire(c, &bad, blob, n, &t2) == RLTEX_E_FORMAT);
	}
	rltex_tex_release(&t);
	rltex_ctx_destroy(c);

	/* persistence: a new process (new ctx) re-logs the miss but does not rewrite the .bc */
	CHECK(rltex_ctx_create(root, &cfg, &c) == 0);
	CHECK(rltex_acquire(c, &d, blob, n, &t) == 0);
	{
		FILE *f = fopen(plog, "r");
		char line[256], last[256] = "";
		while (f && fgets(line, sizeof line, f)) strcpy(last, line);
		if (f) fclose(f);
		snprintf(expect, sizeof expect, "%s,71,16,16,0,0,128,0\n", key); /* bc_saved = 0: already there */
		CHECK(!strcmp(last, expect));
	}
	rltex_tex_release(&t);

	/* ---- hit: place an .astc (as the offline encoder would) ---- */
	{
		rltex_bc_info info;
		rltex_astc_params p;
		rltex_astc_encoder *e = rltex_astc_encoder_create();
		uint8_t *astc, *pix = (uint8_t *)malloc(16 * 16 * 4);
		size_t an;
		rltex_pixfmt pf;
		CHECK(e != NULL);
		rltex_bc_info_for_dxgi(d.dxgi_format, &info);
		rltex_astc_params_for(&cfg, &info, &p);
		CHECK(p.block_x == 6 && p.block_y == 6 && p.profile == RLTEX_ASTC_LDR);
		rltex_bc_decode(&d, blob, n, pix, 16 * 16 * 4, &pf);
		CHECK(rltex_astc_encoder_encode(e, &p, RLTEX_QUALITY_FAST, pix, 16, 16, &astc, &an) == 0);
		/* ASTC file header magic and layout (astc-encoder Docs/FileFormat.md) */
		CHECK(astc[0] == 0x13 && astc[1] == 0xAB && astc[2] == 0xA1 && astc[3] == 0x5C);
		CHECK(astc[4] == 6 && astc[5] == 6 && astc[6] == 1);
		CHECK(astc[7] == 16 && astc[8] == 0 && astc[9] == 0 && astc[10] == 16 && astc[13] == 1);
		CHECK(an == 16 + 3 * 3 * 16);
		rltex_path_astc(c, key, p1, sizeof p1);
		CHECK(rltex_fs_write_atomic(p1, astc, an) == 0);
		CHECK(rltex_acquire(c, &d, blob, n, &t) == 0);
		CHECK(t.source == RLTEX_SRC_ASTC_CACHE && t.pixels == NULL);
		CHECK(t.astc_block_x == 6 && t.astc_block_y == 6 && t.astc_size == 3 * 3 * 16);
		CHECK(!memcmp(t.astc_data, astc + 16, t.astc_size));
		CHECK(!strcmp(t.view_swizzle, "rgba") && t.astc_profile == RLTEX_ASTC_LDR);
		{
			uint8_t dec[16 * 16 * 4];
			double ps;
			CHECK(astc_decode_u8(&t, dec) == 0);
			ps = psnr_u8(dec, pix, sizeof dec);
			printf("  hit decode PSNR (random BC1 content, 6x6, fast): %.2f dB\n", ps);
			CHECK(ps > 18.0); /* random-bytes BC1 is worst-case content; see Python tests for real images */
		}
		rltex_tex_release(&t);

		/* truncated file -> miss */
		CHECK(rltex_fs_write_atomic(p1, astc, an - 5) == 0);
		CHECK(rltex_acquire(c, &d, blob, n, &t) == 0 && t.source == RLTEX_SRC_DECODED);
		rltex_tex_release(&t);
		/* bad magic -> miss */
		astc[0] ^= 0xFF;
		CHECK(rltex_fs_write_atomic(p1, astc, an) == 0);
		CHECK(rltex_acquire(c, &d, blob, n, &t) == 0 && t.source == RLTEX_SRC_DECODED);
		rltex_tex_release(&t);
		astc[0] ^= 0xFF;
		/* header dimensions that disagree with the descriptor -> miss */
		astc[7] = 12;
		CHECK(rltex_fs_write_atomic(p1, astc, an) == 0);
		CHECK(rltex_acquire(c, &d, blob, n, &t) == 0 && t.source == RLTEX_SRC_DECODED);
		rltex_tex_release(&t);
		astc[7] = 16;
		/* restored -> hit again */
		CHECK(rltex_fs_write_atomic(p1, astc, an) == 0);
		CHECK(rltex_acquire(c, &d, blob, n, &t) == 0 && t.source == RLTEX_SRC_ASTC_CACHE);
		rltex_tex_release(&t);
		free(astc);
		free(pix);
		rltex_astc_encoder_destroy(e);
	}
	rltex_ctx_destroy(c);
	free(blob);
	rm_rf(root);
	free(root);
}

static void test_no_astc_formats(void)
{
	char *root = mk_root();
	rltex_config cfg;
	rltex_ctx *c;
	rltex_tex t;
	size_t n;
	char logp[300], bcdir[300];
	uint8_t *blob;
	rltex_desc d = {RLTEX_DXGI_BC4_SNORM, 8, 8, 0, 0};

	rltex_config_defaults(&cfg);
	cfg.background_encode = 0;
	cfg.bc6h_astc_hdr = 0;
	CHECK(rltex_ctx_create(root, &cfg, &c) == 0);
	snprintf(logp, sizeof logp, "%s/logs/texture_miss.log", root);
	snprintf(bcdir, sizeof bcdir, "%s/cache/textures/bc", root);

	blob = make_blob(d.dxgi_format, 8, 8, &n);
	CHECK(rltex_acquire(c, &d, blob, n, &t) == 0);
	CHECK(t.source == RLTEX_SRC_DECODED && t.pixfmt == RLTEX_PIX_R8_SNORM && !t.queued_for_encode);
	rltex_tex_release(&t);
	free(blob);

	d.dxgi_format = RLTEX_DXGI_BC6H_UF16; /* mode rgba16f: keep half floats */
	blob = make_blob(d.dxgi_format, 8, 8, &n);
	CHECK(rltex_acquire(c, &d, blob, n, &t) == 0);
	CHECK(t.source == RLTEX_SRC_DECODED && t.pixfmt == RLTEX_PIX_RGBA16_FLOAT && t.pixels_size == 8 * 8 * 8);
	rltex_tex_release(&t);
	free(blob);

	CHECK(count_ext(bcdir, ".bc") == 0 && count_lines(logp) <= 0);
	rltex_ctx_destroy(c);
	rm_rf(root);
	free(root);
}

#ifdef __linux__
/* nice value of the thread named rltex-encode, from /proc/self/task/<tid>/stat (field 19) */
static int worker_nice(void)
{
	DIR *d = opendir("/proc/self/task");
	struct dirent *e;
	int nice = 1000;
	if (!d) return nice;
	while ((e = readdir(d))) {
		char path[300], buf[1024];
		FILE *f;
		if (e->d_name[0] == '.') continue;
		snprintf(path, sizeof path, "/proc/self/task/%s/stat", e->d_name);
		f = fopen(path, "r");
		if (!f) continue;
		if (fgets(buf, sizeof buf, f)) {
			char *rp = strrchr(buf, ')');
			if (strstr(buf, "(rltex-encode)") && rp) {
				long v;
				int field = 3;
				char *q = rp + 2; /* field 3 (state) */
				while (field < 19) {
					q = strchr(q, ' ');
					if (!q) break;
					q++;
					field++;
				}
				if (q && sscanf(q, "%ld", &v) == 1) nice = (int)v;
			}
		}
		fclose(f);
	}
	closedir(d);
	return nice;
}
#endif

static void test_background_encode(void)
{
	static const uint32_t fmts[] = {RLTEX_DXGI_BC1_UNORM_SRGB, RLTEX_DXGI_BC2_UNORM, RLTEX_DXGI_BC3_UNORM,
	                                RLTEX_DXGI_BC4_UNORM, RLTEX_DXGI_BC5_UNORM, RLTEX_DXGI_BC6H_UF16,
	                                RLTEX_DXGI_BC7_UNORM_SRGB};
	char *root = mk_root();
	rltex_config cfg;
	rltex_ctx *c;
	rltex_queue_stats st;
	char text[128];
	size_t nf = sizeof fmts / sizeof fmts[0];

	rltex_config_defaults(&cfg);
	CHECK(rltex_ctx_create(root, &cfg, &c) == 0);
	rltex_ctx_stats(c, &st);
	CHECK(st.worker_running == 1 && st.worker_priority_applied == 1);
	CHECK(rltex_ctx_worker_priority_text(c, text, sizeof text) == 0);
	printf("  worker priority: %s\n", text);
#ifdef __linux__
	CHECK(strstr(text, "nice 19") != NULL);
	CHECK(worker_nice() == 19);
	printf("  /proc nice of thread 'rltex-encode': %d\n", worker_nice());
#endif

	for (size_t i = 0; i < nf; i++) {
		rltex_desc d = {fmts[i], 20, 12, 0, 0}; /* not a multiple of 4 or 6 */
		rltex_tex t;
		size_t n;
		uint8_t *blob = make_blob(fmts[i], 20, 12, &n);
		CHECK(rltex_acquire(c, &d, blob, n, &t) == 0);
		CHECK(t.source == RLTEX_SRC_DECODED && t.queued_for_encode == 1);
		rltex_tex_release(&t);
		free(blob);
	}
	CHECK(rltex_ctx_wait_idle(c, 60000) == 0);
	rltex_ctx_stats(c, &st);
	CHECK(st.queued == nf && st.encoded == nf && st.failed == 0 && st.dropped == 0);

	{
		/* Re-acquire every logged key via the miss log: each must now be a hit with the block
		 * size, profile and swizzle from the mapping table. */
		char logp[300], line[256];
		FILE *f;
		int seen = 0;
		snprintf(logp, sizeof logp, "%s/logs/texture_miss.log", root);
		f = fopen(logp, "r");
		CHECK(f != NULL);
		while (f && fgets(line, sizeof line, f)) {
			char key[65], path[400];
			unsigned fmt, w, h, mip, sl;
			size_t bn;
			uint8_t *file, *bcf;
			size_t fn, bcn;
			const uint8_t *pb;
			rltex_desc d, d2;
			rltex_bc_info info;
			rltex_astc_params p;
			rltex_astc_header ah;
			if (sscanf(line, "%64[0-9a-f],%u,%u,%u,%u,%u,%zu", key, &fmt, &w, &h, &mip, &sl, &bn) != 7) continue;
			seen++;
			d.dxgi_format = fmt; d.width = w; d.height = h; d.mip_level = mip; d.array_slice = sl;
			/* the .astc written by the worker */
			rltex_path_astc(c, key, path, sizeof path);
			CHECK(rltex_fs_read_all(path, &file, &fn) == 0);
			CHECK(rltex_astc_header_parse(file, fn, &ah) == 0);
			rltex_bc_info_for_dxgi(fmt, &info);
			rltex_astc_params_for(&cfg, &info, &p);
			CHECK(ah.block_x == p.block_x && ah.block_y == p.block_y && ah.block_z == 1);
			CHECK(ah.dim_x == 20 && ah.dim_y == 12 && ah.dim_z == 1);
			CHECK(fn == 16 + (size_t)((20 + p.block_x - 1) / p.block_x) * ((12 + p.block_y - 1) / p.block_y) * 16);
			free(file);
			/* re-acquire from the saved .bc: it must now be a cache hit */
			rltex_path_bc(c, key, path, sizeof path);
			CHECK(rltex_fs_read_all(path, &bcf, &bcn) == 0);
			CHECK(rltex_bc_file_parse(bcf, bcn, &d2, &pb, &bcn) == 0);
			{
				rltex_tex t;
				CHECK(rltex_acquire(c, &d2, pb, bcn, &t) == 0);
				CHECK(t.source == RLTEX_SRC_ASTC_CACHE && !strcmp(t.key, key));
				CHECK(t.astc_profile == p.profile && !strcmp(t.view_swizzle, p.view_swizzle));
				rltex_tex_release(&t);
			}
			free(bcf);
		}
		if (f) fclose(f);
		CHECK(seen == (int)nf);
	}

	/* quality of a real round trip through the worker: smooth BC3 gradient */
	{
		rltex_desc d = {RLTEX_DXGI_BC3_UNORM, 64, 64, 0, 0};
		size_t n;
		uint8_t *blob = (uint8_t *)calloc(1, 16 * 16 * 16);
		rltex_tex t;
		n = 16 * 16 * 16;
		for (int by = 0; by < 16; by++)
			for (int bx = 0; bx < 16; bx++) {
				uint8_t *p = blob + (size_t)(by * 16 + bx) * 16;
				unsigned c0 = (unsigned)(((bx * 2) << 11) | ((by * 4) << 5) | 31);
				unsigned c1 = (unsigned)((((bx * 2) + 1) << 11) | ((by * 4 + 2) << 5) | 5);
				p[0] = (uint8_t)(bx * 16); p[1] = (uint8_t)(by * 16 + 8);
				p[8] = (uint8_t)c0; p[9] = (uint8_t)(c0 >> 8); p[10] = (uint8_t)c1; p[11] = (uint8_t)(c1 >> 8);
				p[12] = 0xE4; p[13] = 0xE4; p[14] = 0xE4; p[15] = 0xE4;
			}
		CHECK(rltex_acquire(c, &d, blob, n, &t) == 0 && t.source == RLTEX_SRC_DECODED);
		{
			uint8_t *ref = (uint8_t *)malloc(64 * 64 * 4), *dec = (uint8_t *)malloc(64 * 64 * 4);
			rltex_tex h;
			memcpy(ref, t.pixels, 64 * 64 * 4);
			CHECK(rltex_ctx_wait_idle(c, 60000) == 0);
			CHECK(rltex_acquire(c, &d, blob, n, &h) == 0 && h.source == RLTEX_SRC_ASTC_CACHE);
			CHECK(astc_decode_u8(&h, dec) == 0);
			{
				double ps = psnr_u8(dec, ref, 64 * 64 * 4);
				printf("  worker BC3 gradient 4x4 fast PSNR: %.2f dB\n", ps);
				CHECK(ps > 35.0);
			}
			rltex_tex_release(&h);
			free(ref);
			free(dec);
		}
		rltex_tex_release(&t);
		free(blob);
	}
	rltex_ctx_destroy(c);
	rm_rf(root);
	free(root);
}

static void test_size_cap(void)
{
	char *root = mk_root();
	rltex_config cfg;
	rltex_ctx *c;
	rltex_queue_stats st;
	rltex_tex t;
	size_t n;
	rltex_desc big = {RLTEX_DXGI_BC1_UNORM, 32, 32, 0, 0}, small = {RLTEX_DXGI_BC1_UNORM, 16, 16, 0, 0};
	uint8_t *b1 = make_blob(big.dxgi_format, 32, 32, &n);
	uint8_t *b2;
	char bcdir[300], logp[300];

	rltex_config_defaults(&cfg);
	cfg.encode_max_pixels = 16 * 16; /* 256 texels: the 32x32 texture is over the cap */
	CHECK(rltex_ctx_create(root, &cfg, &c) == 0);
	CHECK(rltex_acquire(c, &big, b1, n, &t) == 0 && t.source == RLTEX_SRC_DECODED && !t.queued_for_encode);
	rltex_tex_release(&t);
	b2 = make_blob(small.dxgi_format, 16, 16, &n);
	CHECK(rltex_acquire(c, &small, b2, n, &t) == 0 && t.queued_for_encode == 1);
	rltex_tex_release(&t);
	CHECK(rltex_ctx_wait_idle(c, 60000) == 0);
	rltex_ctx_stats(c, &st);
	CHECK(st.skipped_large == 1 && st.queued == 1 && st.encoded == 1);
	/* the oversized texture is still persisted and logged for the offline encoder */
	snprintf(bcdir, sizeof bcdir, "%s/cache/textures/bc", root);
	snprintf(logp, sizeof logp, "%s/logs/texture_miss.log", root);
	CHECK(count_ext(bcdir, ".bc") == 2 && count_lines(logp) == 2);
	rltex_ctx_destroy(c);
	free(b1);
	free(b2);
	rm_rf(root);
	free(root);
}

static void test_queue_overflow_and_shutdown(void)
{
	char *root = mk_root();
	rltex_config cfg;
	rltex_ctx *c;
	rltex_queue_stats st;
	char bcdir[300];
	const int N = 40;

	rltex_config_defaults(&cfg);
	cfg.queue_capacity = 2;
	cfg.runtime_quality = RLTEX_QUALITY_EXHAUSTIVE; /* slow worker, fast producer */
	CHECK(rltex_ctx_create(root, &cfg, &c) == 0);
	for (int i = 0; i < N; i++) {
		rltex_desc d = {RLTEX_DXGI_BC7_UNORM, 128, 128, (uint32_t)i, 0};
		rltex_tex t;
		size_t n;
		uint8_t *blob = make_blob(d.dxgi_format, 128, 128, &n);
		CHECK(rltex_acquire(c, &d, blob, n, &t) == 0);
		rltex_tex_release(&t);
		free(blob);
	}
	rltex_ctx_stats(c, &st);
	printf("  overflow: queued=%llu dropped=%llu\n", (unsigned long long)st.queued, (unsigned long long)st.dropped);
	CHECK(st.dropped > 0 && st.queued + st.dropped == (uint64_t)N);
	/* every texture's .bc was saved even though not all were queued */
	snprintf(bcdir, sizeof bcdir, "%s/cache/textures/bc", root);
	CHECK(count_ext(bcdir, ".bc") == N);
	/* destroying with work pending returns promptly: pending jobs are dropped, the in-flight one finishes */
	rltex_ctx_destroy(c);
	snprintf(bcdir, sizeof bcdir, "%s/cache/textures", root);
	CHECK(count_ext(bcdir, ".astc") < N);
	rm_rf(root);
	free(root);
}

int main(void)
{
	test_miss_log_and_paths();
	test_no_astc_formats();
	test_background_encode();
	test_size_cap();
	test_queue_overflow_and_shutdown();
	printf("test_cache: %d checks, %d failures\n", g_checks, g_fail);
	return g_fail ? 1 : 0;
}
