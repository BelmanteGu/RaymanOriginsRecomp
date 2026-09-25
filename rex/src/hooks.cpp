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

// ---- Frame pacing ----
// sub_826D41B8 is the game's D3D Present (the only path to VdSwap, see
// docs/PROGRESS.md section 7). Wrapping it gives the real game frame rate,
// logged every 2 s: average FPS and the slowest frame.
#include <rex/hook.h>

#include <chrono>
#include <thread>

#if defined(__ANDROID__)
#include <android/log.h>
#define RAYMAN_LOG(...) __android_log_print(ANDROID_LOG_INFO, "RaymanPerf", __VA_ARGS__)
#else
#define RAYMAN_LOG(...) (std::fprintf(stderr, __VA_ARGS__), std::fputc('\n', stderr))
#endif

REX_EXTERN(__imp__sub_826D41B8);
void RaymanNativeCaptureFrame();     // native_capture.cpp
void RaymanNativeRendererPresent();  // native_renderer.cpp
void RaymanAutopilotFrame();         // autopilot.cpp

REX_HOOK_RAW(sub_826D41B8) {
  using clock = std::chrono::steady_clock;
  static clock::time_point window_start = clock::now(), last = window_start;
  static int frames = 0;
  static double worst_ms = 0;
  __imp__sub_826D41B8(ctx, base);
  // Frame limiter: the game's logic advances once per presented frame, so a
  // 120 Hz display (Galaxy S23, ProMotion Macs) would run it twice as fast.
  // RAYMAN_FPS_LIMIT overrides the default of 60 (0 disables it).
  static const double limit = [] {
    const char* v = std::getenv("RAYMAN_FPS_LIMIT");
    return v ? std::atof(v) : 60.0;
  }();
  if (limit > 0) {
    static clock::time_point next = clock::now();
    const auto period = std::chrono::duration_cast<clock::duration>(std::chrono::duration<double>(1.0 / limit));
    next += period;
    auto now_before = clock::now();
    if (next < now_before - period * 2) {
      next = now_before;  // fell behind (loading, a hitch): don't try to catch up
    } else {
      std::this_thread::sleep_until(next);
    }
  }
  RaymanNativeCaptureFrame();
  RaymanNativeRendererPresent();
  RaymanAutopilotFrame();
  auto now = clock::now();
  double ms = std::chrono::duration<double, std::milli>(now - last).count();
  last = now;
  worst_ms = ms > worst_ms ? ms : worst_ms;
  ++frames;
  double window = std::chrono::duration<double>(now - window_start).count();
  if (window >= 2.0) {
    RAYMAN_LOG("[perf] %.1f fps (avg %.1f ms, worst %.1f ms)", frames / window,
               window * 1000.0 / frames, worst_ms);
    window_start = now;
    frames = 0;
    worst_ms = 0;
  }
}
