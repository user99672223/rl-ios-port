/* port.toml subset parser, BC -> ASTC mapping, .astc header (de)serialisation. */
#include "rltex.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- defaults ------------------------------------------------------------------------- */

void rltex_config_defaults(rltex_config *c)
{
	memset(c, 0, sizeof *c);
	c->bc1 = (rltex_block_dim){6, 6};
	c->bc2 = (rltex_block_dim){4, 4};
	c->bc3 = (rltex_block_dim){4, 4};
	c->bc4 = (rltex_block_dim){6, 6};
	c->bc5 = (rltex_block_dim){4, 4};
	c->bc6h = (rltex_block_dim){4, 4};
	c->bc7 = (rltex_block_dim){4, 4};
	c->bc6h_astc_hdr = 1;
	c->runtime_quality = RLTEX_QUALITY_FAST;
	c->encode_qos = RLTEX_QOS_BACKGROUND;
	c->queue_capacity = 256;
	c->encode_max_pixels = 2048u * 2048u;
	c->background_encode = 1;
}

/* The 14 legal 2D sizes: astc-encoder Source/astcenccli_toplevel_help.cpp:125-140. */
int rltex_parse_block_string(const char *s, rltex_block_dim *out)
{
	static const uint8_t legal[14][2] = {{4, 4},  {5, 4},  {5, 5},   {6, 5},   {6, 6},
	                                     {8, 5},  {8, 6},  {10, 5},  {10, 6},  {8, 8},
	                                     {10, 8}, {10, 10}, {12, 10}, {12, 12}};
	unsigned x, y;
	char tail;
	if (!s || sscanf(s, "%ux%u%c", &x, &y, &tail) != 2) return -1;
	for (int i = 0; i < 14; i++) {
		if (legal[i][0] == x && legal[i][1] == y) {
			out->x = (uint8_t)x;
			out->y = (uint8_t)y;
			return 0;
		}
	}
	return -1;
}

/* ---- TOML subset ---------------------------------------------------------------------- */

static void note(char *err, size_t cap, const char *fmt, const char *a, const char *b)
{
	size_t used;
	if (!err || !cap) return;
	used = strlen(err);
	if (used + 1 < cap) snprintf(err + used, cap - used, fmt, a, b);
}

/* Extracts key and value text from one line; returns 0 when the line is blank, a comment, a
 * table header, or not `key = value`. Quoted strings lose their quotes; trailing comments are
 * stripped from bare values. */
static int split_line(char *line, char **key, char **val, int *is_table)
{
	char *p = line, *eq, *e;
	*is_table = 0;
	while (isspace((unsigned char)*p)) p++;
	if (!*p || *p == '#') return 0;
	if (*p == '[') { *is_table = 1; return 0; }
	eq = strchr(p, '=');
	if (!eq) return 0;
	*eq = 0;
	e = eq;
	while (e > p && isspace((unsigned char)e[-1])) *--e = 0;
	*key = p;
	p = eq + 1;
	while (isspace((unsigned char)*p)) p++;
	if (*p == '"') {
		p++;
		e = strchr(p, '"');
		if (e) *e = 0;
	} else {
		e = strchr(p, '#');
		if (e) *e = 0;
		e = p + strlen(p);
		while (e > p && isspace((unsigned char)e[-1])) *--e = 0;
	}
	*val = p;
	return 1;
}

static int parse_quality(const char *v, float *out)
{
	static const struct { const char *n; float q; } names[] = {
		{"fastest", RLTEX_QUALITY_FASTEST}, {"fast", RLTEX_QUALITY_FAST},
		{"medium", RLTEX_QUALITY_MEDIUM}, {"thorough", RLTEX_QUALITY_THOROUGH},
		{"verythorough", RLTEX_QUALITY_VERYTHOROUGH}, {"exhaustive", RLTEX_QUALITY_EXHAUSTIVE}};
	char *end;
	float f;
	for (size_t i = 0; i < sizeof names / sizeof names[0]; i++) {
		if (!strcmp(v, names[i].n)) { *out = names[i].q; return 0; }
	}
	f = strtof(v, &end);
	if (end == v || *end || f < 0.0f || f > 100.0f) return -1;
	*out = f;
	return 0;
}

int rltex_config_parse_toml(rltex_config *cfg, const char *text, char *err, size_t err_cap)
{
	int bad = 0;
	if (err && err_cap) err[0] = 0;
	/* Pass 0: group keys and scalars. Pass 1: per-format overrides, so that
	 * `astc_bc3_block` (the BC3/BC5/BC7 group of PROJECT_SPEC.md 6.9) never clobbers an
	 * explicit astc_bc5_block regardless of line order. */
	for (int pass = 0; pass < 2; pass++) {
		const char *p = text;
		int in_table = 0;
		while (*p) {
			char line[512], *key, *val;
			size_t n = strcspn(p, "\n");
			int is_table;
			if (n >= sizeof line) n = sizeof line - 1;
			memcpy(line, p, n);
			line[n] = 0;
			p += strcspn(p, "\n");
			if (*p == '\n') p++;
			if (!split_line(line, &key, &val, &is_table)) {
				if (is_table) in_table = 1;
				continue;
			}
			if (in_table) continue;
#define BLOCK_KEY(name, field, want_pass)                                                    \
	if (!strcmp(key, name) && pass == (want_pass)) {                                         \
		rltex_block_dim b;                                                                   \
		if (rltex_parse_block_string(val, &b)) { bad++; note(err, err_cap, "%s: bad ASTC block size '%s'\n", key, val); } \
		else { field = b; }                                                                  \
		continue;                                                                            \
	}
			if (!strcmp(key, "astc_bc3_block") && pass == 0) {
				rltex_block_dim b;
				if (rltex_parse_block_string(val, &b)) {
					bad++;
					note(err, err_cap, "%s: bad ASTC block size '%s'\n", key, val);
				} else {
					cfg->bc2 = cfg->bc3 = cfg->bc5 = cfg->bc7 = b;
				}
				continue;
			}
			BLOCK_KEY("astc_bc1_block", cfg->bc1, 0)
			BLOCK_KEY("astc_bc4_block", cfg->bc4, 0)
			BLOCK_KEY("astc_bc6h_block", cfg->bc6h, 0)
			BLOCK_KEY("astc_bc2_block", cfg->bc2, 1)
			BLOCK_KEY("astc_bc5_block", cfg->bc5, 1)
			BLOCK_KEY("astc_bc7_block", cfg->bc7, 1)
#undef BLOCK_KEY
			if (pass != 0) continue;
			if (!strcmp(key, "astc_bc6h_mode")) {
				if (!strcmp(val, "astc_hdr")) cfg->bc6h_astc_hdr = 1;
				else if (!strcmp(val, "rgba16f")) cfg->bc6h_astc_hdr = 0;
				else { bad++; note(err, err_cap, "%s: expected \"astc_hdr\" or \"rgba16f\", got '%s'\n", key, val); }
			} else if (!strcmp(key, "astc_runtime_quality")) {
				if (parse_quality(val, &cfg->runtime_quality)) {
					bad++;
					note(err, err_cap, "%s: bad quality '%s'\n", key, val);
				}
			} else if (!strcmp(key, "astc_encode_qos")) {
				if (!strcmp(val, "background")) cfg->encode_qos = RLTEX_QOS_BACKGROUND;
				else if (!strcmp(val, "utility")) cfg->encode_qos = RLTEX_QOS_UTILITY;
				else { bad++; note(err, err_cap, "%s: expected \"background\" or \"utility\", got '%s'\n", key, val); }
			} else if (!strcmp(key, "astc_encode_queue")) {
				char *end;
				unsigned long v = strtoul(val, &end, 10);
				if (end == val || *end || v > 1000000ul) { bad++; note(err, err_cap, "%s: bad count '%s'\n", key, val); }
				else cfg->queue_capacity = (uint32_t)v;
			} else if (!strcmp(key, "astc_encode_max_pixels")) {
				char *end;
				unsigned long v = strtoul(val, &end, 10);
				if (end == val || *end || v == 0 || v > 0xFFFFFFFFul) { bad++; note(err, err_cap, "%s: bad pixel count '%s'\n", key, val); }
				else cfg->encode_max_pixels = (uint32_t)v;
			} else if (!strcmp(key, "astc_background_encode")) {
				if (!strcmp(val, "true")) cfg->background_encode = 1;
				else if (!strcmp(val, "false")) cfg->background_encode = 0;
				else { bad++; note(err, err_cap, "%s: expected true or false, got '%s'\n", key, val); }
			}
		}
	}
	return bad;
}

int rltex_config_load_file(rltex_config *cfg, const char *path, char *err, size_t err_cap)
{
	uint8_t *data;
	size_t n;
	char *text;
	int r = rltex_fs_read_all(path, &data, &n);
	if (r) return r;
	text = (char *)malloc(n + 1);
	if (!text) { free(data); return RLTEX_E_NOMEM; }
	memcpy(text, data, n);
	text[n] = 0;
	free(data);
	r = rltex_config_parse_toml(cfg, text, err, err_cap);
	free(text);
	return r;
}

/* ---- BC -> ASTC mapping --------------------------------------------------------------- */

int rltex_astc_params_for(const rltex_config *cfg, const rltex_bc_info *info, rltex_astc_params *p)
{
	rltex_block_dim b;
	memset(p, 0, sizeof *p);
	strcpy(p->encode_swizzle, "rgba");
	strcpy(p->sample_weights, "rgba");
	strcpy(p->view_swizzle, "rgba");
	if (info->is_signed) return RLTEX_E_UNSUPPORTED; /* ASTC has no SNORM / negative HDR */
	switch (info->kind) {
	case RLTEX_BC1: b = cfg->bc1; break;
	case RLTEX_BC2: b = cfg->bc2; break;
	case RLTEX_BC3: b = cfg->bc3; break;
	case RLTEX_BC4:
		b = cfg->bc4;
		strcpy(p->encode_swizzle, "rrr1"); /* luminance: one endpoint pair */
		strcpy(p->sample_weights, "r");
		strcpy(p->view_swizzle, "r001");   /* D3D: BC4 samples as (r, 0, 0, 1) */
		break;
	case RLTEX_BC5:
		b = cfg->bc5;
		strcpy(p->encode_swizzle, "rrrg"); /* astcenc L+A mode, astcenc help text -esw */
		strcpy(p->sample_weights, "ra");
		strcpy(p->view_swizzle, "ra01");   /* D3D: BC5 samples as (r, g, 0, 1) */
		break;
	case RLTEX_BC6H:
		if (!cfg->bc6h_astc_hdr) return RLTEX_E_UNSUPPORTED;
		b = cfg->bc6h;
		p->hdr = 1;
		break;
	case RLTEX_BC7: b = cfg->bc7; break;
	default: return RLTEX_E_FORMAT;
	}
	p->block_x = b.x;
	p->block_y = b.y;
	if (p->hdr) p->profile = RLTEX_ASTC_HDR_RGB_LDR_A;
	else p->profile = info->is_srgb ? RLTEX_ASTC_LDR_SRGB : RLTEX_ASTC_LDR;
	return RLTEX_OK;
}

const char *rltex_astc_profile_cli_flag(rltex_astc_profile p)
{
	/* astc-encoder Source/astcenccli_toplevel.cpp:121-131 */
	switch (p) {
	case RLTEX_ASTC_LDR: return "-cl";
	case RLTEX_ASTC_LDR_SRGB: return "-cs";
	case RLTEX_ASTC_HDR_RGB_LDR_A: return "-ch";
	}
	return "?";
}

/* ---- .astc header: astc-encoder Docs/FileFormat.md, Source/astcenccli_image_load_store.cpp:2606 */

void rltex_astc_header_write(const rltex_astc_header *h, uint8_t o[RLTEX_ASTC_HEADER_SIZE])
{
	o[0] = 0x13; o[1] = 0xAB; o[2] = 0xA1; o[3] = 0x5C;
	o[4] = (uint8_t)h->block_x; o[5] = (uint8_t)h->block_y; o[6] = (uint8_t)h->block_z;
	o[7] = (uint8_t)h->dim_x; o[8] = (uint8_t)(h->dim_x >> 8); o[9] = (uint8_t)(h->dim_x >> 16);
	o[10] = (uint8_t)h->dim_y; o[11] = (uint8_t)(h->dim_y >> 8); o[12] = (uint8_t)(h->dim_y >> 16);
	o[13] = (uint8_t)h->dim_z; o[14] = (uint8_t)(h->dim_z >> 8); o[15] = (uint8_t)(h->dim_z >> 16);
}

int rltex_astc_header_parse(const uint8_t *d, size_t n, rltex_astc_header *h)
{
	if (n < RLTEX_ASTC_HEADER_SIZE) return RLTEX_E_CORRUPT;
	if (d[0] != 0x13 || d[1] != 0xAB || d[2] != 0xA1 || d[3] != 0x5C) return RLTEX_E_CORRUPT;
	h->block_x = d[4]; h->block_y = d[5]; h->block_z = d[6];
	h->dim_x = (uint32_t)d[7] | ((uint32_t)d[8] << 8) | ((uint32_t)d[9] << 16);
	h->dim_y = (uint32_t)d[10] | ((uint32_t)d[11] << 8) | ((uint32_t)d[12] << 16);
	h->dim_z = (uint32_t)d[13] | ((uint32_t)d[14] << 8) | ((uint32_t)d[15] << 16);
	return RLTEX_OK;
}
