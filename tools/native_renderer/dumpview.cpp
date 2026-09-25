// dumpview: reads a frame dump written by rex/src/native_capture.cpp
// (captures/native_frame.bin), prints its draw calls and decodes every bound
// texture to a TGA file. First step of the offline native renderer.
//
// Usage: dumpview <native_frame.bin> <output dir>
//
// Texture decoding: Xenos 2D tiling (GetTiledOffset2D below is from ReXGlue /
// Xenia, BSD-3-Clause), endian swap, then DXT1/3/5 or 8888 to RGBA8.
// The output is derived from the game: keep it out of git.
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace dv {

struct Draw {
  uint32_t entry, prim, a5, a6, a7, a8;
  uint64_t vs, ps;
  uint32_t ib, ibWord0, ibAddress;
  std::vector<uint8_t> device;  // device + 0x480 .. + 0x3700 (big-endian)
};

struct Dump {
  std::vector<Draw> draws;
  std::map<uint32_t, std::vector<uint8_t>> memory;  // physical address -> bytes
  std::map<uint64_t, std::vector<uint8_t>> shaders;
};

constexpr uint32_t kDumpBegin = 0x480, kDumpEnd = 0x3700;

uint32_t BE32(const uint8_t* p) { return uint32_t(p[0]) << 24 | p[1] << 16 | p[2] << 8 | p[3]; }

bool Load(const char* path, Dump& out) {
  FILE* f = std::fopen(path, "rb");
  if (!f) return false;
  char magic[4];
  if (std::fread(magic, 1, 4, f) != 4 || std::memcmp(magic, "RNF1", 4)) return false;
  auto get32 = [&] { uint32_t v = 0; std::fread(&v, 4, 1, f); return v; };
  auto get64 = [&] { uint64_t v = 0; std::fread(&v, 8, 1, f); return v; };
  for (;;) {
    uint32_t tag = get32();
    if (tag == 0x57415244) {  // DRAW
      Draw d;
      d.entry = get32(); d.prim = get32(); d.a5 = get32(); d.a6 = get32(); d.a7 = get32(); d.a8 = get32();
      d.vs = get64(); d.ps = get64();
      d.ib = get32(); d.ibWord0 = get32(); d.ibAddress = get32();
      d.device.resize(kDumpEnd - kDumpBegin);
      std::fread(d.device.data(), 1, d.device.size(), f);
      out.draws.push_back(std::move(d));
    } else if (tag == 0x204D454D) {  // MEM
      uint32_t address = get32(), size = get32();
      auto& bytes = out.memory[address];
      bytes.resize(size);
      std::fread(bytes.data(), 1, size, f);
    } else if (tag == 0x52444853) {  // SHDR
      uint64_t hash = get64();
      get32();
      uint32_t size = get32();
      auto& bytes = out.shaders[hash];
      bytes.resize(size);
      std::fread(bytes.data(), 1, size, f);
    } else {
      break;  // END (or truncated)
    }
  }
  std::fclose(f);
  return true;
}

// Bytes of guest physical memory at [address, address+size) if the dump has them.
const uint8_t* Memory(const Dump& dump, uint32_t address, uint32_t size) {
  auto it = dump.memory.upper_bound(address);
  if (it == dump.memory.begin()) return nullptr;
  --it;
  if (address + size > it->first + it->second.size()) return nullptr;
  return it->second.data() + (address - it->first);
}

// ReXGlue/Xenia texture_util::GetTiledOffset2D (BSD-3-Clause).
int32_t GetTiledOffset2D(int32_t x, int32_t y, uint32_t pitch, uint32_t bytes_per_block_log2) {
  pitch = (pitch + 31) & ~31u;
  int32_t macro = ((x >> 5) + (y >> 5) * int32_t(pitch >> 5)) << (bytes_per_block_log2 + 7);
  int32_t micro = ((x & 7) + ((y & 0xE) << 2)) << bytes_per_block_log2;
  int32_t offset = macro + ((micro & ~0xF) << 1) + (micro & 0xF) + ((y & 1) << 4);
  return ((offset & ~0x1FF) << 3) + ((y & 16) << 7) + ((offset & 0x1C0) << 2) +
         (((((y & 8) >> 2) + (x >> 3)) & 3) << 6) + (offset & 0x3F);
}

struct TextureFetch {
  uint32_t format, endian, base, width, height, pitch;
  uint32_t clampX, clampY;  // 0 wrap, 1 mirror, 2+ clamp variants
  bool tiled;
};

TextureFetch DecodeFetch(const uint8_t* p) {
  uint32_t d0 = BE32(p), d1 = BE32(p + 4), d2 = BE32(p + 8);
  TextureFetch t;
  t.tiled = d0 >> 31;
  t.clampX = (d0 >> 10) & 7;
  t.clampY = (d0 >> 13) & 7;
  t.pitch = ((d0 >> 22) & 0x1FF) * 32;
  t.format = d1 & 0x3F;
  t.endian = (d1 >> 6) & 3;
  t.base = d1 & 0xFFFFF000;
  t.width = (d2 & 0x1FFF) + 1;
  t.height = ((d2 >> 13) & 0x1FFF) + 1;
  return t;
}

// Xenos endian modes: 0 none, 1 8in16, 2 8in32, 3 16in32.
void Swap(uint8_t* p, size_t n, uint32_t endian) {
  for (size_t i = 0; i + 4 <= n; i += 4) {
    uint8_t* q = p + i;
    if (endian == 1) { std::swap(q[0], q[1]); std::swap(q[2], q[3]); }
    else if (endian == 2) { std::swap(q[0], q[3]); std::swap(q[1], q[2]); }
    else if (endian == 3) { std::swap(q[0], q[2]); std::swap(q[1], q[3]); }
  }
}

void Rgb565(uint16_t c, uint8_t out[3]) {
  out[0] = uint8_t(((c >> 11) & 31) * 255 / 31);
  out[1] = uint8_t(((c >> 5) & 63) * 255 / 63);
  out[2] = uint8_t((c & 31) * 255 / 31);
}

// Decodes one 4x4 color block (DXT1 layout, little-endian after swap).
void DecodeColorBlock(const uint8_t* b, uint8_t rgba[16][4], bool dxt1) {
  uint16_t c0 = b[0] | b[1] << 8, c1 = b[2] | b[3] << 8;
  uint8_t pal[4][4];
  Rgb565(c0, pal[0]); Rgb565(c1, pal[1]);
  pal[0][3] = pal[1][3] = pal[2][3] = 255;
  pal[3][3] = 255;
  for (int k = 0; k < 3; ++k) {
    if (c0 > c1 || !dxt1) {
      pal[2][k] = uint8_t((2 * pal[0][k] + pal[1][k]) / 3);
      pal[3][k] = uint8_t((pal[0][k] + 2 * pal[1][k]) / 3);
    } else {
      pal[2][k] = uint8_t((pal[0][k] + pal[1][k]) / 2);
      pal[3][k] = 0;
    }
  }
  if (dxt1 && c0 <= c1) pal[3][3] = 0;
  uint32_t bits = b[4] | b[5] << 8 | b[6] << 16 | uint32_t(b[7]) << 24;
  for (int i = 0; i < 16; ++i) std::memcpy(rgba[i], pal[(bits >> (2 * i)) & 3], 4);
}

void DecodeAlphaDxt5(const uint8_t* b, uint8_t rgba[16][4]) {
  uint8_t a[8] = {b[0], b[1]};
  for (int i = 2; i < 8; ++i)
    a[i] = a[0] > a[1] ? uint8_t(((8 - i) * a[0] + (i - 1) * a[1]) / 7)
                       : (i < 6 ? uint8_t(((6 - i) * a[0] + (i - 1) * a[1]) / 5) : (i == 6 ? 0 : 255));
  uint64_t bits = 0;
  for (int i = 0; i < 6; ++i) bits |= uint64_t(b[2 + i]) << (8 * i);
  for (int i = 0; i < 16; ++i) rgba[i][3] = a[(bits >> (3 * i)) & 7];
}

bool DecodeTexture(const Dump& dump, const TextureFetch& t, std::vector<uint8_t>& rgba) {
  bool block = t.format == 0x12 || t.format == 0x13 || t.format == 0x14;
  uint32_t blockBytes = t.format == 0x12 ? 8 : block ? 16 : t.format == 0x06 ? 4 : 0;
  if (!blockBytes) return false;
  uint32_t log2 = blockBytes == 16 ? 4 : blockBytes == 8 ? 3 : 2;
  uint32_t bw = block ? (t.width + 3) / 4 : t.width, bh = block ? (t.height + 3) / 4 : t.height;
  uint32_t pitch = std::max(t.pitch / (block ? 4 : 1), (bw + 31) & ~31u);
  uint32_t span = 0;
  for (uint32_t by = 0; by < bh; ++by)
    for (uint32_t bx = 0; bx < bw; ++bx) {
      uint32_t off = t.tiled ? uint32_t(GetTiledOffset2D(bx, by, pitch, log2)) : (by * pitch + bx) * blockBytes;
      span = std::max(span, off + blockBytes);
    }
  const uint8_t* src = Memory(dump, t.base, span);
  if (!src) return false;
  rgba.assign(size_t(t.width) * t.height * 4, 0);
  for (uint32_t by = 0; by < bh; ++by) {
    for (uint32_t bx = 0; bx < bw; ++bx) {
      uint32_t off = t.tiled ? uint32_t(GetTiledOffset2D(bx, by, pitch, log2)) : (by * pitch + bx) * blockBytes;
      if (off + blockBytes > span) continue;
      uint8_t blk[16];
      std::memcpy(blk, src + off, blockBytes);
      Swap(blk, blockBytes, t.endian);
      if (!block) {
        // D3DFMT_A8R8G8B8 on k_8_8_8_8: after the 8in32 swap the bytes are B G R A.
        uint8_t* o = &rgba[(size_t(by) * t.width + bx) * 4];
        o[0] = blk[2]; o[1] = blk[1]; o[2] = blk[0]; o[3] = blk[3];
        continue;
      }
      uint8_t px[16][4];
      if (t.format == 0x12) {
        DecodeColorBlock(blk, px, true);
      } else {
        DecodeColorBlock(blk + 8, px, false);
        if (t.format == 0x14) {
          DecodeAlphaDxt5(blk, px);
        } else {
          for (int i = 0; i < 16; ++i) px[i][3] = uint8_t(((blk[i / 2] >> (4 * (i & 1))) & 15) * 17);
        }
      }
      for (int i = 0; i < 16; ++i) {
        uint32_t x = bx * 4 + (i & 3), y = by * 4 + i / 4;
        if (x < t.width && y < t.height) std::memcpy(&rgba[(size_t(y) * t.width + x) * 4], px[i], 4);
      }
    }
  }
  return true;
}

void WriteTga(const std::string& path, uint32_t w, uint32_t h, const std::vector<uint8_t>& rgba) {
  FILE* f = std::fopen(path.c_str(), "wb");
  if (!f) return;
  uint8_t header[18] = {0, 0, 2};
  header[12] = w & 255; header[13] = w >> 8; header[14] = h & 255; header[15] = h >> 8;
  header[16] = 32; header[17] = 0x28;  // 8 alpha bits, top-left origin
  std::fwrite(header, 1, 18, f);
  for (size_t i = 0; i < size_t(w) * h; ++i) {
    uint8_t bgra[4] = {rgba[i * 4 + 2], rgba[i * 4 + 1], rgba[i * 4], rgba[i * 4 + 3]};
    std::fwrite(bgra, 1, 4, f);
  }
  std::fclose(f);
}


// ---- Software reconstruction of the frame (renderpct shaders) ----
// Mirrors renderpct_vsh1 / renderpct_psh1 from XenosRecomp's HLSL:
//   pos = WVP (c0..c3) * position, uv through the UV matrix (c4..c7),
//   color = vertex color * c13, pixel = texture * color, alpha blended.
float BEf(const uint8_t* p) { uint32_t v = BE32(p); float f; std::memcpy(&f, &v, 4); return f; }

struct Vec4 { float x, y, z, w; };

Vec4 Const(const Draw& d, uint32_t base, int index) {
  const uint8_t* p = d.device.data() + (base - kDumpBegin) + index * 16;
  return {BEf(p), BEf(p + 4), BEf(p + 8), BEf(p + 12)};
}

struct Vert { float sx, sy, invw, u, v; float c[4]; };

void RenderFrame(const Dump& dump, const char* out, std::map<uint32_t, std::vector<uint8_t>>& texCache,
                 const std::map<uint32_t, TextureFetch>& fetches) {
  const int W = 1280, H = 720;
  std::vector<float> fb(size_t(W) * H * 4, 0.0f);
  int drawn = 0, skipped = 0;
  for (const Draw& d : dump.draws) {
    if (d.entry != 0 || d.prim != 4 || !d.ibAddress) { ++skipped; continue; }
    uint32_t isize = (d.ibWord0 & 0x80000000u) ? 4 : 2;
    uint32_t ibPhys = (d.ibAddress & 0x1FFFFFFF) + (d.ibAddress >= 0xE0000000u ? 0x1000 : 0);
    const uint8_t* idx = Memory(dump, ibPhys + d.a6 * isize, d.a7 * isize);
    const uint8_t* vf = d.device.data() + 95 * 8;
    uint32_t vbase = BE32(vf) & 0x1FFFFFFC, vsize = ((BE32(vf + 4) >> 2) & 0xFFFFFF) * 4;
    const uint8_t* vb = Memory(dump, vbase, vsize);
    if (!idx || !vb) { ++skipped; continue; }
    Vec4 m[4] = {Const(d, 0x780, 0), Const(d, 0x780, 1), Const(d, 0x780, 2), Const(d, 0x780, 3)};
    Vec4 uvm[4] = {Const(d, 0x780, 4), Const(d, 0x780, 5), Const(d, 0x780, 6), Const(d, 0x780, 7)};
    Vec4 tint = Const(d, 0x780, 13);
    // Pixel shader 3A608A3C: lerp(texture * color, c16.rgb, c16.a) (depth fog).
    bool fog = d.ps == 0x3A608A3C9A37D236ull;
    Vec4 fogColor = Const(d, 0x1780, 16);
    float bbox[4] = {1e9f, 1e9f, -1e9f, -1e9f};
    // Texture 0.
    const uint8_t* tp = d.device.data();
    const std::vector<uint8_t>* tex = nullptr;
    TextureFetch tf{};
    if ((BE32(tp) & 3) == 2) {
      tf = DecodeFetch(tp);
      auto it = texCache.find(tf.base);
      if (it == texCache.end()) {
        std::vector<uint8_t> rgba;
        if (!DecodeTexture(dump, tf, rgba)) rgba.clear();
        it = texCache.emplace(tf.base, std::move(rgba)).first;
      }
      if (!it->second.empty()) tex = &it->second;
    }
    auto vertex = [&](uint32_t i) {
      const uint8_t* p = vb + size_t(i) * 24;
      float x = BEf(p), y = BEf(p + 4), z = BEf(p + 8);
      float u = BEf(p + 16), v = BEf(p + 20);
      Vec4 c;
      c.x = m[0].x * x + m[0].y * y + m[0].z * z + m[0].w;
      c.y = m[1].x * x + m[1].y * y + m[1].z * z + m[1].w;
      c.w = m[3].x * x + m[3].y * y + m[3].z * z + m[3].w;
      // UV transform, as in renderpct_vsh1.
      float rx = 2 * v - 1 + uvm[3].y, rz = 2 * u - 1 + uvm[3].x;
      float ry2 = rx * uvm[0].y + rz * uvm[0].x + 1;
      float rx2 = rx * uvm[1].y + rz * uvm[1].x + 1;
      float tu = uvm[0].w + ry2 * 0.5f, tv = rx2 * 0.5f + uvm[1].w;
      Vert o;
      o.invw = c.w != 0 ? 1.0f / c.w : 0;
      o.sx = (c.x * o.invw * 0.5f + 0.5f) * W;
      o.sy = (1 - (c.y * o.invw * 0.5f + 0.5f)) * H;
      o.u = tu; o.v = tv;
      const uint8_t* col = p + 12;  // D3DCOLOR, big-endian ARGB
      float a = col[0] / 255.f, r = col[1] / 255.f, g = col[2] / 255.f, b = col[3] / 255.f;
      o.c[0] = r * tint.x; o.c[1] = g * tint.y; o.c[2] = b * tint.z; o.c[3] = a * tint.w;
      return o;
    };
    for (uint32_t t = 0; t + 2 < d.a7; t += 3) {
      Vert v[3];
      bool bad = false;
      for (int k = 0; k < 3; ++k) {
        uint32_t i = isize == 2 ? (uint32_t(idx[(t + k) * 2]) << 8 | idx[(t + k) * 2 + 1]) : BE32(idx + (t + k) * 4);
        i += d.a5;
        if ((size_t(i) + 1) * 24 > vsize) { bad = true; break; }
        v[k] = vertex(i);
        if (v[k].invw <= 0) bad = true;
      }
      if (bad) continue;
      float minx = std::min({v[0].sx, v[1].sx, v[2].sx}), maxx = std::max({v[0].sx, v[1].sx, v[2].sx});
      float miny = std::min({v[0].sy, v[1].sy, v[2].sy}), maxy = std::max({v[0].sy, v[1].sy, v[2].sy});
      int x0 = std::max(0, int(minx)), x1 = std::min(W - 1, int(maxx) + 1);
      int y0 = std::max(0, int(miny)), y1 = std::min(H - 1, int(maxy) + 1);
      float area = (v[1].sx - v[0].sx) * (v[2].sy - v[0].sy) - (v[2].sx - v[0].sx) * (v[1].sy - v[0].sy);
      if (std::fabs(area) < 1e-6f) continue;
      bbox[0] = std::min(bbox[0], minx); bbox[1] = std::min(bbox[1], miny);
      bbox[2] = std::max(bbox[2], maxx); bbox[3] = std::max(bbox[3], maxy);
      for (int y = y0; y <= y1; ++y) {
        for (int x = x0; x <= x1; ++x) {
          float px = x + 0.5f, py = y + 0.5f;
          float w0 = ((v[1].sx - px) * (v[2].sy - py) - (v[2].sx - px) * (v[1].sy - py)) / area;
          float w1 = ((v[2].sx - px) * (v[0].sy - py) - (v[0].sx - px) * (v[2].sy - py)) / area;
          float w2 = 1 - w0 - w1;
          if (w0 < 0 || w1 < 0 || w2 < 0) continue;
          // Perspective-correct interpolation.
          float iw = w0 * v[0].invw + w1 * v[1].invw + w2 * v[2].invw;
          float b0 = w0 * v[0].invw / iw, b1 = w1 * v[1].invw / iw, b2 = w2 * v[2].invw / iw;
          float col[4];
          for (int k = 0; k < 4; ++k) col[k] = b0 * v[0].c[k] + b1 * v[1].c[k] + b2 * v[2].c[k];
          if (tex) {
            float u = b0 * v[0].u + b1 * v[1].u + b2 * v[2].u;
            float vv = b0 * v[0].v + b1 * v[1].v + b2 * v[2].v;
            auto address = [](float c, uint32_t mode) {
              if (mode == 0) return c - std::floor(c);                       // wrap
              if (mode == 1) { float m = c - 2 * std::floor(c / 2); return m > 1 ? 2 - m : m; }  // mirror
              return std::clamp(c, 0.f, 1.f);                                 // clamp
            };
            u = address(u, tf.clampX);
            vv = address(vv, tf.clampY);
            int tx = std::clamp(int(u * tf.width), 0, int(tf.width) - 1);
            int ty = std::clamp(int(vv * tf.height), 0, int(tf.height) - 1);
            const uint8_t* texel = &(*tex)[(size_t(ty) * tf.width + tx) * 4];
            for (int k = 0; k < 4; ++k) col[k] *= texel[k] / 255.f;
          }
          if (fog) {
            for (int k = 0; k < 3; ++k) col[k] = col[k] + ((&fogColor.x)[k] - col[k]) * fogColor.w;
          }
          float a = std::clamp(col[3], 0.f, 1.f);
          float* dst = &fb[(size_t(y) * W + x) * 4];
          for (int k = 0; k < 3; ++k) dst[k] = dst[k] * (1 - a) + std::clamp(col[k], 0.f, 1.f) * a;
          dst[3] = 1;
        }
      }
    }
    if (std::getenv("DUMPVIEW_PROBE")) {
      float px = std::atof(std::getenv("DUMPVIEW_PROBE")), py = std::atof(std::strchr(std::getenv("DUMPVIEW_PROBE"), ',') + 1);
      if (px >= bbox[0] && px <= bbox[2] && py >= bbox[1] && py <= bbox[3])
        std::printf("probe hit: draw %d vs %016llX ps %016llX tex %08X f%02X %ux%u clamp %u/%u tint (%.2f %.2f %.2f %.2f) bbox %.0f,%.0f-%.0f,%.0f\n",
                    drawn, (unsigned long long)d.vs, (unsigned long long)d.ps, tex ? tf.base : 0, tf.format, tf.width, tf.height,
                    tf.clampX, tf.clampY, tint.x, tint.y, tint.z, tint.w, bbox[0], bbox[1], bbox[2], bbox[3]);
    }
    ++drawn;
  }
  std::vector<uint8_t> rgba(size_t(W) * H * 4);
  for (size_t i = 0; i < rgba.size(); ++i) rgba[i] = uint8_t(std::clamp(fb[i], 0.f, 1.f) * 255.f + 0.5f);
  WriteTga(out, W, H, rgba);
  std::printf("rendered %d draws (%d skipped) -> %s\n", drawn, skipped, out);
}

}  // namespace dv

#ifndef DUMPVIEW_NO_MAIN
int main(int argc, char** argv) {
  using namespace dv;
  if (argc < 3) {
    std::fprintf(stderr, "usage: %s native_frame.bin outdir\n", argv[0]);
    return 1;
  }
  Dump dump;
  if (!Load(argv[1], dump)) {
    std::fprintf(stderr, "cannot read %s\n", argv[1]);
    return 1;
  }
  size_t memBytes = 0;
  for (auto& [a, b] : dump.memory) memBytes += b.size();
  std::printf("%zu draws, %zu memory ranges (%.1f MB), %zu shaders\n", dump.draws.size(),
              dump.memory.size(), memBytes / 1048576.0, dump.shaders.size());

  std::map<uint32_t, TextureFetch> textures;  // base -> fetch
  for (size_t i = 0; i < dump.draws.size(); ++i) {
    const Draw& d = dump.draws[i];
    std::string bound;
    for (uint32_t s = 0; s < 16; ++s) {
      const uint8_t* p = d.device.data() + s * 24;
      if ((BE32(p) & 3) != 2) continue;
      TextureFetch t = DecodeFetch(p);
      textures[t.base] = t;
      char buf[64];
      std::snprintf(buf, sizeof(buf), " t%u=%08X(%ux%u f%02X)", s, t.base, t.width, t.height, t.format);
      bound += buf;
    }
    if (i < 12 || i + 3 >= dump.draws.size())
      std::printf("draw %3zu entry %u prim %u base %u start %u count %u vs %016llX ps %016llX%s\n", i,
                  d.entry, d.prim, d.a5, d.a6, d.a7, (unsigned long long)d.vs, (unsigned long long)d.ps,
                  bound.c_str());
  }
  int ok = 0, fail = 0;
  for (auto& [base, t] : textures) {
    std::vector<uint8_t> rgba;
    if (DecodeTexture(dump, t, rgba)) {
      char name[128];
      std::snprintf(name, sizeof(name), "%s/tex_%08X_%ux%u_f%02X.tga", argv[2], base, t.width, t.height, t.format);
      WriteTga(name, t.width, t.height, rgba);
      ++ok;
    } else {
      std::printf("texture %08X %ux%u format %02X: not decoded\n", base, t.width, t.height, t.format);
      ++fail;
    }
  }
  std::printf("textures: %d decoded, %d not decoded\n", ok, fail);
  std::map<uint32_t, std::vector<uint8_t>> cache;
  RenderFrame(dump, (std::string(argv[2]) + "/frame_native.tga").c_str(), cache, textures);
  return 0;
}
#endif  // DUMPVIEW_NO_MAIN
