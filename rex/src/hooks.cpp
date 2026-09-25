// Hooks do Rayman Origins no runtime ReXGlue.
#include <rex/ppc/context.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

uint8_t* g_rayman_membase = nullptr;  // definido em RaymanApp::OnPostSetup

// 256 amostras x 6 canais (FL, FR, C, LFE, RL, RR), float big-endian, planar.
void RaymanAudioDump(PPCRegister& r4) {
  static FILE* dump = [] {
    const char* path = std::getenv("RAYMAN_AUDIO_DUMP");
    return path ? std::fopen(path, "wb") : nullptr;
  }();
  if (!dump || !g_rayman_membase || r4.u32 == 0) {
    return;
  }
  const uint8_t* src = g_rayman_membase + r4.u32;
  auto sample = [&](int channel, int i) {
    uint32_t bits;
    std::memcpy(&bits, src + (channel * 256 + i) * 4, 4);
    bits = __builtin_bswap32(bits);
    float value;
    std::memcpy(&value, &bits, 4);
    return value;
  };
  float stereo[256 * 2];
  for (int i = 0; i < 256; ++i) {
    float c = sample(2, i);
    stereo[i * 2 + 0] = sample(0, i) + 0.707f * c + 0.707f * sample(4, i);
    stereo[i * 2 + 1] = sample(1, i) + 0.707f * c + 0.707f * sample(5, i);
  }
  std::fwrite(stereo, sizeof(stereo), 1, dump);
}
