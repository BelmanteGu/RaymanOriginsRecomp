// Native renderer, step 1: observe what the game draws through its D3D layer.
//
// With RAYMAN_NATIVE_CAPTURE=1 the hooks below log:
//  - every shader the game creates, with the XXH3 hash XenosRecomp uses to key
//    its precompiled SPIR-V cache (hash of the container: function[1] + function[2] bytes);
//  - a per-frame summary of the draw calls: which draw entry point, primitive
//    type, and the vertex/pixel shader bound in the device at that moment.
// Everything is passed through to the original functions, so the Xenos
// emulation keeps rendering. Addresses and device offsets: docs/D3D_MAP.md.
#include <rex/hook.h>

#define XXH_INLINE_ALL
#include <xxhash.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <set>
#include <tuple>
#include <unordered_map>

#if defined(__ANDROID__)
#include <android/log.h>
#define CAPTURE_LOG(...) __android_log_print(ANDROID_LOG_INFO, "RaymanNative", __VA_ARGS__)
#else
#define CAPTURE_LOG(...) (std::fprintf(stderr, "[native] " __VA_ARGS__), std::fputc('\n', stderr))
#endif

extern uint8_t* g_rayman_membase;  // hooks.cpp

namespace {

constexpr uint32_t kDeviceVertexShader = 0x3310;
constexpr uint32_t kDevicePixelShader = 0x330C;

bool Enabled() {
  static const bool enabled = std::getenv("RAYMAN_NATIVE_CAPTURE") != nullptr;
  return enabled;
}

uint32_t LoadBE32(uint32_t guest_address) {
  uint32_t value;
  std::memcpy(&value, g_rayman_membase + guest_address, 4);
  return __builtin_bswap32(value);
}

struct ShaderInfo {
  uint64_t hash;
  bool vertex;
};

struct DrawKey {
  int entry;          // 0 = DrawIndexed (826D7588), 1 = Draw (826D7170), 2 = DrawUP (826D6C68)
  uint32_t primitive;
  uint64_t vs, ps;
  bool operator<(const DrawKey& o) const {
    return std::tie(entry, primitive, vs, ps) < std::tie(o.entry, o.primitive, o.vs, o.ps);
  }
};

std::mutex g_mutex;
std::unordered_map<uint32_t, ShaderInfo> g_shaders;  // guest shader object -> info
std::map<DrawKey, int> g_frameDraws;
int g_frameDrawCount = 0;
int g_unknownShaderDraws = 0;
uint64_t g_frame = 0;

uint64_t HashShader(uint32_t function) {
  // Container header (big-endian words): [1] + [2] = bytes XenosRecomp hashes.
  uint32_t size = LoadBE32(function + 4) + LoadBE32(function + 8);
  return XXH3_64bits(g_rayman_membase + function, size);
}

void RecordShader(uint32_t object, uint32_t function, bool vertex) {
  if (!object || !function) {
    return;
  }
  uint64_t hash = HashShader(function);
  std::lock_guard lock(g_mutex);
  g_shaders[object] = {hash, vertex};
  CAPTURE_LOG("%s shader 0x%08X hash %016llX", vertex ? "vertex" : "pixel", object,
              (unsigned long long)hash);
}

uint64_t ShaderHash(uint32_t object) {
  auto it = g_shaders.find(object);
  return it == g_shaders.end() ? 0 : it->second.hash;
}

void RecordDraw(int entry, uint32_t device, uint32_t primitive) {
  uint32_t vs = LoadBE32(device + kDeviceVertexShader);
  uint32_t ps = LoadBE32(device + kDevicePixelShader);
  std::lock_guard lock(g_mutex);
  DrawKey key{entry, primitive, ShaderHash(vs), ShaderHash(ps)};
  if ((vs && !key.vs) || (ps && !key.ps)) {
    ++g_unknownShaderDraws;
  }
  ++g_frameDraws[key];
  ++g_frameDrawCount;
}

}  // namespace

// Called from the Present hook (hooks.cpp) once per game frame.
void RaymanNativeCaptureFrame() {
  if (!Enabled()) {
    return;
  }
  std::lock_guard lock(g_mutex);
  ++g_frame;
  if (g_frame % 120 == 0) {
    CAPTURE_LOG("frame %llu: %d draws, %zu distinct (entry/prim/shaders), %d with an unknown shader, %zu shaders known",
                (unsigned long long)g_frame, g_frameDrawCount, g_frameDraws.size(),
                g_unknownShaderDraws, g_shaders.size());
    for (auto& [key, count] : g_frameDraws) {
      CAPTURE_LOG("  entry %d prim %u vs %016llX ps %016llX x%d", key.entry, key.primitive,
                  (unsigned long long)key.vs, (unsigned long long)key.ps, count);
    }
  }
  g_frameDraws.clear();
  g_frameDrawCount = 0;
  g_unknownShaderDraws = 0;
}

REX_EXTERN(__imp__sub_826CD850);  // CreateVertexShader(function) -> shader
REX_EXTERN(__imp__sub_826CD668);  // CreatePixelShader(function) -> shader
REX_EXTERN(__imp__sub_826D7588);  // DrawIndexed
REX_EXTERN(__imp__sub_826D7170);  // Draw
REX_EXTERN(__imp__sub_826D6C68);  // DrawUP

REX_HOOK_RAW(sub_826CD850) {
  uint32_t function = ctx.r3.u32;
  __imp__sub_826CD850(ctx, base);
  if (Enabled()) {
    RecordShader(ctx.r3.u32, function, true);
  }
}

REX_HOOK_RAW(sub_826CD668) {
  uint32_t function = ctx.r3.u32;
  __imp__sub_826CD668(ctx, base);
  if (Enabled()) {
    RecordShader(ctx.r3.u32, function, false);
  }
}

REX_HOOK_RAW(sub_826D7588) {
  if (Enabled()) {
    RecordDraw(0, ctx.r3.u32, ctx.r4.u32);
  }
  __imp__sub_826D7588(ctx, base);
}

REX_HOOK_RAW(sub_826D7170) {
  if (Enabled()) {
    RecordDraw(1, ctx.r3.u32, ctx.r4.u32);
  }
  __imp__sub_826D7170(ctx, base);
}

REX_HOOK_RAW(sub_826D6C68) {
  if (Enabled()) {
    RecordDraw(2, ctx.r3.u32, ctx.r4.u32);
  }
  __imp__sub_826D6C68(ctx, base);
}
