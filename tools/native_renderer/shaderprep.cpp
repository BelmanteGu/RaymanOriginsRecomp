// shaderprep: extracts every Xbox 360 shader container the game can create
// and names it by the hash the native renderer looks up at runtime:
// XXH3_64 over the container's first (word[1] + word[2]) bytes.
//
// Sources: the compiled UbiArt shaders in bootsequence_X360.ipk
// (shaders/compiled/x360/*.ckd) and the Direct3D shaders embedded in the
// executable image (containers found by their 0x102A1100/0x102A1101 magic).
//
// Usage: shaderprep <bootsequence_X360.ipk> <image.bin from tools/diag/imagedump> <out dir>
// Writes <HASH>_vs.bin / <HASH>_ps.bin. The output is derived from the game:
// keep it out of git (private/).
#define XXH_INLINE_ALL
#include <xxhash.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <set>
#include <string>
#include <vector>

namespace {

uint32_t BE32(const uint8_t* p) { return uint32_t(p[0]) << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
uint64_t BE64(const uint8_t* p) { return uint64_t(BE32(p)) << 32 | BE32(p + 4); }

std::vector<uint8_t> ReadFile(const char* path) {
  std::vector<uint8_t> data;
  if (FILE* f = std::fopen(path, "rb")) {
    std::fseek(f, 0, SEEK_END);
    data.resize(size_t(std::ftell(f)));
    std::fseek(f, 0, SEEK_SET);
    size_t n = std::fread(data.data(), 1, data.size(), f);
    data.resize(n);
    std::fclose(f);
  }
  return data;
}

std::set<uint64_t> g_written;

// Container: magic 0x102A1100 (pixel) / 0x102A1101 (vertex), word[1] + word[2] = hashed size.
bool Emit(const uint8_t* p, size_t available, const std::string& dir) {
  if (available < 16) return false;
  uint32_t magic = BE32(p);
  if (magic != 0x102A1100 && magic != 0x102A1101) return false;
  uint64_t size = uint64_t(BE32(p + 4)) + BE32(p + 8);
  if (size < 16 || size > available || size > (1u << 20)) return false;
  uint64_t hash = XXH3_64bits(p, size_t(size));
  if (!g_written.insert(hash).second) return true;
  char name[64];
  std::snprintf(name, sizeof(name), "/%016llX_%s.bin", (unsigned long long)hash, magic == 0x102A1101 ? "vs" : "ps");
  if (FILE* f = std::fopen((dir + name).c_str(), "wb")) {
    std::fwrite(p, 1, size_t(size), f);
    std::fclose(f);
  }
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 4) {
    std::fprintf(stderr, "usage: %s bootsequence_X360.ipk image.bin outdir\n", argv[0]);
    return 1;
  }
  std::string dir = argv[3];

  // IPK v3: header {magic, version, ?, data base, count}, entries from 0x30 up to
  // the data base: {offset count, size, zsize, u64 time, u64 offsets[count],
  // u32 name length, UTF-16BE path}.
  std::vector<uint8_t> ipk = ReadFile(argv[1]);
  int fromIpk = 0;
  if (ipk.size() > 0x30 && BE32(ipk.data()) == 0x50EC12BA) {
    uint32_t base = BE32(ipk.data() + 12);
    size_t p = 0x30;
    while (p + 24 < base && p + 24 < ipk.size()) {
      uint32_t count = BE32(&ipk[p]), size = BE32(&ipk[p + 4]), zsize = BE32(&ipk[p + 8]);
      p += 20;
      uint64_t offset = BE64(&ipk[p]);
      p += 8 * count;
      uint32_t len = BE32(&ipk[p]);
      p += 4;
      std::string name;
      for (uint32_t i = 0; i < len && p + 2 * i + 1 < ipk.size(); ++i) name += char(ipk[p + 2 * i + 1]);
      p += 2 * len;
      if (zsize == 0 && name.find("shaders/compiled/x360/") != std::string::npos && base + offset + size <= ipk.size())
        fromIpk += Emit(&ipk[base + offset], size, dir);
    }
  }

  std::vector<uint8_t> image = ReadFile(argv[2]);
  int fromImage = 0;
  for (size_t i = 0; i + 16 <= image.size(); i += 4) fromImage += Emit(&image[i], image.size() - i, dir);

  std::printf("%d shaders from the bundle, %d from the executable, %zu unique\n", fromIpk, fromImage, g_written.size());
  return 0;
}
