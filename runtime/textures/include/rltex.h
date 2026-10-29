/*
 * rltex.h - hybrid BC -> ASTC texture pipeline (PROJECT_SPEC.md section 6.5).
 *
 * Portable C11. The core (formats, decode, key, cache, config, miss log) uses only the C
 * library plus POSIX file calls (mkdir/stat/rename). The optional ASTC encoder
 * (rltex_astc_*, rltex_queue_*) links ARM astc-encoder 5.7.0 and pthreads; Apple QoS is
 * used under __APPLE__.
 *
 * Full format definitions (key header, .bc file, .astc file, format mapping) are in
 * docs/notes/textures.md.
 */
#ifndef RLTEX_H
#define RLTEX_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------------------------------
 * Status codes
 * ---------------------------------------------------------------------------------------- */
enum {
	RLTEX_OK = 0,
	RLTEX_E_ARG = -1,         /* bad argument */
	RLTEX_E_FORMAT = -2,      /* DXGI format is not a supported BC format */
	RLTEX_E_SIZE = -3,        /* blob size does not match dimensions / format */
	RLTEX_E_NOMEM = -4,
	RLTEX_E_IO = -5,
	RLTEX_E_CORRUPT = -6,     /* cache file failed validation */
	RLTEX_E_ASTC = -7,        /* astc-encoder reported an error */
	RLTEX_E_UNSUPPORTED = -8  /* format has no ASTC mapping (e.g. SNORM) or encoder not built */
};
const char *rltex_status_string(int status);

/* ------------------------------------------------------------------------------------------
 * SHA-256 (FIPS 180-4)
 * ---------------------------------------------------------------------------------------- */
typedef struct rltex_sha256_ctx {
	uint32_t h[8];
	uint64_t total_bytes;
	uint8_t buf[64];
	uint32_t buf_len;
} rltex_sha256_ctx;

void rltex_sha256_init(rltex_sha256_ctx *c);
void rltex_sha256_update(rltex_sha256_ctx *c, const void *data, size_t len);
void rltex_sha256_final(rltex_sha256_ctx *c, uint8_t out[32]);
void rltex_sha256(const void *data, size_t len, uint8_t out[32]);

/* ------------------------------------------------------------------------------------------
 * Formats
 * ---------------------------------------------------------------------------------------- */

/* DXGI_FORMAT values, from mingw-w64 dxgiformat.h (identical to the Windows SDK). */
enum {
	RLTEX_DXGI_BC1_TYPELESS = 0x46, RLTEX_DXGI_BC1_UNORM = 0x47, RLTEX_DXGI_BC1_UNORM_SRGB = 0x48,
	RLTEX_DXGI_BC2_TYPELESS = 0x49, RLTEX_DXGI_BC2_UNORM = 0x4a, RLTEX_DXGI_BC2_UNORM_SRGB = 0x4b,
	RLTEX_DXGI_BC3_TYPELESS = 0x4c, RLTEX_DXGI_BC3_UNORM = 0x4d, RLTEX_DXGI_BC3_UNORM_SRGB = 0x4e,
	RLTEX_DXGI_BC4_TYPELESS = 0x4f, RLTEX_DXGI_BC4_UNORM = 0x50, RLTEX_DXGI_BC4_SNORM = 0x51,
	RLTEX_DXGI_BC5_TYPELESS = 0x52, RLTEX_DXGI_BC5_UNORM = 0x53, RLTEX_DXGI_BC5_SNORM = 0x54,
	RLTEX_DXGI_BC6H_TYPELESS = 0x5e, RLTEX_DXGI_BC6H_UF16 = 0x5f, RLTEX_DXGI_BC6H_SF16 = 0x60,
	RLTEX_DXGI_BC7_TYPELESS = 0x61, RLTEX_DXGI_BC7_UNORM = 0x62, RLTEX_DXGI_BC7_UNORM_SRGB = 0x63
};

typedef enum rltex_bc_kind {
	RLTEX_BC1 = 1, RLTEX_BC2, RLTEX_BC3, RLTEX_BC4, RLTEX_BC5, RLTEX_BC6H, RLTEX_BC7
} rltex_bc_kind;

typedef struct rltex_bc_info {
	rltex_bc_kind kind;
	int is_srgb;
	int is_signed;        /* BC4/BC5 SNORM, BC6H SF16 */
	uint32_t block_bytes; /* 8 or 16 */
} rltex_bc_info;

/* TYPELESS formats are accepted and treated as their UNORM / UF16 variant. */
int rltex_bc_info_for_dxgi(uint32_t dxgi_format, rltex_bc_info *out);
const char *rltex_bc_kind_name(rltex_bc_kind k);

/* Size of one mip level / slice of tightly packed BC blocks: ceil(w/4)*ceil(h/4)*block_bytes. */
int rltex_bc_blob_size(uint32_t dxgi_format, uint32_t width, uint32_t height, size_t *out_size);

/* Pixel layouts produced by the decoder (tightly packed rows, no padding). */
typedef enum rltex_pixfmt {
	RLTEX_PIX_R8_UNORM = 1,
	RLTEX_PIX_RG8_UNORM,
	RLTEX_PIX_RGBA8_UNORM,
	RLTEX_PIX_R8_SNORM,
	RLTEX_PIX_RG8_SNORM,
	RLTEX_PIX_RGBA16_FLOAT /* IEEE half floats, alpha = 1.0 */
} rltex_pixfmt;

size_t rltex_pixfmt_bytes(rltex_pixfmt f);
const char *rltex_pixfmt_name(rltex_pixfmt f);
rltex_pixfmt rltex_decoded_pixfmt(const rltex_bc_info *info);

/* ------------------------------------------------------------------------------------------
 * Texture description, key
 * ---------------------------------------------------------------------------------------- */
typedef struct rltex_desc {
	uint32_t dxgi_format;
	uint32_t width;      /* texel size of THIS mip level */
	uint32_t height;
	uint32_t mip_level;
	uint32_t array_slice;
} rltex_desc;

#define RLTEX_KEY_HEADER_SIZE 32
#define RLTEX_KEY_HEX_SIZE 65 /* 64 hex chars + NUL */

/* Writes the fixed 32 byte key header for (desc, blob_size). Layout in docs/notes/textures.md. */
void rltex_key_header(const rltex_desc *desc, uint64_t blob_size, uint8_t out[RLTEX_KEY_HEADER_SIZE]);
/* key = SHA-256(key_header || blob), as 64 lowercase hex chars + NUL. */
void rltex_key_compute(const rltex_desc *desc, const void *blob, size_t blob_size,
                       char out_hex[RLTEX_KEY_HEX_SIZE]);

/* ------------------------------------------------------------------------------------------
 * BC decode (bcdec)
 * ---------------------------------------------------------------------------------------- */

/* Decodes one mip level into `out` (tight rows). out_cap must be >= width*height*pixel bytes.
 * The blob must be exactly rltex_bc_blob_size() bytes. Non-multiple-of-4 sizes are cropped. */
int rltex_bc_decode(const rltex_desc *desc, const void *blob, size_t blob_size,
                    void *out, size_t out_cap, rltex_pixfmt *out_fmt);

/* Expands R8/RG8 to RGBA8 as (r,0,0,255) / (r,g,0,255), or copies RGBA8. UNORM only. */
int rltex_expand_to_rgba8(rltex_pixfmt fmt, const void *src, uint32_t w, uint32_t h, uint8_t *dst);

/* ------------------------------------------------------------------------------------------
 * Configuration (port.toml subset)
 * ---------------------------------------------------------------------------------------- */
/* Same values as ASTCENC_PRE_* in astc-encoder Source/astcenc.h:253-268 (checked in tests). */
#define RLTEX_QUALITY_FASTEST 0.0f
#define RLTEX_QUALITY_FAST 10.0f
#define RLTEX_QUALITY_MEDIUM 60.0f
#define RLTEX_QUALITY_THOROUGH 98.0f
#define RLTEX_QUALITY_VERYTHOROUGH 99.0f
#define RLTEX_QUALITY_EXHAUSTIVE 100.0f

typedef struct rltex_block_dim {
	uint8_t x, y;
} rltex_block_dim;

typedef enum rltex_qos { RLTEX_QOS_BACKGROUND = 0, RLTEX_QOS_UTILITY = 1 } rltex_qos;

typedef struct rltex_config {
	rltex_block_dim bc1, bc2, bc3, bc4, bc5, bc6h, bc7;
	int bc6h_astc_hdr;          /* 1: BC6H -> ASTC HDR; 0: keep decoded RGBA16F, no ASTC */
	float runtime_quality;      /* astc-encoder effort (ASTCENC_PRE_*) for the on-device encode */
	rltex_qos encode_qos;
	uint32_t queue_capacity;    /* max pending background encodes; more are dropped */
	uint32_t encode_max_pixels; /* larger textures are saved as .bc but not encoded on device
	                               (astc-encoder needs ~45 bytes/texel of working memory) */
	int background_encode;      /* 1 = rltex_ctx starts an encode worker */
} rltex_config;

void rltex_config_defaults(rltex_config *cfg);
/* "6x6" style 2D ASTC block size; only the 14 legal 2D sizes are accepted. */
int rltex_parse_block_string(const char *s, rltex_block_dim *out);
/* Parses the flat top-level keys of port.toml; unknown keys are ignored. Returns the number of
 * invalid values (each leaves the previous value in place and appends a line to err). */
int rltex_config_parse_toml(rltex_config *cfg, const char *text, char *err, size_t err_cap);
int rltex_config_load_file(rltex_config *cfg, const char *path, char *err, size_t err_cap);

/* ------------------------------------------------------------------------------------------
 * BC -> ASTC mapping
 * ---------------------------------------------------------------------------------------- */
typedef enum rltex_astc_profile {
	RLTEX_ASTC_LDR = 0,
	RLTEX_ASTC_LDR_SRGB,
	RLTEX_ASTC_HDR_RGB_LDR_A
} rltex_astc_profile;

typedef struct rltex_astc_params {
	uint32_t block_x, block_y;
	rltex_astc_profile profile;
	char encode_swizzle[5]; /* astcenc -esw applied to the RGBA expansion of the decoded BC */
	char sample_weights[5]; /* astcenc -ssw letters (error weights); "rgba" = all */
	char view_swizzle[5];   /* swizzle a texture view must apply to the ASTC texel to match the
	                           BC texture's sampled value; chars from r,g,b,a,0,1 */
	int hdr;
} rltex_astc_params;

/* Returns RLTEX_OK, or RLTEX_E_UNSUPPORTED when this format is not ASTC-encoded
 * (SNORM, BC6H SF16, or BC6H with bc6h_astc_hdr = 0). */
int rltex_astc_params_for(const rltex_config *cfg, const rltex_bc_info *info, rltex_astc_params *out);
/* astcenc CLI compress flag for the profile: "-cl", "-cs" or "-ch". */
const char *rltex_astc_profile_cli_flag(rltex_astc_profile p);

/* ------------------------------------------------------------------------------------------
 * .astc files (astc-encoder docs/FileFormat.md)
 * ---------------------------------------------------------------------------------------- */
#define RLTEX_ASTC_HEADER_SIZE 16
typedef struct rltex_astc_header {
	uint32_t block_x, block_y, block_z;
	uint32_t dim_x, dim_y, dim_z;
} rltex_astc_header;
void rltex_astc_header_write(const rltex_astc_header *h, uint8_t out[RLTEX_ASTC_HEADER_SIZE]);
int rltex_astc_header_parse(const uint8_t *data, size_t size, rltex_astc_header *out);

/* ------------------------------------------------------------------------------------------
 * ASTC encoder (library path; needs astc-encoder)
 * ---------------------------------------------------------------------------------------- */
typedef struct rltex_astc_encoder rltex_astc_encoder;
rltex_astc_encoder *rltex_astc_encoder_create(void);
void rltex_astc_encoder_destroy(rltex_astc_encoder *e);
/* pixels: RGBA8 (LDR) or RGBA16F (hdr) tightly packed. On success *out is malloc'd (free()) and
 * holds a complete .astc file (16 byte header + payload). */
int rltex_astc_encoder_encode(rltex_astc_encoder *e, const rltex_astc_params *p, float quality,
                              const void *pixels, uint32_t w, uint32_t h,
                              uint8_t **out, size_t *out_size);

/* ------------------------------------------------------------------------------------------
 * Context: cache + miss log + background queue
 * ---------------------------------------------------------------------------------------- */
typedef struct rltex_ctx rltex_ctx;

/* documents_root is the app's Documents directory; everything lives below it:
 *   <root>/cache/textures/<key>.astc
 *   <root>/cache/textures/bc/<key>.bc
 *   <root>/logs/texture_miss.log
 * cfg may be NULL for defaults. */
int rltex_ctx_create(const char *documents_root, const rltex_config *cfg, rltex_ctx **out);
void rltex_ctx_destroy(rltex_ctx *ctx); /* stops the worker; pending jobs are dropped (the .bc stays) */

typedef enum rltex_source {
	RLTEX_SRC_ASTC_CACHE = 1, /* astc_* fields valid */
	RLTEX_SRC_DECODED = 2     /* pixels / pixfmt valid */
} rltex_source;

typedef struct rltex_tex {
	rltex_source source;
	char key[RLTEX_KEY_HEX_SIZE];
	uint32_t width, height;

	/* RLTEX_SRC_ASTC_CACHE: points into memory owned by the rltex_tex. */
	const uint8_t *astc_data; /* payload only (header stripped), 16 bytes per block */
	size_t astc_size;
	uint32_t astc_block_x, astc_block_y;
	rltex_astc_profile astc_profile;
	char view_swizzle[5];

	/* RLTEX_SRC_DECODED */
	void *pixels;
	size_t pixels_size;
	rltex_pixfmt pixfmt;

	int queued_for_encode; /* a background encode was queued by this call */
	void *owned_;          /* internal */
} rltex_tex;

/* The D3D11 texture-creation entry point. Computes the key; on a cache hit returns the ASTC
 * payload; on a miss decodes with bcdec, saves the .bc blob (once), logs the miss (once per
 * key per process) and queues a background encode. Formats with no ASTC mapping are decoded
 * and neither saved nor logged. */
int rltex_acquire(rltex_ctx *ctx, const rltex_desc *desc, const void *blob, size_t blob_size,
                  rltex_tex *out);
void rltex_tex_release(rltex_tex *t);

/* Path helpers (return RLTEX_E_ARG if buf is too small). */
int rltex_path_astc(const rltex_ctx *ctx, const char *key, char *buf, size_t cap);
int rltex_path_bc(const rltex_ctx *ctx, const char *key, char *buf, size_t cap);
int rltex_path_miss_log(const rltex_ctx *ctx, char *buf, size_t cap);

/* .bc file (see docs). Pure functions so tools and tests share one implementation. */
#define RLTEX_BC_FILE_PREFIX_SIZE 8
int rltex_bc_file_build(const rltex_desc *desc, const void *blob, size_t blob_size,
                        uint8_t **out, size_t *out_size);
int rltex_bc_file_parse(const uint8_t *file, size_t file_size, rltex_desc *desc,
                        const uint8_t **blob, size_t *blob_size);

/* Background encode queue introspection. */
typedef struct rltex_queue_stats {
	uint64_t queued, encoded, failed, dropped, skipped_large, in_flight;
	int worker_running;
	int worker_priority_applied; /* 1 if low-priority/QoS setup succeeded on the worker */
} rltex_queue_stats;
void rltex_ctx_stats(rltex_ctx *ctx, rltex_queue_stats *out);
/* Blocks until the queue is empty and the worker idle, or timeout_ms elapses. 0 on idle. */
int rltex_ctx_wait_idle(rltex_ctx *ctx, uint32_t timeout_ms);
/* Test hook: nice value / QoS description of the encode worker, written as text. */
int rltex_ctx_worker_priority_text(rltex_ctx *ctx, char *buf, size_t cap);

/* ------------------------------------------------------------------------------------------
 * File helpers shared with tools
 * ---------------------------------------------------------------------------------------- */
int rltex_fs_mkdir_p(const char *path);
int rltex_fs_read_all(const char *path, uint8_t **out, size_t *out_size);
/* Writes to a temp file next to `path` and renames. */
int rltex_fs_write_atomic(const char *path, const void *data, size_t size);
int rltex_fs_exists(const char *path);

#ifdef __cplusplus
}
#endif
#endif /* RLTEX_H */
