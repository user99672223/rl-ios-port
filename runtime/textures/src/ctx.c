/* Cache lookup, miss handling, miss log and the background encode worker. */
#define _POSIX_C_SOURCE 200809L
#include "rltex_internal.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define PATH_CAP 1024

typedef struct job {
	char key[RLTEX_KEY_HEX_SIZE];
} job;

struct rltex_ctx {
	rltex_config cfg;
	char root[PATH_CAP - 128];
	char dir_astc[PATH_CAP];
	char dir_bc[PATH_CAP];
	char miss_log[PATH_CAP];

	pthread_mutex_t mu;
	pthread_cond_t cv_work;
	pthread_cond_t cv_idle;

	/* keys seen this process (open addressing, 65 bytes per slot) */
	char (*seen)[RLTEX_KEY_HEX_SIZE];
	size_t seen_cap, seen_count;

	/* ring buffer of pending encodes */
	job *queue;
	size_t q_head, q_len;

	pthread_t worker;
	int worker_started;
	int stop;
	rltex_queue_stats stats;
	char prio_text[128];
};

/* ---- paths ---------------------------------------------------------------------------- */

static int fmt_path(char *buf, size_t cap, const char *a, const char *key, const char *ext)
{
	int n = snprintf(buf, cap, "%s/%s%s", a, key, ext);
	return (n < 0 || (size_t)n >= cap) ? RLTEX_E_ARG : RLTEX_OK;
}

int rltex_path_astc(const rltex_ctx *c, const char *key, char *buf, size_t cap)
{
	return fmt_path(buf, cap, c->dir_astc, key, ".astc");
}

int rltex_path_bc(const rltex_ctx *c, const char *key, char *buf, size_t cap)
{
	return fmt_path(buf, cap, c->dir_bc, key, ".bc");
}

int rltex_path_miss_log(const rltex_ctx *c, char *buf, size_t cap)
{
	int n = snprintf(buf, cap, "%s", c->miss_log);
	return (n < 0 || (size_t)n >= cap) ? RLTEX_E_ARG : RLTEX_OK;
}

/* ---- seen set ------------------------------------------------------------------------- */

static size_t hash_key(const char *k)
{
	uint64_t h = 1469598103934665603ull; /* FNV-1a */
	for (; *k; k++) h = (h ^ (uint8_t)*k) * 1099511628211ull;
	return (size_t)h;
}

/* Returns 1 if newly inserted, 0 if already present, -1 on OOM. Caller holds mu. */
static int seen_insert(rltex_ctx *c, const char *key)
{
	if ((c->seen_count + 1) * 2 > c->seen_cap) {
		size_t ncap = c->seen_cap ? c->seen_cap * 2 : 1024;
		char(*nt)[RLTEX_KEY_HEX_SIZE] = (char(*)[RLTEX_KEY_HEX_SIZE])calloc(ncap, RLTEX_KEY_HEX_SIZE);
		if (!nt) return -1;
		for (size_t i = 0; i < c->seen_cap; i++) {
			if (c->seen[i][0]) {
				size_t j = hash_key(c->seen[i]) & (ncap - 1);
				while (nt[j][0]) j = (j + 1) & (ncap - 1);
				memcpy(nt[j], c->seen[i], RLTEX_KEY_HEX_SIZE);
			}
		}
		free(c->seen);
		c->seen = nt;
		c->seen_cap = ncap;
	}
	{
		size_t j = hash_key(key) & (c->seen_cap - 1);
		while (c->seen[j][0]) {
			if (!strcmp(c->seen[j], key)) return 0;
			j = (j + 1) & (c->seen_cap - 1);
		}
		memcpy(c->seen[j], key, RLTEX_KEY_HEX_SIZE);
		c->seen_count++;
		return 1;
	}
}

/* ---- ASTC cache read ------------------------------------------------------------------ */

static int block_is_legal(uint32_t x, uint32_t y)
{
	rltex_block_dim b;
	char s[16];
	snprintf(s, sizeof s, "%ux%u", x, y);
	return rltex_parse_block_string(s, &b) == 0;
}

/* Loads and validates <key>.astc against the expected dimensions. */
static int load_astc(const rltex_ctx *c, const char *key, const rltex_desc *d, uint8_t **file,
                     size_t *file_size, rltex_astc_header *h)
{
	char path[PATH_CAP];
	uint8_t *buf;
	size_t n, expect;
	int r;
	if (rltex_path_astc(c, key, path, sizeof path)) return RLTEX_E_ARG;
	r = rltex_fs_read_all(path, &buf, &n);
	if (r) return r;
	r = rltex_astc_header_parse(buf, n, h);
	if (r == RLTEX_OK) {
		if (h->block_z != 1 || h->dim_z != 1 || h->dim_x != d->width || h->dim_y != d->height ||
		    !block_is_legal(h->block_x, h->block_y))
			r = RLTEX_E_CORRUPT;
	}
	if (r == RLTEX_OK) {
		expect = (size_t)((h->dim_x + h->block_x - 1) / h->block_x) *
		         ((h->dim_y + h->block_y - 1) / h->block_y) * 16;
		if (n != RLTEX_ASTC_HEADER_SIZE + expect) r = RLTEX_E_CORRUPT;
	}
	if (r) {
		free(buf);
		return r;
	}
	*file = buf;
	*file_size = n;
	return RLTEX_OK;
}

/* ---- background encode worker --------------------------------------------------------- */

static int encode_one(rltex_ctx *c, rltex_astc_encoder *enc, const char *key)
{
	char bcpath[PATH_CAP], outpath[PATH_CAP];
	uint8_t *file = NULL, *astc = NULL, *rgba = NULL;
	void *pix = NULL;
	size_t fsz = 0, astc_sz = 0;
	rltex_desc d;
	const uint8_t *blob;
	size_t blob_sz;
	rltex_bc_info info;
	rltex_astc_params p;
	rltex_pixfmt pf;
	const void *enc_pixels;
	int r;

	rltex_path_astc(c, key, outpath, sizeof outpath);
	if (rltex_fs_exists(outpath)) return RLTEX_OK; /* e.g. pushed by the offline encoder */
	rltex_path_bc(c, key, bcpath, sizeof bcpath);
	r = rltex_fs_read_all(bcpath, &file, &fsz);
	if (r) return r;
	r = rltex_bc_file_parse(file, fsz, &d, &blob, &blob_sz);
	if (r) goto done;
	rltex_bc_info_for_dxgi(d.dxgi_format, &info);
	r = rltex_astc_params_for(&c->cfg, &info, &p);
	if (r) goto done;
	pf = rltex_decoded_pixfmt(&info);
	pix = malloc((size_t)d.width * d.height * rltex_pixfmt_bytes(pf));
	if (!pix) { r = RLTEX_E_NOMEM; goto done; }
	r = rltex_bc_decode(&d, blob, blob_sz, pix, (size_t)d.width * d.height * rltex_pixfmt_bytes(pf), &pf);
	if (r) goto done;
	if (p.hdr) {
		enc_pixels = pix; /* RGBA16F already */
	} else {
		if (pf == RLTEX_PIX_RGBA8_UNORM) {
			enc_pixels = pix;
		} else {
			rgba = (uint8_t *)malloc((size_t)d.width * d.height * 4);
			if (!rgba) { r = RLTEX_E_NOMEM; goto done; }
			r = rltex_expand_to_rgba8(pf, pix, d.width, d.height, rgba);
			if (r) goto done;
			enc_pixels = rgba;
		}
	}
	r = rltex_astc_encoder_encode(enc, &p, c->cfg.runtime_quality, enc_pixels, d.width, d.height,
	                              &astc, &astc_sz);
	if (r) goto done;
	r = rltex_fs_write_atomic(outpath, astc, astc_sz);
done:
	free(file);
	free(pix);
	free(rgba);
	free(astc);
	return r;
}

static void *worker_main(void *arg)
{
	rltex_ctx *c = (rltex_ctx *)arg;
	rltex_astc_encoder *enc = rltex_astc_encoder_create();
	char text[128];
	int applied;

	rltex_platform_name_thread("rltex-encode");
	applied = rltex_platform_lower_priority(c->cfg.encode_qos, text, sizeof text);
	pthread_mutex_lock(&c->mu);
	c->stats.worker_priority_applied = applied;
	snprintf(c->prio_text, sizeof c->prio_text, "%s", text);
	c->stats.worker_running = 1;
	pthread_cond_broadcast(&c->cv_idle);
	for (;;) {
		job j;
		int r;
		while (!c->stop && c->q_len == 0) pthread_cond_wait(&c->cv_work, &c->mu);
		if (c->stop) break;
		j = c->queue[c->q_head];
		c->q_head = (c->q_head + 1) % c->cfg.queue_capacity;
		c->q_len--;
		c->stats.in_flight = 1;
		pthread_mutex_unlock(&c->mu);
		r = enc ? encode_one(c, enc, j.key) : RLTEX_E_UNSUPPORTED;
		pthread_mutex_lock(&c->mu);
		c->stats.in_flight = 0;
		if (r == RLTEX_OK) c->stats.encoded++;
		else c->stats.failed++;
		pthread_cond_broadcast(&c->cv_idle);
	}
	c->stats.worker_running = 0;
	pthread_cond_broadcast(&c->cv_idle);
	pthread_mutex_unlock(&c->mu);
	rltex_astc_encoder_destroy(enc);
	return NULL;
}

/* ---- context -------------------------------------------------------------------------- */

int rltex_ctx_create(const char *root, const rltex_config *cfg, rltex_ctx **out)
{
	rltex_ctx *c;
	int r;
	if (!root || !out || strlen(root) >= sizeof c->root) return RLTEX_E_ARG;
	c = (rltex_ctx *)calloc(1, sizeof *c);
	if (!c) return RLTEX_E_NOMEM;
	if (cfg) c->cfg = *cfg;
	else rltex_config_defaults(&c->cfg);
	if (c->cfg.queue_capacity == 0) c->cfg.background_encode = 0;
	snprintf(c->root, sizeof c->root, "%s", root);
	snprintf(c->dir_astc, sizeof c->dir_astc, "%s/cache/textures", c->root);
	snprintf(c->dir_bc, sizeof c->dir_bc, "%s/cache/textures/bc", c->root);
	snprintf(c->miss_log, sizeof c->miss_log, "%s/logs/texture_miss.log", c->root);
	/* The cache directories are created lazily too, but doing it up front surfaces permission
	 * problems at startup rather than at the first texture. */
	if ((r = rltex_fs_mkdir_p(c->dir_bc)) != RLTEX_OK) { free(c); return r; }
	{
		char logdir[PATH_CAP];
		snprintf(logdir, sizeof logdir, "%s/logs", c->root);
		if ((r = rltex_fs_mkdir_p(logdir)) != RLTEX_OK) { free(c); return r; }
	}
	pthread_mutex_init(&c->mu, NULL);
	pthread_cond_init(&c->cv_work, NULL);
	pthread_cond_init(&c->cv_idle, NULL);
#ifdef RLTEX_WITH_ASTCENC
	if (c->cfg.background_encode) {
		c->queue = (job *)calloc(c->cfg.queue_capacity, sizeof(job));
		if (!c->queue) { rltex_ctx_destroy(c); return RLTEX_E_NOMEM; }
		if (pthread_create(&c->worker, NULL, worker_main, c) != 0) {
			rltex_ctx_destroy(c);
			return RLTEX_E_IO;
		}
		c->worker_started = 1;
		/* wait until the worker reported its priority state */
		pthread_mutex_lock(&c->mu);
		while (!c->stats.worker_running) pthread_cond_wait(&c->cv_idle, &c->mu);
		pthread_mutex_unlock(&c->mu);
	}
#else
	c->cfg.background_encode = 0;
#endif
	*out = c;
	return RLTEX_OK;
}

void rltex_ctx_destroy(rltex_ctx *c)
{
	if (!c) return;
	if (c->worker_started) {
		pthread_mutex_lock(&c->mu);
		c->stop = 1;
		pthread_cond_broadcast(&c->cv_work);
		pthread_mutex_unlock(&c->mu);
		pthread_join(c->worker, NULL);
	}
	pthread_mutex_destroy(&c->mu);
	pthread_cond_destroy(&c->cv_work);
	pthread_cond_destroy(&c->cv_idle);
	free(c->queue);
	free(c->seen);
	free(c);
}

void rltex_ctx_stats(rltex_ctx *c, rltex_queue_stats *o)
{
	pthread_mutex_lock(&c->mu);
	*o = c->stats;
	pthread_mutex_unlock(&c->mu);
}

int rltex_ctx_wait_idle(rltex_ctx *c, uint32_t timeout_ms)
{
	struct timespec dl;
	int rc = 0;
	clock_gettime(CLOCK_REALTIME, &dl);
	dl.tv_sec += timeout_ms / 1000;
	dl.tv_nsec += (long)(timeout_ms % 1000) * 1000000L;
	if (dl.tv_nsec >= 1000000000L) { dl.tv_sec++; dl.tv_nsec -= 1000000000L; }
	pthread_mutex_lock(&c->mu);
	while ((c->q_len || c->stats.in_flight) && rc == 0)
		rc = pthread_cond_timedwait(&c->cv_idle, &c->mu, &dl);
	rc = (c->q_len || c->stats.in_flight) ? -1 : 0;
	pthread_mutex_unlock(&c->mu);
	return rc;
}

int rltex_ctx_worker_priority_text(rltex_ctx *c, char *buf, size_t cap)
{
	int have;
	pthread_mutex_lock(&c->mu);
	snprintf(buf, cap, "%s", c->prio_text);
	have = c->prio_text[0] != 0;
	pthread_mutex_unlock(&c->mu);
	return have ? RLTEX_OK : RLTEX_E_UNSUPPORTED;
}

/* ---- acquire -------------------------------------------------------------------------- */

void rltex_tex_release(rltex_tex *t)
{
	if (!t) return;
	free(t->owned_);
	free(t->pixels);
	memset(t, 0, sizeof *t);
}

static void log_miss(rltex_ctx *c, const char *key, const rltex_desc *d, size_t blob_size, int bc_saved)
{
	FILE *f = fopen(c->miss_log, "a");
	if (!f) return;
	fprintf(f, "%s,%u,%u,%u,%u,%u,%zu,%d\n", key, d->dxgi_format, d->width, d->height, d->mip_level,
	        d->array_slice, blob_size, bc_saved);
	fclose(f);
}

int rltex_acquire(rltex_ctx *c, const rltex_desc *d, const void *blob, size_t blob_size, rltex_tex *t)
{
	rltex_bc_info info;
	rltex_astc_params p;
	rltex_pixfmt pf;
	size_t expect, pix_bytes;
	int r, has_astc, first;

	if (!c || !d || !blob || !t) return RLTEX_E_ARG;
	memset(t, 0, sizeof *t);
	if ((r = rltex_bc_info_for_dxgi(d->dxgi_format, &info)) != RLTEX_OK) return r;
	if ((r = rltex_bc_blob_size(d->dxgi_format, d->width, d->height, &expect)) != RLTEX_OK) return r;
	if (expect != blob_size) return RLTEX_E_SIZE;

	rltex_key_compute(d, blob, blob_size, t->key);
	t->width = d->width;
	t->height = d->height;
	has_astc = rltex_astc_params_for(&c->cfg, &info, &p) == RLTEX_OK;

	if (has_astc) {
		uint8_t *file;
		size_t fsz;
		rltex_astc_header h;
		if (load_astc(c, t->key, d, &file, &fsz, &h) == RLTEX_OK) {
			t->source = RLTEX_SRC_ASTC_CACHE;
			t->owned_ = file;
			t->astc_data = file + RLTEX_ASTC_HEADER_SIZE;
			t->astc_size = fsz - RLTEX_ASTC_HEADER_SIZE;
			t->astc_block_x = h.block_x;
			t->astc_block_y = h.block_y;
			t->astc_profile = p.profile;
			memcpy(t->view_swizzle, p.view_swizzle, sizeof t->view_swizzle);
			return RLTEX_OK;
		}
	}

	/* Miss (or format without ASTC mapping): decode for upload. */
	pf = rltex_decoded_pixfmt(&info);
	pix_bytes = (size_t)d->width * d->height * rltex_pixfmt_bytes(pf);
	t->pixels = malloc(pix_bytes);
	if (!t->pixels) return RLTEX_E_NOMEM;
	r = rltex_bc_decode(d, blob, blob_size, t->pixels, pix_bytes, &pf);
	if (r) {
		free(t->pixels);
		memset(t, 0, sizeof *t);
		return r;
	}
	t->source = RLTEX_SRC_DECODED;
	t->pixels_size = pix_bytes;
	t->pixfmt = pf;
	if (!has_astc) return RLTEX_OK;
	memcpy(t->view_swizzle, "rgba", 5);

	/* Persist the BC blob, log the miss and queue the encode, once per key per process. */
	pthread_mutex_lock(&c->mu);
	first = seen_insert(c, t->key);
	pthread_mutex_unlock(&c->mu);
	if (first == 1) {
		char bcpath[PATH_CAP];
		int saved = 0, ok = 0;
		rltex_path_bc(c, t->key, bcpath, sizeof bcpath);
		if (rltex_fs_exists(bcpath)) {
			ok = 1;
		} else {
			uint8_t *f;
			size_t fn;
			if (rltex_bc_file_build(d, blob, blob_size, &f, &fn) == RLTEX_OK) {
				ok = saved = rltex_fs_write_atomic(bcpath, f, fn) == RLTEX_OK;
				free(f);
			}
		}
		pthread_mutex_lock(&c->mu);
		log_miss(c, t->key, d, blob_size, saved);
		if (ok && c->worker_started) {
			if ((uint64_t)d->width * d->height > c->cfg.encode_max_pixels) {
				c->stats.skipped_large++; /* too big for the on-device encoder; left for encode.py */
			} else if (c->q_len < c->cfg.queue_capacity) {
				job *j = &c->queue[(c->q_head + c->q_len) % c->cfg.queue_capacity];
				memcpy(j->key, t->key, RLTEX_KEY_HEX_SIZE);
				c->q_len++;
				c->stats.queued++;
				t->queued_for_encode = 1;
				pthread_cond_signal(&c->cv_work);
			} else {
				c->stats.dropped++;
			}
		}
		pthread_mutex_unlock(&c->mu);
	}
	return RLTEX_OK;
}
