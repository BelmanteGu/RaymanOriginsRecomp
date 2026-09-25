// Native renderer, in-game (shadow mode): with RAYMAN_NATIVE_RENDER=1 a second
// window draws every game frame with the native renderer
// (tools/native_renderer/vk_renderer.h: Vulkan + the game's SPIR-V shaders),
// next to the Xenos emulation, which keeps running for comparison.
//
// SPIR-V is read from RAYMAN_NATIVE_SPIRV (default: private/native/shaders_by_hash,
// relative to the repository root), as <HASH>_vs.spv / <HASH>_ps.spv.
#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <mutex>
#include <string>

#include "vk_renderer.h"

#if defined(__ANDROID__)
#include <android/log.h>
#define NATIVE_LOG(...) __android_log_print(ANDROID_LOG_INFO, "RaymanNative", __VA_ARGS__)
#else
#define NATIVE_LOG(...) (std::fprintf(stderr, "[native] " __VA_ARGS__), std::fputc('\n', stderr))
#endif

extern uint8_t* g_rayman_physbase;  // native_capture.cpp

namespace {

native::Renderer* g_renderer = nullptr;
SDL_Window* g_window = nullptr;
bool g_frameOpen = false;
std::mutex g_mutex;

bool Enabled() {
  static const bool enabled = std::getenv("RAYMAN_NATIVE_RENDER") != nullptr;
  return enabled;
}

std::string SpirvDir() {
  if (const char* dir = std::getenv("RAYMAN_NATIVE_SPIRV")) return dir;
  return "../private/native/shaders_by_hash";
}

}  // namespace

// Main thread, after the runtime is set up: creates the window and the renderer.
void RaymanNativeRendererInit() {
  if (!Enabled() || g_renderer) {
    return;
  }
  if (!SDL_Vulkan_GetVkGetInstanceProcAddr() && !SDL_Vulkan_LoadLibrary(nullptr)) {
    NATIVE_LOG("SDL could not load Vulkan: %s", SDL_GetError());
    return;
  }
  g_window = SDL_CreateWindow("Rayman Origins - native renderer", 960, 540,
                              SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE);
  if (!g_window) {
    NATIVE_LOG("window: %s", SDL_GetError());
    return;
  }
  Uint32 count = 0;
  const char* const* sdlExts = SDL_Vulkan_GetInstanceExtensions(&count);
  std::vector<const char*> exts(sdlExts, sdlExts + count);
  auto loader = reinterpret_cast<PFN_vkGetInstanceProcAddr>(SDL_Vulkan_GetVkGetInstanceProcAddr());
  int w = 0, h = 0;
  SDL_GetWindowSizeInPixels(g_window, &w, &h);
  auto* renderer = new native::Renderer();
  bool ok = renderer->Init(
      loader, exts,
      [](VkInstance instance) {
        VkSurfaceKHR surface = VK_NULL_HANDLE;
        if (!SDL_Vulkan_CreateSurface(g_window, instance, nullptr, &surface)) return VkSurfaceKHR(VK_NULL_HANDLE);
        return surface;
      },
      uint32_t(w), uint32_t(h));
  if (!ok) {
    NATIVE_LOG("renderer init failed: %s", renderer->error().c_str());
    delete renderer;
    return;
  }
  std::string dir = SpirvDir();
  renderer->SetShaderSource([dir](uint64_t hash, bool vertex) {
    char name[64];
    std::snprintf(name, sizeof(name), "/%016llX_%s.spv", (unsigned long long)hash, vertex ? "vs" : "ps");
    std::ifstream f(dir + name, std::ios::binary);
    std::vector<char> bytes((std::istreambuf_iterator<char>(f)), {});
    std::vector<uint32_t> words(bytes.size() / 4);
    std::memcpy(words.data(), bytes.data(), words.size() * 4);
    if (words.empty()) NATIVE_LOG("missing SPIR-V %s", name);
    return words;
  });
  renderer->SetMemory([](uint32_t address, uint32_t size) -> const uint8_t* {
    if (!g_rayman_physbase || uint64_t(address) + size > 0x20000000u) return nullptr;
    return g_rayman_physbase + address;
  });
  g_renderer = renderer;
  NATIVE_LOG("native renderer ready (%dx%d), SPIR-V from %s", w, h, dir.c_str());
}

// Render thread, from the draw hooks (native_capture.cpp).
void RaymanNativeRendererDraw(const native::DrawCall& call) {
  if (!g_renderer) {
    return;
  }
  std::lock_guard lock(g_mutex);
  if (!g_frameOpen) {
    g_renderer->BeginFrame();
    g_frameOpen = true;
  }
  g_renderer->Draw(call);
}

// Render thread, from the Present hook.
void RaymanNativeRendererPresent() {
  if (!g_renderer) {
    return;
  }
  std::lock_guard lock(g_mutex);
  if (!g_frameOpen) {
    return;
  }
  // With RAYMAN_CAPTURE=1, save the native frame every 10 s next to the
  // emulated captures (captures/native_NNN.ppm).
  static auto start = std::chrono::steady_clock::now();
  static int nextShot = 10;
  bool shot = std::getenv("RAYMAN_CAPTURE") &&
              std::chrono::steady_clock::now() - start >= std::chrono::seconds(nextShot);
  std::vector<uint8_t> pixels;
  g_renderer->EndFrame(shot ? &pixels : nullptr);
  g_frameOpen = false;
  if (shot && !pixels.empty()) {
    char name[64];
    std::snprintf(name, sizeof(name), "captures/native_%03d.ppm", nextShot);
    if (FILE* f = std::fopen(name, "wb")) {
      std::fprintf(f, "P6\n%u %u\n255\n", g_renderer->width(), g_renderer->height());
      for (size_t i = 0; i + 3 < pixels.size(); i += 4) std::fwrite(&pixels[i], 1, 3, f);
      std::fclose(f);
      NATIVE_LOG("saved %s", name);
    }
    nextShot += 10;
  }
  static int frames = 0;
  if (++frames % 300 == 0) {
    auto& s = g_renderer->stats();
    NATIVE_LOG("native frame %d: %u draws, %u skipped, %u pipelines, %u textures", frames, s.draws, s.skipped,
               s.pipelines, s.textures);
  }
}
