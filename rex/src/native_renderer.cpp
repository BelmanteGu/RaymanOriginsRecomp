// Native renderer, in-game. Draws every game frame with the native renderer
// (tools/native_renderer/vk_renderer.h: Vulkan + the game's SPIR-V shaders).
//   RAYMAN_NATIVE_RENDER=1     second window, next to the Xenos emulation (shadow mode)
//   RAYMAN_NATIVE_RENDER=main  the game's own window; run with --gpu_plugin=null so
//                              no GPU emulation runs and nothing else presents to it
//
// SPIR-V is read from RAYMAN_NATIVE_SPIRV (default: private/native/shaders_by_hash,
// relative to the repository root), as <HASH>_vs.spv / <HASH>_ps.spv.
#include <SDL3/SDL.h>
#if defined(__ANDROID__)
#include <android/native_window.h>
#include <dlfcn.h>
#endif
#include <SDL3/SDL_metal.h>
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
  return "../private/native/spirv_ubo";
}

}  // namespace

// Main thread, after the runtime is set up: creates the window and the renderer.
void RaymanNativeRendererInit() {
  if (!Enabled() || g_renderer) {
    return;
  }
  NATIVE_LOG("native renderer: starting (%s)", std::getenv("RAYMAN_NATIVE_RENDER"));
  bool mainWindow = std::string(std::getenv("RAYMAN_NATIVE_RENDER")) == "main";
  std::vector<const char*> exts;
  PFN_vkGetInstanceProcAddr loader = nullptr;
  native::Renderer::SurfaceFactory makeSurface;
  if (mainWindow) {
    // The game's window was not created for Vulkan: make the surface from its
    // native handle, the way ReXGlue's presenter does.
    int count = 0;
    SDL_Window** windows = SDL_GetWindows(&count);
    g_window = count > 0 ? windows[0] : nullptr;
    SDL_free(windows);
    if (!g_window) {
      NATIVE_LOG("no game window");
      return;
    }
    exts.push_back(VK_KHR_SURFACE_EXTENSION_NAME);
#if defined(__APPLE__)
    exts.push_back(VK_EXT_METAL_SURFACE_EXTENSION_NAME);
    makeSurface = [](VkInstance instance) {
      SDL_MetalView view = SDL_Metal_CreateView(g_window);
      VkMetalSurfaceCreateInfoEXT info{VK_STRUCTURE_TYPE_METAL_SURFACE_CREATE_INFO_EXT};
      info.pLayer = static_cast<const CAMetalLayer*>(SDL_Metal_GetLayer(view));
      VkSurfaceKHR surface = VK_NULL_HANDLE;
      vkCreateMetalSurfaceEXT(instance, &info, nullptr, &surface);
      return surface;
    };
#elif defined(__ANDROID__)
    exts.push_back(VK_KHR_ANDROID_SURFACE_EXTENSION_NAME);
    makeSurface = [](VkInstance instance) {
      VkAndroidSurfaceCreateInfoKHR info{VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR};
      info.window = static_cast<ANativeWindow*>(SDL_GetPointerProperty(
          SDL_GetWindowProperties(g_window), SDL_PROP_WINDOW_ANDROID_WINDOW_POINTER, nullptr));
      // The game runs at 60 fps: ask for a 60 Hz display mode rather than 120.
      // ANativeWindow_setFrameRate is API 30+; look it up so API 29 still loads.
      using SetFrameRate = int32_t (*)(ANativeWindow*, float, int8_t);
      if (auto set = reinterpret_cast<SetFrameRate>(dlsym(RTLD_DEFAULT, "ANativeWindow_setFrameRate"))) {
        set(info.window, 60.0f, 1 /* ANATIVEWINDOW_FRAME_RATE_COMPATIBILITY_FIXED_SOURCE */);
      }
      VkSurfaceKHR surface = VK_NULL_HANDLE;
      vkCreateAndroidSurfaceKHR(instance, &info, nullptr, &surface);
      return surface;
    };
#endif
  } else {
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
    exts.assign(sdlExts, sdlExts + count);
    loader = reinterpret_cast<PFN_vkGetInstanceProcAddr>(SDL_Vulkan_GetVkGetInstanceProcAddr());
    makeSurface = [](VkInstance instance) {
      VkSurfaceKHR surface = VK_NULL_HANDLE;
      if (!SDL_Vulkan_CreateSurface(g_window, instance, nullptr, &surface)) return VkSurfaceKHR(VK_NULL_HANDLE);
      return surface;
    };
  }
  int w = 0, h = 0;
  SDL_GetWindowSizeInPixels(g_window, &w, &h);
  NATIVE_LOG("native renderer: window %p %dx%d", static_cast<void*>(g_window), w, h);
  auto* renderer = new native::Renderer();
  renderer->log = [](const char* stage) { NATIVE_LOG("native renderer: %s", stage); };
  bool ok = renderer->Init(loader, exts, makeSurface, uint32_t(w), uint32_t(h));
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
  static const int interval = std::getenv("RAYMAN_CAPTURE_INTERVAL") ? std::atoi(std::getenv("RAYMAN_CAPTURE_INTERVAL")) : 10;
  static int nextShot = interval;
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
    nextShot += interval;
  }
  static int frames = 0;
  if (++frames % 300 == 0) {
    auto& s = g_renderer->stats();
    NATIVE_LOG("native frame %d: %u draws, %u skipped, %u pipelines, %u textures", frames, s.draws, s.skipped,
               s.pipelines, s.textures);
    for (auto& [reason, n] : g_renderer->TakeSkipReasons()) NATIVE_LOG("  skipped x%u: %s", n, reason.c_str());
  }
}
