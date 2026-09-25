// rayman - ReXGlue Recompiled Project
//
// Customize your app by overriding virtual hooks from rex::ReXApp.

#pragma once

#include <rex/rex_app.h>
#include <rex/runtime.h>
#include <rex/system/interfaces/graphics.h>
#include <rex/ui/presenter.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>

class RaymanApp : public rex::ReXApp {
 public:
  using rex::ReXApp::ReXApp;

  static std::unique_ptr<rex::ui::WindowedApp> Create(
      rex::ui::WindowedAppContext& ctx) {
    return std::unique_ptr<RaymanApp>(new RaymanApp(ctx, "rayman",
        PPCImageConfig));
  }

  // Com RAYMAN_CAPTURE=1, salva o frame do jogo a cada 10 s em captures/frame_NNN.ppm
  // (diagnóstico: permite ver a imagem sem acesso à janela).
  void OnPostSetup() override {
    if (!std::getenv("RAYMAN_CAPTURE")) {
      return;
    }
    std::thread([this] {
      std::system("mkdir -p captures");
      for (int n = 1; n <= 12; ++n) {
        std::this_thread::sleep_for(std::chrono::seconds(10));
        auto* graphics = runtime() ? runtime()->graphics_system() : nullptr;
        auto* presenter = graphics ? graphics->presenter() : nullptr;
        rex::ui::RawImage image;
        if (!presenter || !presenter->CaptureGuestOutput(image) || !image.width) {
          std::fprintf(stderr, "[capture] sem imagem no segundo %d\n", n * 10);
          continue;
        }
        char name[64];
        std::snprintf(name, sizeof(name), "captures/frame_%03d.ppm", n * 10);
        if (FILE* f = std::fopen(name, "wb")) {
          std::fprintf(f, "P6\n%u %u\n255\n", image.width, image.height);
          for (uint32_t y = 0; y < image.height; ++y) {
            const uint8_t* row = image.data.data() + y * image.stride;
            for (uint32_t x = 0; x < image.width; ++x) {
              std::fwrite(row + x * 4, 1, 3, f);  // RGBX -> RGB
            }
          }
          std::fclose(f);
          std::fprintf(stderr, "[capture] %s (%ux%u)\n", name, image.width, image.height);
        }
      }
    }).detach();
  }
};
