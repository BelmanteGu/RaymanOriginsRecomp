// Scripted controller for unattended test runs (no one at the keyboard, no
// window on screen): RAYMAN_AUTOPILOT="<seconds>:<action>,..." drives an SDL
// virtual gamepad that the runtime's SDL input driver picks up like a real pad.
//   a, b, x, y, start, back   tap the button (0.2 s)
//   right, left               hold the stick until "stop"
//   stop                      release the stick
//   jump                      tap A while keeping the stick as it is
// Seconds count from the first presented frame. Example:
//   RAYMAN_AUTOPILOT="10:start,14:a,18:a,22:a,30:right,32:jump,34:jump"
#include <SDL3/SDL.h>

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

struct Step {
  double at;
  std::string action;
};

std::vector<Step> Parse(const char* spec) {
  std::vector<Step> steps;
  std::string s(spec);
  size_t pos = 0;
  while (pos < s.size()) {
    size_t end = s.find(',', pos);
    std::string item = s.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
    size_t colon = item.find(':');
    if (colon != std::string::npos) steps.push_back({std::atof(item.c_str()), item.substr(colon + 1)});
    if (end == std::string::npos) break;
    pos = end + 1;
  }
  return steps;
}

int Button(const std::string& name) {
  if (name == "a" || name == "jump") return SDL_GAMEPAD_BUTTON_SOUTH;
  if (name == "b") return SDL_GAMEPAD_BUTTON_EAST;
  if (name == "x") return SDL_GAMEPAD_BUTTON_WEST;
  if (name == "y") return SDL_GAMEPAD_BUTTON_NORTH;
  if (name == "start") return SDL_GAMEPAD_BUTTON_START;
  if (name == "back") return SDL_GAMEPAD_BUTTON_BACK;
  return -1;
}

}  // namespace

// Called once per presented frame (hooks.cpp).
void RaymanAutopilotFrame() {
  static const char* spec = std::getenv("RAYMAN_AUTOPILOT");
  if (!spec) return;
  static std::vector<Step> steps = Parse(spec);
  static SDL_Joystick* pad = nullptr;
  static auto start = std::chrono::steady_clock::now();
  static size_t next = 0;
  static int held = -1;
  static double releaseAt = 0;
  static float stick = 0;
  if (!pad) {
    if (!SDL_WasInit(SDL_INIT_JOYSTICK)) return;
    SDL_VirtualJoystickDesc desc;
    SDL_INIT_INTERFACE(&desc);
    desc.type = SDL_JOYSTICK_TYPE_GAMEPAD;
    desc.naxes = SDL_GAMEPAD_AXIS_COUNT;
    desc.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
    desc.name = "Rayman autopilot";
    SDL_JoystickID id = SDL_AttachVirtualJoystick(&desc);
    if (!id || !(pad = SDL_OpenJoystick(id))) return;
    start = std::chrono::steady_clock::now();
  }
  double now = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
  if (held >= 0 && now >= releaseAt) {
    SDL_SetJoystickVirtualButton(pad, held, false);
    held = -1;
  }
  while (next < steps.size() && steps[next].at <= now) {
    const std::string& a = steps[next++].action;
    if (a == "right") stick = 1;
    else if (a == "left") stick = -1;
    else if (a == "stop") stick = 0;
    else if (int b = Button(a); b >= 0) {
      if (held >= 0) SDL_SetJoystickVirtualButton(pad, held, false);
      SDL_SetJoystickVirtualButton(pad, b, true);
      held = b;
      releaseAt = now + 0.2;
    }
    std::fprintf(stderr, "[autopilot] %.1f s: %s\n", now, a.c_str());
  }
  SDL_SetJoystickVirtualAxis(pad, SDL_GAMEPAD_AXIS_LEFTX, Sint16(stick * 32767));
  // Triggers rest at the axis minimum on a virtual pad.
  SDL_SetJoystickVirtualAxis(pad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER, SDL_JOYSTICK_AXIS_MIN);
  SDL_SetJoystickVirtualAxis(pad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, SDL_JOYSTICK_AXIS_MIN);
}
