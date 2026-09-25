// Touch controls (Android): the Java overlay (TouchControls.java) reports its
// state here, and it is fed to an SDL3 virtual gamepad. The runtime's SDL input
// driver picks the pad up through hotplug, so the game sees an ordinary Xbox
// controller.
#if defined(__ANDROID__)

#include <SDL3/SDL.h>
#include <jni.h>

#include <algorithm>
#include <atomic>
#include <mutex>

namespace {

std::mutex g_mutex;
SDL_Joystick* g_pad = nullptr;
std::atomic<bool> g_enabled{false};

// Must hold g_mutex. The joystick subsystem is started by the runtime's input
// driver, so the pad is attached lazily once it is up.
bool EnsurePad() {
  if (g_pad) {
    return true;
  }
  if (!SDL_WasInit(SDL_INIT_JOYSTICK)) {
    return false;
  }
  SDL_VirtualJoystickDesc desc;
  SDL_INIT_INTERFACE(&desc);
  desc.type = SDL_JOYSTICK_TYPE_GAMEPAD;
  desc.naxes = SDL_GAMEPAD_AXIS_COUNT;
  desc.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
  desc.name = "Rayman touch controls";
  SDL_JoystickID id = SDL_AttachVirtualJoystick(&desc);
  if (!id) {
    return false;
  }
  g_pad = SDL_OpenJoystick(id);
  if (!g_pad) {
    SDL_DetachVirtualJoystick(id);
  }
  return g_pad != nullptr;
}

void ReleasePad() {
  if (!g_pad) {
    return;
  }
  SDL_JoystickID id = SDL_GetJoystickID(g_pad);
  SDL_CloseJoystick(g_pad);
  SDL_DetachVirtualJoystick(id);
  g_pad = nullptr;
}

Sint16 ToAxis(float value) {
  return Sint16(std::clamp(value, -1.0f, 1.0f) * 32767.0f);
}

}  // namespace

extern "C" {

// Attached while the overlay is visible, so a physical controller is the only
// pad when the overlay is hidden.
JNIEXPORT void JNICALL Java_io_github_belmantegu_raymanrecomp_TouchControls_nativeSetEnabled(
    JNIEnv*, jclass, jboolean enabled) {
  std::lock_guard lock(g_mutex);
  g_enabled = enabled;
  if (!enabled) {
    ReleasePad();
  }
}

// buttons: bit N = SDL_GamepadButton N. Stick in [-1, 1] (y down), trigger in [0, 1].
JNIEXPORT void JNICALL Java_io_github_belmantegu_raymanrecomp_TouchControls_nativeSetState(
    JNIEnv*, jclass, jint buttons, jfloat left_x, jfloat left_y, jfloat right_trigger) {
  std::lock_guard lock(g_mutex);
  if (!g_enabled || !EnsurePad()) {
    return;
  }
  for (int b = 0; b < SDL_GAMEPAD_BUTTON_COUNT; ++b) {
    SDL_SetJoystickVirtualButton(g_pad, b, (buttons >> b) & 1);
  }
  SDL_SetJoystickVirtualAxis(g_pad, SDL_GAMEPAD_AXIS_LEFTX, ToAxis(left_x));
  SDL_SetJoystickVirtualAxis(g_pad, SDL_GAMEPAD_AXIS_LEFTY, ToAxis(left_y));
  // Triggers range 0..32767.
  SDL_SetJoystickVirtualAxis(g_pad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER,
                             Sint16(std::clamp(right_trigger, 0.0f, 1.0f) * 32767.0f));
}

}  // extern "C"

#endif  // __ANDROID__
