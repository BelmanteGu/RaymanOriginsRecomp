// Xenos texture decoding for the native renderer: texture fetch constants,
// 2D tiling, endian swap and DXT1/3/5 / 8888 to RGBA8.
// GetTiledOffset2D is from ReXGlue / Xenia (texture_util, BSD-3-Clause).
#pragma once

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <functional>
#include <utility>
#include <vector>

namespace xenos {

inline uint32_t BE32(const uint8_t* p) { return uint32_t(p[0]) << 24 | p[1] << 16 | p[2] << 8 | p[3]; }

// Reads `size` bytes of guest physical memory at `address`, or null.
using MemoryReader = std::function<const uint8_t*(uint32_t address, uint32_t size)>;

struct TextureFetch {
  uint32_t format, endian, base, width, height, pitch;
  uint32_t clampX, clampY;  // 0 wrap, 1 mirror, 2+ clamp variants
  bool tiled;
  bool packedMips;  // small levels share one 32x32 tile (dword 5 bit 11)
};

// Texture fetch constant: 6 big-endian dwords (Xenos registers 0x4800 + 6*slot).
inline TextureFetch DecodeFetch(const uint8_t* p) {
  uint32_t d0 = BE32(p), d1 = BE32(p + 4), d2 = BE32(p + 8), d5 = BE32(p + 20);
  TextureFetch t;
  t.packedMips = (d5 >> 11) & 1;
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

inline int32_t GetTiledOffset2D(int32_t x, int32_t y, uint32_t pitch, uint32_t bytes_per_block_log2) {
  pitch = (pitch + 31) & ~31u;
  int32_t macro = ((x >> 5) + (y >> 5) * int32_t(pitch >> 5)) << (bytes_per_block_log2 + 7);
  int32_t micro = ((x & 7) + ((y & 0xE) << 2)) << bytes_per_block_log2;
  int32_t offset = macro + ((micro & ~0xF) << 1) + (micro & 0xF) + ((y & 1) << 4);
  return ((offset & ~0x1FF) << 3) + ((y & 16) << 7) + ((offset & 0x1C0) << 2) +
         (((((y & 8) >> 2) + (x >> 3)) & 3) << 6) + (offset & 0x3F);
}

inline uint32_t Log2Ceil(uint32_t v) {
  uint32_t l = 0;
  while ((1u << l) < v) ++l;
  return l;
}

// Where the base level of a small texture sits inside its packed mip tile, in
// texels (ReXGlue / Xenia texture_util::GetPackedMipOffset for mip 0, 2D).
inline void PackedBaseOffset(uint32_t width, uint32_t height, uint32_t& x, uint32_t& y) {
  x = y = 0;
  uint32_t lw = Log2Ceil(width), lh = Log2Ceil(height);
  if (std::min(lw, lh) > 4) return;  // shortest side > 16: not packed
  if (lw > lh) y = 16;               // wider than tall: laid out vertically
  else x = 16;                       // square or taller: laid out horizontally
}

// Xenos endian modes: 0 none, 1 8in16, 2 8in32, 3 16in32.
inline void Swap(uint8_t* p, size_t n, uint32_t endian) {
  for (size_t i = 0; i + 4 <= n; i += 4) {
    uint8_t* q = p + i;
    if (endian == 1) { std::swap(q[0], q[1]); std::swap(q[2], q[3]); }
    else if (endian == 2) { std::swap(q[0], q[3]); std::swap(q[1], q[2]); }
    else if (endian == 3) { std::swap(q[0], q[2]); std::swap(q[1], q[3]); }
  }
}

inline void Rgb565(uint16_t c, uint8_t out[3]) {
  out[0] = uint8_t(((c >> 11) & 31) * 255 / 31);
  out[1] = uint8_t(((c >> 5) & 63) * 255 / 63);
  out[2] = uint8_t((c & 31) * 255 / 31);
}

inline void DecodeColorBlock(const uint8_t* b, uint8_t rgba[16][4], bool dxt1) {
  uint16_t c0 = b[0] | b[1] << 8, c1 = b[2] | b[3] << 8;
  uint8_t pal[4][4];
  Rgb565(c0, pal[0]);
  Rgb565(c1, pal[1]);
  pal[0][3] = pal[1][3] = pal[2][3] = pal[3][3] = 255;
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

inline void DecodeAlphaDxt5(const uint8_t* b, uint8_t rgba[16][4]) {
  uint8_t a[8] = {b[0], b[1]};
  for (int i = 2; i < 8; ++i)
    a[i] = a[0] > a[1] ? uint8_t(((8 - i) * a[0] + (i - 1) * a[1]) / 7)
                       : (i < 6 ? uint8_t(((6 - i) * a[0] + (i - 1) * a[1]) / 5) : (i == 6 ? 0 : 255));
  uint64_t bits = 0;
  for (int i = 0; i < 6; ++i) bits |= uint64_t(b[2 + i]) << (8 * i);
  for (int i = 0; i < 16; ++i) rgba[i][3] = a[(bits >> (3 * i)) & 7];
}

inline bool Supported(uint32_t format) {
  return format == 0x12 || format == 0x13 || format == 0x14 || format == 0x06 || format == 0x02;
}

// Bytes per texel of the decoded image: k_8 stays one channel (R8, sampled
// through an RRRR swizzle), everything else becomes RGBA8.
inline uint32_t DecodedBytesPerTexel(uint32_t format) { return format == 0x02 ? 1 : 4; }
inline size_t DecodedSize(const TextureFetch& t) { return size_t(t.width) * t.height * DecodedBytesPerTexel(t.format); }

// Decodes the top mip of a 2D texture into `out` (DecodedSize bytes, tightly
// packed rows; R8 for k_8, RGBA8 otherwise).
inline bool DecodeTextureTo(const MemoryReader& memory, const TextureFetch& t, uint8_t* out) {
  bool block = t.format == 0x12 || t.format == 0x13 || t.format == 0x14;
  uint32_t blockBytes = t.format == 0x12 ? 8 : block ? 16 : t.format == 0x06 ? 4 : t.format == 0x02 ? 1 : 0;
  if (!blockBytes) return false;
  uint32_t log2 = blockBytes == 16 ? 4 : blockBytes == 8 ? 3 : blockBytes == 4 ? 2 : 0;
  uint32_t bw = block ? (t.width + 3) / 4 : t.width, bh = block ? (t.height + 3) / 4 : t.height;
  uint32_t pitch = std::max(t.pitch / (block ? 4 : 1), (bw + 31) & ~31u);
  uint32_t ox = 0, oy = 0;  // base level offset inside a packed mip tile, in blocks
  if (t.packedMips && t.tiled) {
    PackedBaseOffset(t.width, t.height, ox, oy);
    if (block) { ox /= 4; oy /= 4; }
  }
  // Bytes the level spans: tiled surfaces are laid out in 32x32-block macro
  // tiles over the 32-aligned pitch, so this bounds every offset.
  uint32_t span = t.tiled ? ((pitch + 31) & ~31u) * ((bh + oy + 31) & ~31u) * blockBytes
                          : ((bh - 1) * pitch + bw) * blockBytes;
  const uint8_t* src = memory(t.base, span);
  if (!src) return false;
  // Within a run of 8 blocks in x (x & ~7 fixed), the tiled offset is the
  // run's offset plus a delta that depends on x & 7 only: the address bits x
  // feeds there don't overlap the ones y feeds.
  int32_t delta[8];
  for (int32_t i = 0; i < 8; ++i) delta[i] = GetTiledOffset2D(i, 0, pitch, log2) - GetTiledOffset2D(0, 0, pitch, log2);
  const uint32_t outBpp = DecodedBytesPerTexel(t.format);
  // k_8 tiled: the 8 texels of a run are 8 contiguous bytes (delta 0..7), so
  // whole runs are copied at once and byte-swapped within their 32-bit groups
  // (movie planes, rewritten every frame).
  bool contiguous8 = blockBytes == 1 && t.tiled;
  for (int32_t i = 0; contiguous8 && i < 8; ++i) contiguous8 = delta[i] == i;
  for (uint32_t by = 0; by < bh; ++by) {
    uint32_t y = by + oy;
    int32_t runBase = 0;
    for (uint32_t bx = 0; bx < bw; ++bx) {
      uint32_t x = bx + ox;
      if (contiguous8 && (x & 7) == 0 && bx + 8 <= bw) {
        const uint8_t* s = src + GetTiledOffset2D(int32_t(x), int32_t(y), pitch, 0);
        uint8_t* o = &out[size_t(by) * t.width + bx];
        static constexpr uint8_t kLane[4][4] = {{0, 1, 2, 3}, {1, 0, 3, 2}, {3, 2, 1, 0}, {0, 1, 2, 3}};
        const uint8_t* lane = kLane[t.endian];  // same lane mapping as the per-texel path below
        for (int i = 0; i < 8; ++i) o[i] = s[(i & ~3) + lane[i & 3]];
        bx += 7;
        continue;
      }
      uint32_t off;
      if (t.tiled) {
        if (bx == 0 || (x & 7) == 0) runBase = GetTiledOffset2D(int32_t(x & ~7u), int32_t(y), pitch, log2);
        off = uint32_t(runBase + delta[x & 7]);
      } else {
        off = (by * pitch + bx) * blockBytes;
      }
      if (blockBytes == 1) {
        // k_8: one channel. The endian swap works on 32-bit groups, so read the
        // byte from its swapped position within the group.
        uint32_t lane = off & 3, swapped = t.endian == 2 ? 3 - lane : t.endian == 1 ? lane ^ 1 : lane;
        out[size_t(by) * t.width + bx] = src[(off & ~3u) + swapped];
        continue;
      }
      uint8_t blk[16];
      std::memcpy(blk, src + off, blockBytes);
      Swap(blk, blockBytes, t.endian);
      if (!block) {
        // D3DFMT_A8R8G8B8 on k_8_8_8_8: after the 8in32 swap the bytes are B G R A.
        uint8_t* o = &out[(size_t(by) * t.width + bx) * 4];
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
        uint32_t px_x = bx * 4 + (i & 3), px_y = by * 4 + i / 4;
        if (px_x < t.width && px_y < t.height) std::memcpy(&out[(size_t(px_y) * t.width + px_x) * outBpp], px[i], 4);
      }
    }
  }
  return true;
}

// Decodes the top mip to RGBA8 (width*height*4 bytes); k_8 is replicated to
// all four channels. For the offline tools.
inline bool DecodeTexture(const MemoryReader& memory, const TextureFetch& t, std::vector<uint8_t>& rgba) {
  std::vector<uint8_t> decoded(DecodedSize(t));
  if (!DecodeTextureTo(memory, t, decoded.data())) return false;
  if (DecodedBytesPerTexel(t.format) == 4) {
    rgba = std::move(decoded);
    return true;
  }
  rgba.resize(decoded.size() * 4);
  for (size_t i = 0; i < decoded.size(); ++i) rgba[i * 4] = rgba[i * 4 + 1] = rgba[i * 4 + 2] = rgba[i * 4 + 3] = decoded[i];
  return true;
}

}  // namespace xenos
