// D3D11 BC1-BC7 texture test (patches/dxmt/0100 and up): the same program runs against DXMT with native
// BC (macOS GPU), with DXMT_DEBUG_FORCE_NO_BC=1 (everything below goes through the emulation),
// and with the ASTC cache enabled (initial data is then served from cached ASTC).
//
// For each format x upload path: build a texture from hand-made BC blocks (vectors.h, from
// gen_vectors.py), draw every mip of it, texel for texel (shader: Load), to an RGBA8 render
// target, read the target back through a staging texture and compare with the colours the D3D11
// block-compression rules give for those blocks. One PASS / FAIL line per format x path; the exit
// status is the number of failures.
//
//   bc_textures.exe [--flat] [--tol N] [--filter SUBSTRING] [--variants ABC] [--sync] [--verbose]
//     --flat    one colour per subresource (the ASTC cache run: ASTC reproduces it within a few levels)
//     --tol N   allowed |difference| per channel (default 3, 8 with --flat)
//     --sync    wait for the GPU with an event query before every readback (diagnostics)
//     --variants  sizes to run: A 8x8 (4 mips), B 10x6 (4 mips, not a multiple of 4), C 2x2 (2 mips)
//
// Paths (see docs/notes/dxmt-bc.md for the DXMT code each one exercises):
//   caps                CheckFormatSupport: sampleable, not a render target
//   init-immutable      CreateTexture2D(IMMUTABLE, initial data)
//   init-default        CreateTexture2D(DEFAULT, initial data)
//   update-full         UpdateSubresource, whole subresource, padded row pitch
//   update-box          UpdateSubresource into block-aligned boxes
//   dynamic-map         DYNAMIC texture: Map(WRITE_DISCARD) + Unmap
//   dynamic-init        DYNAMIC texture with initial data
//   staging-copy        STAGING Map(WRITE) + CopyResource to DEFAULT
//   staging-region      STAGING Map(WRITE) + CopySubresourceRegion (two half-height boxes) to DEFAULT
//   copy-default        DEFAULT -> DEFAULT CopyResource
//   copy-region         DEFAULT -> DEFAULT CopySubresourceRegion (two half-height boxes)
//   readback            DEFAULT -> STAGING CopyResource, Map(READ): the original blocks come back
#include <d3d11.h>
#include <dxgi.h>
#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "vectors.h"
#include "shaders.h"

static ID3D11Device *g_dev;
static ID3D11DeviceContext *g_ctx;
static ID3D11VertexShader *g_vs;
static ID3D11PixelShader *g_ps[4];  // sample_l at level 0..3
static ID3D11SamplerState *g_samp;
static ID3D11InputLayout *g_il;
static ID3D11Buffer *g_vb;
static ID3D11RasterizerState *g_rs;
static bool g_flat = false;
static bool g_verbose = false;
static bool g_sync = false;  // --sync: wait for the GPU (event query) before every readback
static int g_tol = -1;
static std::string g_variants_sel = "ABC";  // --variants: which sizes to run (A 8x8, B 10x6, C 2x2)

template <class T> static void rel(T *&p) {
  if (p) {
    p->Release();
    p = nullptr;
  }
}

template <class T> struct Ref {
  T *p = nullptr;
  Ref() {}
  ~Ref() { rel(p); }
  Ref(const Ref &) = delete;
  Ref &operator=(const Ref &) = delete;
  T **operator&() { return &p; }
  T *operator->() const { return p; }
  operator T *() const { return p; }
};

static UINT cols_of(UINT w) { return (w + 3) / 4; }

static std::string hr_str(HRESULT hr) {
  char b[32];
  snprintf(b, sizeof b, "hr=0x%08x", (unsigned)hr);
  return b;
}

// ------------------------------------------------------------------------------------------
// block helpers
// ------------------------------------------------------------------------------------------
// The vector's blocks (tight) re-laid out with `pitch` bytes per block row.
static std::vector<uint8_t> with_pitch(const SubVec &sv, const FormatInfo &fi, UINT pitch) {
  UINT cols = cols_of(sv.w), rows = cols_of(sv.h), row_bytes = cols * fi.block_bytes;
  std::vector<uint8_t> out((size_t)pitch * rows, 0xEE);
  for (UINT r = 0; r < rows; r++)
    memcpy(out.data() + (size_t)r * pitch, sv.blocks + (size_t)r * row_bytes, row_bytes);
  return out;
}

// Blocks [bx0, bx0 + cols) x [by0, by0 + rows) of the vector, tight.
static std::vector<uint8_t> sub_blocks(const SubVec &sv, const FormatInfo &fi, UINT bx0, UINT by0, UINT cols, UINT rows) {
  UINT full_cols = cols_of(sv.w);
  std::vector<uint8_t> out((size_t)cols * rows * fi.block_bytes);
  for (UINT r = 0; r < rows; r++)
    memcpy(out.data() + (size_t)r * cols * fi.block_bytes,
           sv.blocks + ((size_t)(by0 + r) * full_cols + bx0) * fi.block_bytes, (size_t)cols * fi.block_bytes);
  return out;
}

static D3D11_TEXTURE2D_DESC make_desc(const FormatInfo &fi, const VariantInfo &vi, UINT mips, D3D11_USAGE usage, UINT bind,
                                      UINT cpu) {
  D3D11_TEXTURE2D_DESC d = {};
  d.Width = vi.w;
  d.Height = vi.h;
  d.MipLevels = mips;
  d.ArraySize = 1;
  d.Format = (DXGI_FORMAT)fi.tex_format;
  d.SampleDesc.Count = 1;
  d.Usage = usage;
  d.BindFlags = bind;
  d.CPUAccessFlags = cpu;
  return d;
}

// ------------------------------------------------------------------------------------------
// verification: draw a mip, read it back, compare
// ------------------------------------------------------------------------------------------
// Blocks until the GPU has executed everything submitted so far (event query).
static void wait_idle() {
  D3D11_QUERY_DESC qd = {D3D11_QUERY_EVENT, 0};
  ID3D11Query *q = nullptr;
  if (FAILED(g_dev->CreateQuery(&qd, &q)))
    return;
  g_ctx->End(q);
  g_ctx->Flush();
  BOOL done = FALSE;
  for (int i = 0; i < 20000 && !done; i++) {
    if (g_ctx->GetData(q, &done, sizeof done, 0) != S_OK)
      Sleep(1);
  }
  q->Release();
}

static bool verify_mip(ID3D11Texture2D *tex, const FormatInfo &fi, UINT mip, const SubVec &sv, std::string &err) {
  const int tol = g_tol >= 0 ? g_tol : (g_flat ? 8 : 3);
  HRESULT hr;

  D3D11_SHADER_RESOURCE_VIEW_DESC sd = {};
  sd.Format = (DXGI_FORMAT)fi.srv_format;
  sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
  sd.Texture2D.MostDetailedMip = 0;
  sd.Texture2D.MipLevels = (UINT)-1;  // the whole chain: the shader picks the level (sample_l)
  Ref<ID3D11ShaderResourceView> srv;
  if (FAILED(hr = g_dev->CreateShaderResourceView(tex, &sd, &srv))) {
    err = "CreateShaderResourceView " + hr_str(hr);
    return false;
  }

  D3D11_TEXTURE2D_DESC rd = {};
  rd.Width = sv.w;
  rd.Height = sv.h;
  rd.MipLevels = rd.ArraySize = 1;
  rd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  rd.SampleDesc.Count = 1;
  rd.Usage = D3D11_USAGE_DEFAULT;
  rd.BindFlags = D3D11_BIND_RENDER_TARGET;
  Ref<ID3D11Texture2D> rt;
  if (FAILED(hr = g_dev->CreateTexture2D(&rd, nullptr, &rt))) {
    err = "CreateTexture2D(render target) " + hr_str(hr);
    return false;
  }
  Ref<ID3D11RenderTargetView> rtv;
  if (FAILED(hr = g_dev->CreateRenderTargetView(rt, nullptr, &rtv))) {
    err = "CreateRenderTargetView " + hr_str(hr);
    return false;
  }
  rd.Usage = D3D11_USAGE_STAGING;
  rd.BindFlags = 0;
  rd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  Ref<ID3D11Texture2D> stg;
  if (FAILED(hr = g_dev->CreateTexture2D(&rd, nullptr, &stg))) {
    err = "CreateTexture2D(staging) " + hr_str(hr);
    return false;
  }

  const float sentinel[4] = {0.2f, 0.4f, 0.6f, 0.8f};
  g_ctx->ClearRenderTargetView(rtv, sentinel);
  ID3D11RenderTargetView *rtvs[1] = {rtv};
  g_ctx->OMSetRenderTargets(1, rtvs, nullptr);
  D3D11_VIEWPORT vp = {0, 0, (float)sv.w, (float)sv.h, 0, 1};
  g_ctx->RSSetViewports(1, &vp);
  g_ctx->RSSetState(g_rs);
  UINT stride = 24, offset = 0;
  g_ctx->IASetInputLayout(g_il);
  g_ctx->IASetVertexBuffers(0, 1, &g_vb, &stride, &offset);
  g_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  g_ctx->VSSetShader(g_vs, nullptr, 0);
  g_ctx->PSSetShader(g_ps[mip], nullptr, 0);
  g_ctx->PSSetSamplers(0, 1, &g_samp);
  ID3D11ShaderResourceView *srvs[1] = {srv};
  g_ctx->PSSetShaderResources(0, 1, srvs);
  g_ctx->Draw(3, 0);
  ID3D11ShaderResourceView *none[1] = {nullptr};
  g_ctx->PSSetShaderResources(0, 1, none);
  g_ctx->OMSetRenderTargets(0, nullptr, nullptr);
  g_ctx->CopyResource(stg, rt);
  if (g_sync)
    wait_idle();

  D3D11_MAPPED_SUBRESOURCE m;
  if (FAILED(hr = g_ctx->Map(stg, 0, D3D11_MAP_READ, 0, &m))) {
    err = "Map(readback) " + hr_str(hr);
    return false;
  }
  bool ok = true;
  int worst = 0;
  for (UINT y = 0; y < sv.h && ok; y++) {
    const uint8_t *row = (const uint8_t *)m.pData + (size_t)y * m.RowPitch;
    for (UINT x = 0; x < sv.w; x++) {
      const uint8_t *e = sv.expect + ((size_t)y * sv.w + x) * 4;
      int dmax = 0;
      for (int c = 0; c < 4; c++)
        dmax = std::max(dmax, abs((int)row[x * 4 + c] - (int)e[c]));
      worst = std::max(worst, dmax);
      if (dmax > tol) {
        char b[200];
        snprintf(b, sizeof b, "mip %u texel (%u,%u): got %02x%02x%02x%02x expected %02x%02x%02x%02x (tol %d)", mip, x, y,
                 row[x * 4], row[x * 4 + 1], row[x * 4 + 2], row[x * 4 + 3], e[0], e[1], e[2], e[3], tol);
        err = b;
        ok = false;
        break;
      }
    }
  }
  g_ctx->Unmap(stg, 0);
  if (ok && g_verbose)
    printf("  mip %u ok (%ux%u, worst diff %d)\n", mip, sv.w, sv.h, worst);
  return ok;
}

// Pipeline self-test with an ordinary RGBA8 texture of four mips (8x8 .. 1x1): if this fails, the draw /
// readback machinery or the device's mip views are at fault, not the BC support.
static bool selftest_rgba8(std::string &err) {
  const UINT mips = 4;
  std::vector<std::vector<uint8_t>> px(mips);
  std::vector<D3D11_SUBRESOURCE_DATA> sd(mips);
  for (UINT m = 0; m < mips; m++) {
    UINT w = 8 >> m, h = 8 >> m;
    px[m].resize(w * h * 4);
    for (UINT y = 0; y < h; y++)
      for (UINT x = 0; x < w; x++) {
        uint8_t *p = &px[m][(y * w + x) * 4];
        p[0] = (uint8_t)(x * 30 + 10 + m * 50);
        p[1] = (uint8_t)(y * 25 + 9);
        p[2] = (uint8_t)(m * 60 + x + y);
        p[3] = (uint8_t)(255 - x - y);
      }
    sd[m] = {px[m].data(), w * 4, 0};
  }
  D3D11_TEXTURE2D_DESC d = {};
  d.Width = d.Height = 8;
  d.MipLevels = mips;
  d.ArraySize = 1;
  d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  d.SampleDesc.Count = 1;
  d.Usage = D3D11_USAGE_DEFAULT;
  d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
  Ref<ID3D11Texture2D> tex;
  HRESULT hr = g_dev->CreateTexture2D(&d, sd.data(), &tex);
  if (FAILED(hr)) {
    err = "CreateTexture2D " + hr_str(hr);
    return false;
  }
  FormatInfo fi = {"RGBA8", DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R8G8B8A8_UNORM, 0, 4, 0};
  for (UINT m = 0; m < mips; m++) {
    UINT w = 8 >> m, h = 8 >> m;
    SubVec sv = {px[m].data(), (unsigned)px[m].size(), px[m].data(), w, h};
    if (!verify_mip(tex, fi, m, sv, err))
      return false;
  }
  return true;
}

static bool verify_all(ID3D11Texture2D *tex, const FormatInfo &fi, const VariantInfo &vi, UINT vidx, UINT mips, const SubVec (*vec)[4],
                       std::string &err) {
  for (UINT m = 0; m < mips; m++) {
    if (!verify_mip(tex, fi, m, vec[vidx][m], err)) {
      err = std::string("variant ") + vi.name + " " + err;
      return false;
    }
  }
  return true;
}

// ------------------------------------------------------------------------------------------
// paths. Each runs for every variant (8x8 / 10x6 / 2x2) and returns false on the first failure.
// ------------------------------------------------------------------------------------------
typedef bool (*PathFn)(UINT f, UINT v, std::string &err);

#define VEC (g_vec[f][g_flat ? 1 : 0])

static bool create_with_data(UINT f, UINT v, D3D11_USAGE usage, UINT cpu, UINT mips, ID3D11Texture2D **out, std::string &err) {
  const FormatInfo &fi = g_formats[f];
  const VariantInfo &vi = g_variants[v];
  std::vector<D3D11_SUBRESOURCE_DATA> data(mips);
  for (UINT m = 0; m < mips; m++) {
    const SubVec &sv = VEC[v][m];
    data[m].pSysMem = sv.blocks;
    data[m].SysMemPitch = cols_of(sv.w) * fi.block_bytes;
    data[m].SysMemSlicePitch = 0;
  }
  D3D11_TEXTURE2D_DESC d = make_desc(fi, vi, mips, usage, D3D11_BIND_SHADER_RESOURCE, cpu);
  HRESULT hr = g_dev->CreateTexture2D(&d, data.data(), out);
  if (FAILED(hr)) {
    err = "CreateTexture2D(initial data) " + hr_str(hr);
    return false;
  }
  return true;
}

static bool path_init(UINT f, UINT v, D3D11_USAGE usage, std::string &err) {
  Ref<ID3D11Texture2D> tex;
  if (!create_with_data(f, v, usage, 0, g_variants[v].mips, &tex, err))
    return false;
  return verify_all(tex, g_formats[f], g_variants[v], v, g_variants[v].mips, VEC, err);
}
static bool path_init_immutable(UINT f, UINT v, std::string &err) { return path_init(f, v, D3D11_USAGE_IMMUTABLE, err); }
static bool path_init_default(UINT f, UINT v, std::string &err) { return path_init(f, v, D3D11_USAGE_DEFAULT, err); }

static bool create_empty(UINT f, UINT v, UINT mips, D3D11_USAGE usage, UINT bind, UINT cpu, ID3D11Texture2D **out, std::string &err) {
  D3D11_TEXTURE2D_DESC d = make_desc(g_formats[f], g_variants[v], mips, usage, bind, cpu);
  HRESULT hr = g_dev->CreateTexture2D(&d, nullptr, out);
  if (FAILED(hr)) {
    err = "CreateTexture2D " + hr_str(hr);
    return false;
  }
  return true;
}

static bool path_update_full(UINT f, UINT v, std::string &err) {
  const FormatInfo &fi = g_formats[f];
  UINT mips = g_variants[v].mips;
  Ref<ID3D11Texture2D> tex;
  if (!create_empty(f, v, mips, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE, 0, &tex, err))
    return false;
  for (UINT m = 0; m < mips; m++) {
    const SubVec &sv = VEC[v][m];
    UINT pitch = cols_of(sv.w) * fi.block_bytes + (m & 1 ? 32 : 0);  // odd mips: padded rows
    auto data = with_pitch(sv, fi, pitch);
    g_ctx->UpdateSubresource(tex, m, nullptr, data.data(), pitch, 0);
  }
  return verify_all(tex, fi, g_variants[v], v, mips, VEC, err);
}

static bool path_update_box(UINT f, UINT v, std::string &err) {
  const FormatInfo &fi = g_formats[f];
  UINT mips = g_variants[v].mips;
  Ref<ID3D11Texture2D> tex;
  if (!create_empty(f, v, mips, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE, 0, &tex, err))
    return false;
  for (UINT m = 0; m < mips; m++) {
    const SubVec &sv = VEC[v][m];
    UINT cols = cols_of(sv.w), rows = cols_of(sv.h);
    if (cols < 2) {  // a single block column: whole subresource
      auto data = with_pitch(sv, fi, cols * fi.block_bytes);
      g_ctx->UpdateSubresource(tex, m, nullptr, data.data(), cols * fi.block_bytes, 0);
      continue;
    }
    // two boxes side by side: block columns [0, cols-1) and the last one (the edge for 10-wide mips)
    UINT split = cols - 1;
    D3D11_BOX left = {0, 0, 0, split * 4, sv.h, 1};
    D3D11_BOX right = {split * 4, 0, 0, sv.w, sv.h, 1};
    auto a = sub_blocks(sv, fi, 0, 0, split, rows);
    auto b = sub_blocks(sv, fi, split, 0, 1, rows);
    g_ctx->UpdateSubresource(tex, m, &right, b.data(), fi.block_bytes, 0);
    g_ctx->UpdateSubresource(tex, m, &left, a.data(), split * fi.block_bytes, 0);
  }
  return verify_all(tex, fi, g_variants[v], v, mips, VEC, err);
}

static bool path_dynamic_map(UINT f, UINT v, std::string &err) {
  const FormatInfo &fi = g_formats[f];
  Ref<ID3D11Texture2D> tex;
  if (!create_empty(f, v, 1, D3D11_USAGE_DYNAMIC, D3D11_BIND_SHADER_RESOURCE, D3D11_CPU_ACCESS_WRITE, &tex, err))
    return false;
  const SubVec &sv = VEC[v][0];
  D3D11_MAPPED_SUBRESOURCE m;
  HRESULT hr = g_ctx->Map(tex, 0, D3D11_MAP_WRITE_DISCARD, 0, &m);
  if (FAILED(hr)) {
    err = "Map(WRITE_DISCARD) " + hr_str(hr);
    return false;
  }
  UINT cols = cols_of(sv.w), rows = cols_of(sv.h);
  for (UINT r = 0; r < rows; r++)
    memcpy((uint8_t *)m.pData + (size_t)r * m.RowPitch, sv.blocks + (size_t)r * cols * fi.block_bytes, cols * fi.block_bytes);
  g_ctx->Unmap(tex, 0);
  return verify_all(tex, fi, g_variants[v], v, 1, VEC, err);
}

static bool path_dynamic_init(UINT f, UINT v, std::string &err) {
  Ref<ID3D11Texture2D> tex;
  if (!create_with_data(f, v, D3D11_USAGE_DYNAMIC, D3D11_CPU_ACCESS_WRITE, 1, &tex, err))
    return false;
  return verify_all(tex, g_formats[f], g_variants[v], v, 1, VEC, err);
}

static bool make_staging_written(UINT f, UINT v, ID3D11Texture2D **out, std::string &err) {
  const FormatInfo &fi = g_formats[f];
  UINT mips = g_variants[v].mips;
  if (!create_empty(f, v, mips, D3D11_USAGE_STAGING, 0, D3D11_CPU_ACCESS_WRITE | D3D11_CPU_ACCESS_READ, out, err))
    return false;
  for (UINT m = 0; m < mips; m++) {
    const SubVec &sv = VEC[v][m];
    D3D11_MAPPED_SUBRESOURCE mp;
    HRESULT hr = g_ctx->Map(*out, m, D3D11_MAP_WRITE, 0, &mp);
    if (FAILED(hr)) {
      err = "Map(staging WRITE) " + hr_str(hr);
      return false;
    }
    UINT cols = cols_of(sv.w), rows = cols_of(sv.h);
    for (UINT r = 0; r < rows; r++)
      memcpy((uint8_t *)mp.pData + (size_t)r * mp.RowPitch, sv.blocks + (size_t)r * cols * fi.block_bytes, cols * fi.block_bytes);
    g_ctx->Unmap(*out, m);
  }
  return true;
}

static bool path_staging_copy(UINT f, UINT v, std::string &err) {
  Ref<ID3D11Texture2D> stg, tex;
  if (!make_staging_written(f, v, &stg, err))
    return false;
  UINT mips = g_variants[v].mips;
  if (!create_empty(f, v, mips, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE, 0, &tex, err))
    return false;
  g_ctx->CopyResource(tex, stg);
  return verify_all(tex, g_formats[f], g_variants[v], v, mips, VEC, err);
}

// Copies `src` subresources into `dst` with two half-height boxes where a mip has two block rows.
static void copy_regions(ID3D11Texture2D *dst, ID3D11Texture2D *src, UINT v, UINT mips, UINT f) {
  for (UINT m = 0; m < mips; m++) {
    const SubVec &sv = VEC[v][m];
    if (sv.h > 4) {
      D3D11_BOX top = {0, 0, 0, sv.w, 4, 1};
      D3D11_BOX bottom = {0, 4, 0, sv.w, sv.h, 1};
      g_ctx->CopySubresourceRegion(dst, m, 0, 4, 0, src, m, &bottom);
      g_ctx->CopySubresourceRegion(dst, m, 0, 0, 0, src, m, &top);
    } else {
      g_ctx->CopySubresourceRegion(dst, m, 0, 0, 0, src, m, nullptr);
    }
  }
}

static bool path_staging_region(UINT f, UINT v, std::string &err) {
  Ref<ID3D11Texture2D> stg, tex;
  if (!make_staging_written(f, v, &stg, err))
    return false;
  UINT mips = g_variants[v].mips;
  if (!create_empty(f, v, mips, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE, 0, &tex, err))
    return false;
  copy_regions(tex, stg, v, mips, f);
  return verify_all(tex, g_formats[f], g_variants[v], v, mips, VEC, err);
}

static bool path_copy_default(UINT f, UINT v, std::string &err) {
  Ref<ID3D11Texture2D> src, dst;
  UINT mips = g_variants[v].mips;
  if (!create_with_data(f, v, D3D11_USAGE_DEFAULT, 0, mips, &src, err))
    return false;
  if (!create_empty(f, v, mips, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE, 0, &dst, err))
    return false;
  g_ctx->CopyResource(dst, src);
  return verify_all(dst, g_formats[f], g_variants[v], v, mips, VEC, err);
}

static bool path_copy_region(UINT f, UINT v, std::string &err) {
  Ref<ID3D11Texture2D> src, dst;
  UINT mips = g_variants[v].mips;
  if (!create_with_data(f, v, D3D11_USAGE_DEFAULT, 0, mips, &src, err))
    return false;
  if (!create_empty(f, v, mips, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE, 0, &dst, err))
    return false;
  copy_regions(dst, src, v, mips, f);
  return verify_all(dst, g_formats[f], g_variants[v], v, mips, VEC, err);
}

static bool path_readback(UINT f, UINT v, std::string &err) {
  const FormatInfo &fi = g_formats[f];
  Ref<ID3D11Texture2D> src, stg;
  UINT mips = g_variants[v].mips;
  if (!create_with_data(f, v, D3D11_USAGE_DEFAULT, 0, mips, &src, err))
    return false;
  if (!create_empty(f, v, mips, D3D11_USAGE_STAGING, 0, D3D11_CPU_ACCESS_READ, &stg, err))
    return false;
  g_ctx->CopyResource(stg, src);
  for (UINT m = 0; m < mips; m++) {
    const SubVec &sv = VEC[v][m];
    D3D11_MAPPED_SUBRESOURCE mp;
    HRESULT hr = g_ctx->Map(stg, m, D3D11_MAP_READ, 0, &mp);
    if (FAILED(hr)) {
      err = "Map(staging READ) " + hr_str(hr);
      return false;
    }
    UINT cols = cols_of(sv.w), rows = cols_of(sv.h);
    bool same = true;
    for (UINT r = 0; r < rows; r++)
      same &= memcmp((const uint8_t *)mp.pData + (size_t)r * mp.RowPitch, sv.blocks + (size_t)r * cols * fi.block_bytes,
                     cols * fi.block_bytes) == 0;
    g_ctx->Unmap(stg, m);
    if (!same) {
      char b[96];
      snprintf(b, sizeof b, "variant %s mip %u: blocks read back differ from the blocks written", g_variants[v].name, m);
      err = b;
      return false;
    }
  }
  return true;
}

static bool path_caps(UINT f, UINT v, std::string &err) {
  UINT s = 0;
  HRESULT hr = g_dev->CheckFormatSupport((DXGI_FORMAT)g_formats[f].tex_format, &s);
  if (FAILED(hr)) {
    err = "CheckFormatSupport " + hr_str(hr);
    return false;
  }
  const UINT need = D3D11_FORMAT_SUPPORT_TEXTURE2D | D3D11_FORMAT_SUPPORT_SHADER_SAMPLE | D3D11_FORMAT_SUPPORT_SHADER_LOAD;
  const UINT forbid = D3D11_FORMAT_SUPPORT_RENDER_TARGET | D3D11_FORMAT_SUPPORT_BLENDABLE | D3D11_FORMAT_SUPPORT_TYPED_UNORDERED_ACCESS_VIEW |
                      D3D11_FORMAT_SUPPORT_DEPTH_STENCIL;
  if ((s & need) != need || (s & forbid)) {
    char b[96];
    snprintf(b, sizeof b, "support flags 0x%08x (need 0x%x, forbid 0x%x)", s, need, forbid);
    err = b;
    return false;
  }
  return true;
}

struct PathDesc {
  const char *name;
  PathFn fn;
  bool per_variant;
};

static const PathDesc g_paths[] = {
    {"caps", path_caps, false},
    {"init-immutable", path_init_immutable, true},
    {"init-default", path_init_default, true},
    {"update-full", path_update_full, true},
    {"update-box", path_update_box, true},
    {"dynamic-map", path_dynamic_map, true},
    {"dynamic-init", path_dynamic_init, true},
    {"staging-copy", path_staging_copy, true},
    {"staging-region", path_staging_region, true},
    {"copy-default", path_copy_default, true},
    {"copy-region", path_copy_region, true},
    {"readback", path_readback, true},
};

// ------------------------------------------------------------------------------------------
static bool init_device() {
  D3D_FEATURE_LEVEL fl;
  HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &g_dev, &fl, &g_ctx);
  if (FAILED(hr)) {
    printf("FAIL setup: D3D11CreateDevice %s\n", hr_str(hr).c_str());
    return false;
  }
  {
    IDXGIDevice *dxgi = nullptr;
    if (SUCCEEDED(g_dev->QueryInterface(__uuidof(IDXGIDevice), (void **)&dxgi))) {
      IDXGIAdapter *ad = nullptr;
      if (SUCCEEDED(dxgi->GetAdapter(&ad))) {
        DXGI_ADAPTER_DESC desc;
        if (SUCCEEDED(ad->GetDesc(&desc))) {
          char name[128];
          WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, name, sizeof name, nullptr, nullptr);
          printf("INFO adapter: %s, feature level 0x%x\n", name, (unsigned)fl);
        }
        ad->Release();
      }
      dxgi->Release();
    }
  }
  if (FAILED(hr = g_dev->CreateVertexShader(g_vs_blob, sizeof g_vs_blob, nullptr, &g_vs))) {
    printf("FAIL setup: CreateVertexShader %s\n", hr_str(hr).c_str());
    return false;
  }
  for (int m = 0; m < 4; m++) {
    if (FAILED(hr = g_dev->CreatePixelShader(g_ps_blobs[m], g_ps_blob_size, nullptr, &g_ps[m]))) {
      printf("FAIL setup: CreatePixelShader(level %d) %s\n", m, hr_str(hr).c_str());
      return false;
    }
  }
  D3D11_INPUT_ELEMENT_DESC el[2] = {
      {"POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
      {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 16, D3D11_INPUT_PER_VERTEX_DATA, 0},
  };
  if (FAILED(hr = g_dev->CreateInputLayout(el, 2, g_vs_blob, sizeof g_vs_blob, &g_il))) {
    printf("FAIL setup: CreateInputLayout %s\n", hr_str(hr).c_str());
    return false;
  }
  // one oversized triangle covering the viewport; uv = (0,0) top-left .. (1,1) bottom-right
  const float tri[3][6] = {{-1, -1, 0, 1, 0, 1}, {-1, 3, 0, 1, 0, -1}, {3, -1, 0, 1, 2, 1}};
  D3D11_BUFFER_DESC bd = {};
  bd.ByteWidth = sizeof tri;
  bd.Usage = D3D11_USAGE_IMMUTABLE;
  bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
  D3D11_SUBRESOURCE_DATA sd = {tri, 0, 0};
  if (FAILED(hr = g_dev->CreateBuffer(&bd, &sd, &g_vb))) {
    printf("FAIL setup: CreateBuffer %s\n", hr_str(hr).c_str());
    return false;
  }
  D3D11_SAMPLER_DESC sm = {};
  sm.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
  sm.AddressU = sm.AddressV = sm.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
  sm.ComparisonFunc = D3D11_COMPARISON_NEVER;
  sm.MinLOD = 0;
  sm.MaxLOD = 16;
  if (FAILED(hr = g_dev->CreateSamplerState(&sm, &g_samp))) {
    printf("FAIL setup: CreateSamplerState %s\n", hr_str(hr).c_str());
    return false;
  }
  D3D11_RASTERIZER_DESC rs = {};
  rs.FillMode = D3D11_FILL_SOLID;
  rs.CullMode = D3D11_CULL_NONE;
  rs.DepthClipEnable = TRUE;
  if (FAILED(hr = g_dev->CreateRasterizerState(&rs, &g_rs))) {
    printf("FAIL setup: CreateRasterizerState %s\n", hr_str(hr).c_str());
    return false;
  }
  return true;
}

int main(int argc, char **argv) {
  setvbuf(stdout, nullptr, _IOLBF, 0);
  std::string filter;
  for (int i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "--flat"))
      g_flat = true;
    else if (!strcmp(argv[i], "--verbose"))
      g_verbose = true;
    else if (!strcmp(argv[i], "--sync"))
      g_sync = true;
    else if (!strcmp(argv[i], "--tol") && i + 1 < argc)
      g_tol = atoi(argv[++i]);
    else if (!strcmp(argv[i], "--filter") && i + 1 < argc)
      filter = argv[++i];
    else if (!strcmp(argv[i], "--variants") && i + 1 < argc)
      g_variants_sel = argv[++i];
    else {
      printf("usage: bc_textures [--flat] [--tol N] [--filter S] [--variants ABC] [--sync] [--verbose]\n");
      return 2;
    }
  }
  const char *no_bc = getenv("DXMT_DEBUG_FORCE_NO_BC");
  const char *cache = getenv("DXMT_BC_CACHE_DIR");
  printf("INFO mode: %s content, tolerance %d, DXMT_DEBUG_FORCE_NO_BC=%s, DXMT_BC_CACHE_DIR=%s\n", g_flat ? "flat" : "pattern",
         g_tol >= 0 ? g_tol : (g_flat ? 8 : 3), no_bc ? no_bc : "(unset)", cache ? cache : "(unset)");
  if (!init_device())
    return 1;

  int failures = 0, total = 0;
  for (int rep = 1; rep <= 3; rep++) {
    std::string err;
    total++;
    if (selftest_rgba8(err)) {
      printf("PASS selftest rgba8-mips-%d\n", rep);
    } else {
      printf("FAIL selftest rgba8-mips-%d: %s\n", rep, err.c_str());
      failures++;
    }
  }
  for (UINT f = 0; f < NUM_VEC_FORMATS; f++) {
    for (const PathDesc &p : g_paths) {
      std::string label = std::string(g_formats[f].name) + " " + p.name;
      if (!filter.empty() && label.find(filter) == std::string::npos)
        continue;
      std::string err;
      bool ok = true;
      if (!p.per_variant) {
        ok = p.fn(f, 0, err);
      } else {
        for (UINT v = 0; v < sizeof g_variants / sizeof g_variants[0] && ok; v++) {
          if (g_variants_sel.find(g_variants[v].name[0]) == std::string::npos)
            continue;
          // dynamic textures carry one mip; the other paths use the whole chain
          ok = p.fn(f, v, err);
        }
      }
      total++;
      if (ok) {
        printf("PASS %s\n", label.c_str());
      } else {
        printf("FAIL %s: %s\n", label.c_str(), err.c_str());
        failures++;
      }
    }
  }
  printf("SUMMARY %d checks, %d failed\n", total, failures);

  rel(g_rs);
  rel(g_vb);
  rel(g_il);
  for (auto &p : g_ps)
    rel(p);
  rel(g_samp);
  rel(g_vs);
  rel(g_ctx);
  rel(g_dev);
  return failures;
}
