# Hybrid texture pipeline (BC -> ASTC)

Spec: `PROJECT_SPEC.md` 6.5 "Textures (hybrid)", config keys in 6.9, device tooling in 6.10.
Code: `runtime/textures/` (C11 library `rltex` + CLI `rltex`), `tools/textures/encode.py`,
`tests/textures/`. The DXMT integration is done later; this note is the contract for it.

The A16 GPU cannot sample BC formats, so every BC texture D3D11 creates has to become either
ASTC or an uncompressed upload. Flow at texture creation (`rltex_acquire`):

```
BC blob + (format, w, h, mip, slice)
   |  key = SHA-256(key header || blob)
   v
Documents/cache/textures/<key>.astc exists and validates?
   |-- yes: HIT  -> return the ASTC payload + block size (upload as an ASTC texture)
   '-- no : MISS -> bcdec decode (RGBA8 / R8 / RG8 / RGBA16F) and return it for upload
                    + save the original blob to cache/textures/bc/<key>.bc   (once)
                    + append a line to logs/texture_miss.log                  (once per process)
                    + queue a background ASTC encode (low QoS) -> writes <key>.astc for next launch
Offline: tools/device/pull_logs.sh pulls bc/*.bc -> tools/textures/encode.py -> push *.astc back.
```

## 1. The key

`key = lowercase_hex(SHA-256(KEY_HEADER || BLOB))`, 64 hex characters.

`BLOB` is one subresource (one mip level of one array slice) as tightly packed BC blocks:
`ceil(w/4) * ceil(h/4) * block_bytes` bytes (8 for BC1/BC4, 16 for the rest), where `w`, `h` are the
texel size **of that mip level**. The caller (DXMT) must repack D3D11 initial data (which has
row/slice pitches) into this layout before calling.

`KEY_HEADER` is exactly 32 bytes, all integers little-endian:

| offset | size | field |
|---|---|---|
| 0  | 4 | ASCII `RLK1` (key format version; bump to invalidate every cache) |
| 4  | 4 | `u32 dxgi_format`: the numeric `DXGI_FORMAT` (BC1_UNORM = 0x47 = 71 ... BC7_UNORM_SRGB = 0x63 = 99; values from mingw-w64 `dxgiformat.h`) |
| 8  | 4 | `u32 width`  (this mip level) |
| 12 | 4 | `u32 height` (this mip level) |
| 16 | 4 | `u32 mip_level` |
| 20 | 4 | `u32 array_slice` |
| 24 | 8 | `u64 blob_size` in bytes |

Why each field: format and size make identical bytes interpreted as different formats/shapes differ
(a BC1 and a BC4 blob can be byte-identical); mip level and slice make the same content at two
places distinct (the encoder output is per surface and the runtime looks it up per surface);
`blob_size` is explicit length framing.
`TYPELESS` formats are accepted and treated as UNORM/UF16, but the key uses the numeric format the
caller passes, so pass the same value every time.

Golden vector (asserted in C and Python tests): `RLK1 47000000 08000000 04000000 00000000 00000000
1000000000000000` + bytes `00 01 .. 0f` hashes to
`7a4946eadfd85f1cb288b73b7d5263cf0308dc4c9f5c66aa5ee59d14628b69c6`.
The key is implemented independently in C (`format.c`) and Python (`tests/textures/texgen.py`,
`tools/textures/encode.py`) and the tests compare them over every format, several sizes, mips and slices.

## 2. File formats

* **`cache/textures/<key>.astc`**: the ASTC file format, `astc-encoder/Docs/FileFormat.md` and
  `Source/astcenccli_image_load_store.cpp:2606` (`ASTC_MAGIC_ID = 0x5CA1AB13`): 16 byte header
  `13 AB A1 5C | block_x block_y block_z | dim_x[3] dim_y[3] dim_z[3]` (24-bit little-endian dims),
  then `ceil(w/bx)*ceil(h/by)*16` bytes. One 2D surface, `block_z = dim_z = 1`. The runtime validates
  magic, legal 2D block size, z = 1, dimensions equal to the requested texture, and exact size; any
  mismatch is treated as a miss. **The runtime takes the block size from this header, never from the
  config**, so changing `port.toml` never invalidates or breaks an existing cache.
* **`cache/textures/bc/<key>.bc`**: `"RLBC"`, `u32le version = 1`, the 32 byte key header (same bytes
  that were hashed), then the blob. Self-describing: format/size/mip/slice are in it. The key equals
  `SHA-256(file[8:])`; `encode.py` re-derives it and checks the file name.
* **`logs/texture_miss.log`**: append-only text, one line per distinct key per process:
  `key,dxgi_format,width,height,mip,slice,blob_bytes,bc_saved` (`bc_saved` = 1 if this call wrote the
  `.bc`, 0 if it already existed). Not deduplicated across launches (a texture still missing after a
  launch is logged again, which is useful as a coverage signal).
* Writes are atomic (temp file + `rename`).

## 3. Format mapping (defaults; block sizes configurable)

Decoder output (`rltex_bc_decode`, tight rows, cropped to w x h) and the ASTC target:

| DXGI format | decoded upload on a miss | ASTC block | astcenc profile | encode swizzle `-esw` / `-ssw` | view swizzle |
|---|---|---|---|---|---|
| BC1_UNORM | RGBA8 | **6x6** (`astc_bc1_block`) | `-cl` LDR | rgba / rgba | rgba |
| BC1_UNORM_SRGB | RGBA8 (sRGB-encoded values) | 6x6 | `-cs` LDR sRGB | rgba / rgba | rgba |
| BC2_UNORM (+SRGB) | RGBA8 | **4x4** (`astc_bc2_block`, else `astc_bc3_block`) | `-cl` / `-cs` | rgba / rgba | rgba |
| BC3_UNORM (+SRGB) | RGBA8 | **4x4** (`astc_bc3_block`) | `-cl` / `-cs` | rgba / rgba | rgba |
| BC4_UNORM | **R8** | **6x6** (`astc_bc4_block`) | `-cl` | `rrr1` / `r` | `r001` |
| BC5_UNORM | **RG8** | **4x4** (`astc_bc5_block`, else `astc_bc3_block`) | `-cl` | `rrrg` / `ra` | `ra01` |
| BC6H_UF16 | **RGBA16F** (alpha = 1.0) | **4x4** ASTC HDR (`astc_bc6h_block`, `astc_bc6h_mode = "astc_hdr"`) | `-ch` HDR RGB + LDR alpha | rgba / rgba | rgba |
| BC7_UNORM (+SRGB) | RGBA8 | **4x4** (`astc_bc7_block`, else `astc_bc3_block`) | `-cl` / `-cs` | rgba / rgba | rgba |
| BC4_SNORM, BC5_SNORM, BC6H_SF16 | R8_SNORM / RG8_SNORM / RGBA16F | none | none | n/a | n/a |
| BC6H with `astc_bc6h_mode = "rgba16f"` | RGBA16F | none (keep half floats) | none | n/a | n/a |

Rationale:

* **Block sizes** are the spec's. Measured trade-off for BC1 / BC4 (thorough preset, PSNR against the
  bcdec decode, 128x128 synthetic images; `bench.py --blocks`):

  | | bpp | BC1 smooth (gradient / wheel) | BC1 detailed | BC4 smooth | BC4 detailed |
  |---|---|---|---|---|---|
  | 4x4 | 8.00 | 60.3 / 60.3 dB | 55.9 | 62.5 | 57.9 |
  | 5x5 | 5.12 | 49.0 / 47.2 | 32.3 | 54.6 | 43.6 |
  | 6x6 | 3.56 | 47.9 / 45.8 | 30.0 | 49.3 | 37.2 |
  | 8x8 | 2.00 | 44.2 / 43.8 | 27.4 | 44.9 | 31.9 |

  BC1/BC4 are 4 bpp so 6x6 (3.56 bpp) is *smaller* than the source; 4x4/5x5 are larger but visibly
  cleaner on detailed textures. BC2/3/5/6H/7 are 8 bpp, 4x4 (8 bpp) keeps size parity.
* **`astc_bc3_block` is a group key**: it sets BC2, BC3, BC5 and BC7 (spec 6.5 lists "BC3/BC5/BC7 -> 4x4"
  and BC2 -> 4x4 follows). Per-format keys `astc_bc2_block`, `astc_bc5_block`, `astc_bc7_block` override it
  regardless of line order (tested). Extra keys beyond the two in spec 6.9: `astc_bc4_block`,
  `astc_bc6h_block`, `astc_bc6h_mode`, `astc_runtime_quality` (default `fast`), `astc_encode_qos`
  (`background`|`utility`), `astc_encode_queue` (256), `astc_encode_max_pixels` (2048*2048),
  `astc_background_encode` (true). Only the 14 legal 2D sizes are accepted.
* **sRGB**: `*_SRGB` formats map to the ASTC sRGB profile (`ASTCENC_PRF_LDR_SRGB`, CLI `-cs`). BC sRGB data
  decodes to sRGB-encoded 8-bit values (conversion is the sampler's job), which is exactly what
  `-cs` expects as input. Alpha is always linear. The ASTC LDR vs sRGB distinction is the Metal pixel
  format (`ASTC_*_LDR` vs `ASTC_*_sRGB`).
* **BC4 `rrr1`/`r001`, BC5 `rrrg`/`ra01`**: ASTC always returns RGBA, BC4 samples as `(r,0,0,1)` and BC5 as
  `(r,g,0,1)`. Encoding BC4 as luminance and BC5 as luminance+alpha lets astcenc use its cheap L / L+A
  endpoint modes (astcenc help text for `-esw`: "map two component data to the L+A endpoint (e.g.,
  -esw rrrg)"; `-ssw` zeroes the error weight of channels the shader never reads). Measured against
  storing the channels as they are (`rg01` / `r001`), astcenc thorough, 64x64 etcpak-encoded synthetic
  textures (`bench.py --swizzle`): BC5 normal map +1.5 dB at 4x4 and +0.6 dB at 6x6, BC5 detailed +1.3 / +0.5 dB;
  BC4 smooth +4.9 dB at 4x4 and +2.3 dB at 6x6, BC4 detailed +2.0 / +1.4 dB (the other `-ssw` choices are
  within 1 dB below the chosen ones). The `view swizzle` column must be applied to the
  ASTC texture view (Metal texture swizzle) so the shader still sees BC semantics; it is returned in
  `rltex_tex.view_swizzle` and printed by `rltex plan`. It is **fixed per format, not configurable**,
  because the `.astc` file does not record how it was encoded and the encode path (runtime and offline)
  must agree.
* **BC4/BC5 decode to R8/RG8**, not RGBA8 as the spec text says: it is the exact D3D semantics with no
  view swizzle on the fallback path and uses 1/4 and 1/2 of the memory. `rltex_expand_to_rgba8` makes
  the RGBA8 form where it is needed (astcenc input).
* **BC6H** decodes to RGBA16F (Metal has no RGB16F), alpha 1.0. ASTC HDR uses profile
  `ASTCENC_PRF_HDR_RGB_LDR_A` (`-ch`) since alpha is constant 1. ASTC has no negative-value HDR, so
  `BC6H_SF16` stays RGBA16F. Whether the A16 supports `MTLPixelFormatASTC_4x4_HDR` is **not verified**
  (Apple's page title exists, its availability text could not be retrieved offline); runtime must check
  and fall back with `astc_bc6h_mode = "rgba16f"`.
* **SNORM** BC4/BC5 have no ASTC equivalent: decoded to `R8_SNORM`/`RG8_SNORM` and never cached.
* BC1 punch-through alpha (3-colour mode, `c0 <= c1`) decodes as `(0,0,0,0)` for index 3 (bcdec behaviour,
  matches the D3D spec) and is carried through ASTC's alpha channel.

## 4. Library

`runtime/textures/include/rltex.h` is the API; `CMakeLists.txt` builds `librltex.a` and the `rltex` CLI.
Portable C11 + POSIX file calls + pthreads. Inputs: `bcdec.h` (`external/bcdec`, pinned 80859ed, v0.985,
`-DBCDEC_BC4BC5_PRECISE`), optionally the astc-encoder 5.7.0 static library (without it
`rltex_astc_*` and the queue compile to `RLTEX_E_UNSUPPORTED` stubs). Verified bcdec details, all in
`src/bc_decode.c`: pitch is in bytes for the 8-bit decoders but in *elements* for `bcdec_bc6h_half`
(`bcdec.h:621-624`); the PRECISE option adds the `isSigned` argument.

astc-encoder API actually used (all in `Source/astcenc.h`): `astcenc_config_init` (:725),
`astcenc_context_alloc` (:761), `astcenc_compress_image` (:785), `astcenc_compress_reset` (:806),
`astcenc_context_free` (:864), `struct astcenc_image` (:613), `ASTCENC_PRF_*` (:241),
`ASTCENC_PRE_FAST` (:256), `ASTCENC_FLG_SELF_DECOMPRESS_ONLY` (:380), `cw_*_weight` (:444-454, what the CLI
`-ssw` sets, `astcenccli_toplevel.cpp:792-845`). The runtime uses one single-threaded context,
re-created only when (profile, block, quality, weights) change.

Background encode (`src/ctx.c`, `src/platform.c`): one worker thread `rltex-encode`; a job is just a key,
the worker re-reads `bc/<key>.bc` from disk (so queue memory is ~65 bytes per job, not the blob),
decodes, encodes at `astc_runtime_quality` (default `ASTCENC_PRE_FAST`), writes `<key>.astc`
atomically. Bounded queue (`astc_encode_queue`, extra jobs counted as `dropped`, they stay on disk as
`.bc` and are picked up offline or next launch). Textures above `astc_encode_max_pixels` are saved and
logged but not encoded on device (`skipped_large`). Destroy drops pending jobs, lets the in-flight one
finish. Priority:

* `__APPLE__`: `pthread_set_qos_class_self_np(QOS_CLASS_BACKGROUND | QOS_CLASS_UTILITY, 0)` called on the
  worker itself (`<pthread/qos.h>`; signature at libpthread `include/pthread/qos.h:155`, constants at
  `include/sys/qos.h:130-141`, https://github.com/apple-oss-distributions/libpthread). BACKGROUND is
  the default (efficiency cores, throttled I/O); `astc_encode_qos = "utility"` is the alternative.
  No later `pthread_setschedparam` on that thread (the header says it would opt the thread out of QoS).
  `pthread_setname_np(const char*)` names the thread (`include/pthread/pthread.h:535`).
* Linux: `setpriority(PRIO_PROCESS, gettid, 19)`. Other: no-op, reported via `worker_priority_applied`.

`rltex_ctx_stats` exposes queued/encoded/failed/dropped/skipped_large/in_flight and whether priority
setup succeeded, for the first on-device run's logs.

## 5. Offline tool

```
tools/textures/encode.py --in DIR_OF_.bc --out DIR [--jobs N] [--config port.toml]
                         [--quality thorough] [--force] [--rltex PATH] [--astcenc PATH] [--quiet]
```

Per `.bc`: parse + verify key; skip if `<key>.astc` exists and validates; `rltex prepare` (bcdec decode to
a KTX1 RGBA8 / RGBA16F file for astcenc, plus the plan as JSON from the *same* C mapping the runtime
uses); then
`astcenc {-cl|-cs|-ch} in.ktx out.astc <block> -thorough -j 1 [-esw ..] [-ssw ..] -silent`.
Flags confirmed in `astc-encoder/Source/astcenccli_toplevel_help.cpp` (mode flags :112-113 and
`modes[]` in `astcenccli_toplevel.cpp:121-131`, block sizes :125-140, presets :161-166 with
`-thorough` = 98, `-esw/-ssw` :416-440, `-j`, `-silent`) and KTX input requirements in
`astcenccli_image_load_store.cpp:871-1000` (GL_RGBA8 / GL_RGBA16F accepted; the CLI reads
PNG/BMP/TGA/JPEG/EXR/HDR/KTX/DDS). `-dl/-ds/-dh` decode; the HDR decode writes RGB KTX when alpha is 1.
Exit 0 ok, 1 a file failed, 2 usage / missing tool / invalid config. `astcenc` is found via
`--astcenc`, `$ASTCENC`, `build/astcenc/Source/astcenc-{avx2,sse4.1}`, then `PATH`; an explicit path is
never silently replaced. Determinism (tested): output is byte-identical for `--jobs 1` vs `8`, for
repeated runs, and for the AVX2 vs SSE4.1 builds of astcenc.

`rltex` CLI: `config`, `key`, `plan`, `decode` (raw or KTX), `prepare`, `encode` (library path), `lookup`
(runs `rltex_acquire` on a Documents tree; used by tests to prove `encode.py` output is a runtime hit).

## 6. Tests and evidence (Debian x86-64, clang 19.1.7, astcenc 5.7.0)

`tests/textures/run.sh` builds everything with `clang -Wall -Wextra -Werror` (library, CLI, tests; third
party headers are `-isystem`), creates `build/venv` from `requirements.txt` and runs:

| suite | what it proves | result |
|---|---|---|
| `test_core.c` | SHA-256 NIST vectors + padding boundaries; key header bytes and golden keys; every DXGI format entry; **hand-built blocks with exactly known colours** (BC1 4-colour `(170,0,85)`/`(85,0,170)`, BC1 3-colour midpoint `(66,65,66)` + transparent index, BC2 alpha nibbles, BC3/BC4/BC5 6- and 4-interpolant tables, BC4 SNORM clamp, BC7 mode 6 `(201,101,51,255)`, BC6H alpha = 1.0); cropping of 5x3; config parser (group/override keys, bad values, comments, tables); BC->ASTC mapping for every format incl. sRGB; `.astc` header bytes + 24-bit dims; `.bc` file; `ASTCENC_PRE_*` constants | 389 checks, 0 failures |
| `test_cache.c` | paths; miss -> decoded + `.bc` + one log line; repeat miss does not rewrite or re-log; new process logs again with `bc_saved=0`; pre-placed `.astc` -> hit with correct block/payload; truncated / bad magic / wrong dimensions -> miss; background encode for all 7 formats at 20x12 -> `.astc` with correct header, then hit with mapped profile/swizzle; worker nice = 19 read from `/proc`; queue overflow (3 queued, 37 dropped, all 40 `.bc` kept); size cap; prompt shutdown; SNORM / `rgba16f` BC6H bypass the cache | 204 checks, 0 failures |
| ASan+UBSan, TSan runs of both | memory and thread safety of cache + queue | clean |
| `apple_syntax_check.c` | the `__APPLE__` QoS branch type-checks against declaration stubs copied from Apple's headers (syntax only; real iOS build is later) | ok |
| `test_crosscheck.py` | bcdec vs independent decoders on ISPC and etcpak encoder output (345 texture/size combos, sizes 64x64, 37x23, 13x5, 4x4, 1x1), random blocks, key parity, ASTC round trips | 76 rows, 0 failures |
| `test_encode.py` | `encode.py` end to end (see 5), runtime hit on its output | 105 checks, 0 failures |

Decoder agreement (max |diff| in 8-bit units; limits asserted). Independent decoders: Pillow 13.0.0
(libImaging BcnDecode via its DDS plugin) for every format, texture2ddecoder 1.0.6 for BC1/3/4/5/6/7, and a
pure-Python D3D-spec BC4/BC5 reference (UNORM and SNORM). Encoders: ISPC texcomp (BC1/3/4/5/6H/7) and etcpak
(BC1/3/4/5/7); BC2 assembled from a BC1 colour block + 4-bit alpha.

| format | vs Pillow (encoder output) | vs texture2ddecoder | random blocks vs Pillow | vs spec reference |
|---|---|---|---|---|
| BC1 | 1 | 1 | 1 (1024 blocks), 1 (all 565 values) | n/a |
| BC2 | 1 | n/a (no BC2) | 1 | n/a |
| BC3 | 1 | 1 | 1 | n/a |
| BC4 | 1 | 1 | 1 | 0 (unsigned), 0 (SNORM) |
| BC5 | 1 | 1 | 1 | 0, 0 (SNORM) |
| BC6H (UF16, after clamp to 8 bit) | 1 | 54 (not asserted) | 1 | n/a |
| BC7 | 0 | 0 | 0 (all 8 modes hit: 497/258/130/67/34/22/9/7 blocks) | n/a |
| BC6H_SF16 | n/a | n/a | 1 on the real signed image from bcdec's test suite (511566 of 524288 texels) | n/a |

Findings behind those rows, each reproducible with `build/venv/bin/python tests/textures/test_crosscheck.py`:

* The +-1 on BC1-BC5/BC6H is interpolation rounding (bcdec's 1/3, 2/3 fixed-point magic vs the others);
  bcdec's BC4/BC5 are *exactly* the float-then-round D3D spec (0 difference to the Python reference).
* texture2ddecoder mismatches random BC3 blocks by up to ~250 (cause not investigated; plausibly its
  handling of BC1's 3-colour mode, which BC3 never uses) so it is only used on real encoder output, where
  it agrees within 1; its BC6H disagrees with both Pillow and bcdec on some blocks (54 on ISPC output;
  Pillow and bcdec agree within 1), so that row is reported, not asserted.
* Pillow's *signed* BC6H is unreliable near zero (returns 255 for texels bcdec decodes to ~1e-4 whose
  neighbours agree with bcdec; random SF16 blocks disagree wildly) and Pillow's BC5_SNORM, read as a +128 bias, differs from the spec reference by up to 2. The SF16 check therefore uses the bcdec upstream test image
  (`external/bcdec/test_images/lythwood_room_1k_bc6h_signed.dds`, read in place, an upstream test asset, not
  game data) restricted to texels >= 0.01, and BC4/BC5 SNORM use the Python spec reference. **No
  independent float BC6H decoder was available** (both Python decoders output 8-bit clamped values), so
  HDR values above 1.0 are validated by round trip against the source image instead (median relative
  error 0.7 %, 99th percentile 4.2 % on a 0..16 range image) and by preservation of range.
* The ISPC python binding's BC4/BC5 output decodes consistently in all decoders but is only ~5 dB PSNR vs
  every source channel (cause not investigated); quality checks for BC4/BC5 use etcpak. This is a
  test-encoder quirk, not a decoder issue.

### Measured PSNR: BC -> ASTC (vs the bcdec decode of the same blob)

"cli" = astcenc CLI `-thorough -j 1` (offline tool), "lib" = `rltex encode --quality fast` (device path,
`ASTCENC_PRE_FAST`). Sets: smooth = gradient, colour wheel, height, normal map, HDR; detailed = multi-octave
"organic" (near-aliased at 37x23) and hard-edged checker; sizes 64x64 and 37x23 (partial blocks).
Range over the set; assertion = floor in the right-hand column (min measured minus about 2 dB).
HDR uses PSNR of log2(1+x) scaled so 16 -> 255.

| format (block) | cli smooth | cli detailed | lib smooth | lib detailed | asserted floor (cli sm/det, lib sm/det) |
|---|---|---|---|---|---|
| BC1 (6x6) | 35.4-43.2 | 23.2-99 | 33.8-36.6 | 22.3-48.7 | 33/21, 31/20 |
| BC1_SRGB (6x6) | 35.4-43.2 | 23.2-26.4 | 33.9-36.6 | 22.3-25.6 | 33/21, 31/20 |
| BC2 (4x4) | 46.9-52.7 | 32.1-35.0 | 37.6-47.2 | 28.2-30.8 | 44/30, 35/26 |
| BC3 (4x4) | 47.0-59.1 | 32.2-99 | 38.5-58.7 | 28.3-99 | 44/30, 36/26 |
| BC3_SRGB (4x4) | 59.4-63.1 | 32.3-35.1 | 58.5-61.6 | 28.8-31.0 | 57/30, 56/26 |
| BC4 (6x6) | 39.3-42.4 | 27.8-29.8 | 36.0-38.0 | 27.7-29.4 | 37/25, 34/25 |
| BC5 (4x4) | 42.8-46.2 | 28.9-30.9 | 41.6-45.2 | 27.3-29.1 | 40/26, 39/25 |
| BC6H -> ASTC HDR (4x4) | 48.2-54.5 | n/a | 45.8-52.4 | n/a | 46/-, 43/- |
| BC7 (4x4) | 41.6-48.8 | 29.3-32.7 | 37.8-45.0 | 27.6-30.8 | 39/27, 35/25 |
| BC7_SRGB (4x4) | 42.9-48.9 | 29.3-32.7 | 39.4-45.1 | 27.6-30.8 | 40/27, 37/25 |

("99" = bit-identical on exactly representable content.) The library "fast" path costs 2-5 dB against
"thorough", which is why the offline pass replaces on-device output.

### Cost on this host (i5-1135G7, single thread; relative guide only, A16 E-cores are slower; `bench.py`)

| texture | BC size | decoded (miss upload) | ASTC | `rltex encode` fast | peak RSS of that process | astcenc CLI thorough, 1 thread |
|---|---|---|---|---|---|---|
| 1024 BC1 | 0.5 MiB | 4 MiB | 0.45 MiB (6x6) | 0.12 s | 87 MiB | 9.1 s |
| 1024 BC3/BC7 | 1 MiB | 4 MiB | 1 MiB | 0.12-0.17 s | 95 MiB | 2.2-3.3 s |
| 2048 BC1 | 2 MiB | 16 MiB | 1.78 MiB | 0.31 s | 231 MiB | 23.3 s |
| 2048 BC3/BC7 | 4 MiB | 16 MiB | 4 MiB | 0.35 s | 231 MiB | 8.4-9.1 s |

Memory on a miss (uploaded uncompressed, per 2048x2048 level, without mips): BC1 2 MiB -> 16 MiB (8x),
BC3/BC7 4 -> 16 MiB (4x), BC4 2 -> 4 MiB (R8), BC5 4 -> 8 MiB (RG8), BC6H 4 -> **32 MiB** (RGBA16F, 8x).
A warm cache removes all of that (ASTC is <= BC size).

## 7. Proposed `DECISIONS.md` entries (not edited by this work)

**D-0xx: Texture cache key and file formats.** Key = SHA-256 of a fixed 32-byte little-endian header
(`RLK1`, DXGI format, mip width, mip height, mip level, array slice, blob size) followed by the tightly
packed BC blob of that subresource; cache files `<key>.astc` (ASTC file format, block size read from its
header) and `bc/<key>.bc` (`RLBC` v1 + the same header + blob); miss log `logs/texture_miss.log`. Full
definition in `docs/notes/textures.md`. Changing the header layout requires bumping `RLK1`.

**D-0xx: BC4 and BC5 are encoded as luminance / luminance+alpha with a fixed view swizzle.** `rrr1`
(view `r001`) and `rrrg` (view `ra01`) beat storing the channels as-is by 0.5-5 dB in measurements (`bench.py --swizzle`); DXMT must
apply the view swizzle on the ASTC texture view. Not configurable because the `.astc` file does not record it.

**D-0xx: BC4/BC5 miss path decodes to R8/RG8 (not RGBA8).** Exact D3D semantics, 1/4 and 1/2 the memory.

**D-0xx: `astc_bc3_block` is the group key for BC2/BC3/BC5/BC7**, with per-format overrides
(`astc_bc2_block`, `astc_bc5_block`, `astc_bc7_block`) and extra keys `astc_bc4_block`, `astc_bc6h_block`,
`astc_bc6h_mode`, `astc_runtime_quality`, `astc_encode_qos`, `astc_encode_queue`, `astc_encode_max_pixels`,
`astc_background_encode`. SNORM formats and BC6H SF16 are never ASTC-encoded.

**D-0xx: The on-device encoder is a stopgap bounded by size and queue.** Single worker, ASTC `fast`
preset, `QOS_CLASS_BACKGROUND`, textures above 2048x2048 are left for `encode.py`, pending jobs are dropped
at shutdown (their `.bc` remain).

## 8. Proposed `RISKS.md` entries

* **R-0xx: First-run memory with a cold texture cache.** Every miss uploads uncompressed (BC1 8x, BC6H 8x,
  BC3/7 4x). A cold cache on a texture-heavy scene could exceed the iOS memory limit. Mitigations: run the
  discovery loop so the cache is warm before real play (BC -> ASTC is never larger than BC), cap
  `astc_encode_max_pixels`, and let DXMT evict/replace miss-path textures when an `.astc` lands (not done:
  background results only take effect on the next launch).
* **R-0xx: On-device ASTC encode cost.** Measured on an x86-64 laptop core: 0.3-0.4 s per 2048x2048 at
  `fast`, ~230 MiB peak (about 40-50 MiB fixed for the astcenc context plus ~45 bytes per texel); the A16 efficiency cores will be several
  times slower and background QoS can be starved while the game runs. The encode worker competes for the
  memory budget (peak ~45 bytes/texel transient). Config knobs: `astc_encode_max_pixels`,
  `astc_runtime_quality`, `astc_background_encode = false`.
* **R-0xx: ASTC HDR on A16 is unverified.** `MTLPixelFormatASTC_4x4_HDR` availability for Apple GPU family
  8 could not be confirmed offline. Fallback `astc_bc6h_mode = "rgba16f"` (32 MiB per 2048x2048, 8x BC6H).
* **R-0xx: Quality loss BC1 -> ASTC 6x6** is 1-2 dB (smooth) / 2.3 dB (detailed) worse than 5x5 and 12-26 dB worse
  than 4x4 (table in section 3; 4x4 is 8 bpp, twice the BC1 size), and ASTC re-compresses already lossy BC data (double loss). Keep
  `astc_bc1_block` configurable and judge visually on device.
* **R-0xx: No independent float BC6H / SF16 reference.** bcdec's BC6H is validated against 8-bit clamped
  decoders and source images, not against a float reference; very large HDR values and signed BC6H are
  not independently verified.
* **R-0xx: Cache validity across game updates.** Keys are content hashes, so unchanged textures
  survive updates; stale `.astc` files for removed textures just waste space (no GC implemented).

## 9. Unverified / for the integrator

* Not run on iOS/ARM64: the `__APPLE__` branch only type-checks against stubs here; astc-encoder's NEON
  backend and the A16 encode speed are untested. Build it with the iOS toolchain (the library has no
  Linux-only code in its core; the only OS-specific calls are in `src/platform.c` and POSIX
  `mkdir/stat/rename`).
* Metal side (to confirm at integration against Apple docs): the `MTLPixelFormat` ASTC LDR / sRGB / HDR
  names per block size, `MTLTextureSwizzleChannels` for the view swizzles, and `os_proc_available_memory`
  gating. The pixel formats follow `rltex_astc_profile` (LDR -> `*_LDR`, LDR_SRGB -> `*_sRGB`,
  HDR_RGB_LDR_A -> `*_HDR`).
* DXMT must pass tightly packed per-mip blobs and the mip/slice indices; texture creation with more than
  one subresource calls `rltex_acquire` once per subresource.
* astc-encoder release builds use LTO, so linking the static library with the system linker needs
  `-fuse-ld=lld` (done in `tests/textures/build.sh`).
