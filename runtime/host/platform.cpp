// Camada de plataforma com SDL3: janela, eventos, controle e som.
//
// Teclado (quando não há controle): setas/WASD = analógico esquerdo e direcional,
// Espaço = A (pular), J = X (atacar), K = B, L = Y, Shift = gatilho direito
// (correr), Q/E = LB/RB, Enter = Start, Backspace = Back.
#include "host/platform.h"
#include <SDL3/SDL.h>
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <mutex>
#include <xbox.h>
#include "apu/audio.h"

namespace
{
SDL_Window* g_window = nullptr;
SDL_Gamepad* g_gamepad = nullptr;
SDL_AudioStream* g_audio = nullptr;
std::mutex g_padMutex;
PadState g_pad;
std::atomic<bool> g_running{ true };

void PushAudio(const float* stereo, uint32_t frames)
{
    if (!g_audio)
        return;
    // Evita acumular atraso se o jogo produzir mais rápido que a saída consome.
    if (SDL_GetAudioStreamQueued(g_audio) > int(48000 * 2 * sizeof(float) / 5))
        return;
    SDL_PutAudioStreamData(g_audio, stereo, int(frames * 2 * sizeof(float)));
}

int16_t Axis(bool negative, bool positive)
{
    return positive == negative ? 0 : positive ? 32767 : -32768;
}

void UpdatePad()
{
    PadState pad;
    if (g_gamepad)
    {
        struct { SDL_GamepadButton button; uint16_t bit; } map[] = {
            { SDL_GAMEPAD_BUTTON_DPAD_UP, XAMINPUT_GAMEPAD_DPAD_UP },
            { SDL_GAMEPAD_BUTTON_DPAD_DOWN, XAMINPUT_GAMEPAD_DPAD_DOWN },
            { SDL_GAMEPAD_BUTTON_DPAD_LEFT, XAMINPUT_GAMEPAD_DPAD_LEFT },
            { SDL_GAMEPAD_BUTTON_DPAD_RIGHT, XAMINPUT_GAMEPAD_DPAD_RIGHT },
            { SDL_GAMEPAD_BUTTON_START, XAMINPUT_GAMEPAD_START },
            { SDL_GAMEPAD_BUTTON_BACK, XAMINPUT_GAMEPAD_BACK },
            { SDL_GAMEPAD_BUTTON_LEFT_STICK, XAMINPUT_GAMEPAD_LEFT_THUMB },
            { SDL_GAMEPAD_BUTTON_RIGHT_STICK, XAMINPUT_GAMEPAD_RIGHT_THUMB },
            { SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, XAMINPUT_GAMEPAD_LEFT_SHOULDER },
            { SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, XAMINPUT_GAMEPAD_RIGHT_SHOULDER },
            { SDL_GAMEPAD_BUTTON_SOUTH, XAMINPUT_GAMEPAD_A },
            { SDL_GAMEPAD_BUTTON_EAST, XAMINPUT_GAMEPAD_B },
            { SDL_GAMEPAD_BUTTON_WEST, XAMINPUT_GAMEPAD_X },
            { SDL_GAMEPAD_BUTTON_NORTH, XAMINPUT_GAMEPAD_Y },
        };
        for (auto& m : map)
            if (SDL_GetGamepadButton(g_gamepad, m.button))
                pad.buttons |= m.bit;
        auto trigger = [](int16_t v) { return uint8_t(std::clamp(v, int16_t(0), int16_t(32767)) >> 7); };
        auto flip = [](int16_t v) { return int16_t(std::clamp(-int32_t(v), -32768, 32767)); }; // SDL: +Y para baixo
        pad.leftTrigger = trigger(SDL_GetGamepadAxis(g_gamepad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER));
        pad.rightTrigger = trigger(SDL_GetGamepadAxis(g_gamepad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER));
        pad.thumbLX = SDL_GetGamepadAxis(g_gamepad, SDL_GAMEPAD_AXIS_LEFTX);
        pad.thumbLY = flip(SDL_GetGamepadAxis(g_gamepad, SDL_GAMEPAD_AXIS_LEFTY));
        pad.thumbRX = SDL_GetGamepadAxis(g_gamepad, SDL_GAMEPAD_AXIS_RIGHTX);
        pad.thumbRY = flip(SDL_GetGamepadAxis(g_gamepad, SDL_GAMEPAD_AXIS_RIGHTY));
    }
    else
    {
        const bool* k = SDL_GetKeyboardState(nullptr);
        bool up = k[SDL_SCANCODE_UP] || k[SDL_SCANCODE_W], down = k[SDL_SCANCODE_DOWN] || k[SDL_SCANCODE_S];
        bool left = k[SDL_SCANCODE_LEFT] || k[SDL_SCANCODE_A], right = k[SDL_SCANCODE_RIGHT] || k[SDL_SCANCODE_D];
        if (up) pad.buttons |= XAMINPUT_GAMEPAD_DPAD_UP;
        if (down) pad.buttons |= XAMINPUT_GAMEPAD_DPAD_DOWN;
        if (left) pad.buttons |= XAMINPUT_GAMEPAD_DPAD_LEFT;
        if (right) pad.buttons |= XAMINPUT_GAMEPAD_DPAD_RIGHT;
        if (k[SDL_SCANCODE_SPACE]) pad.buttons |= XAMINPUT_GAMEPAD_A;
        if (k[SDL_SCANCODE_K]) pad.buttons |= XAMINPUT_GAMEPAD_B;
        if (k[SDL_SCANCODE_J]) pad.buttons |= XAMINPUT_GAMEPAD_X;
        if (k[SDL_SCANCODE_L]) pad.buttons |= XAMINPUT_GAMEPAD_Y;
        if (k[SDL_SCANCODE_Q]) pad.buttons |= XAMINPUT_GAMEPAD_LEFT_SHOULDER;
        if (k[SDL_SCANCODE_E]) pad.buttons |= XAMINPUT_GAMEPAD_RIGHT_SHOULDER;
        if (k[SDL_SCANCODE_RETURN]) pad.buttons |= XAMINPUT_GAMEPAD_START;
        if (k[SDL_SCANCODE_BACKSPACE]) pad.buttons |= XAMINPUT_GAMEPAD_BACK;
        if (k[SDL_SCANCODE_LSHIFT] || k[SDL_SCANCODE_RSHIFT]) pad.rightTrigger = 255;
        pad.thumbLX = Axis(left, right);
        pad.thumbLY = Axis(down, up);
    }
    std::lock_guard lock(g_padMutex);
    g_pad = pad;
}
} // namespace

bool InitPlatform()
{
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMEPAD))
    {
        fprintf(stderr, "[host] SDL_Init falhou: %s\n", SDL_GetError());
        return false;
    }
    g_window = SDL_CreateWindow("Rayman Origins Recompiled", 1280, 720, SDL_WINDOW_RESIZABLE);
    if (!g_window)
    {
        fprintf(stderr, "[host] janela: %s\n", SDL_GetError());
        return false;
    }

    SDL_AudioSpec spec{ SDL_AUDIO_F32, 2, 48000 };
    g_audio = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
    if (g_audio)
    {
        SDL_ResumeAudioStreamDevice(g_audio);
        SetAudioSink(PushAudio);
    }
    else
    {
        fprintf(stderr, "[host] sem saída de áudio: %s\n", SDL_GetError());
    }

    int count = 0;
    SDL_JoystickID* pads = SDL_GetGamepads(&count);
    if (count > 0)
        g_gamepad = SDL_OpenGamepad(pads[0]);
    SDL_free(pads);
    fprintf(stderr, "[host] janela 1280x720, %s\n", g_gamepad ? "controle conectado" : "usando o teclado");
    return true;
}

void RunPlatformLoop()
{
    while (g_running)
    {
        SDL_Event event;
        while (SDL_PollEvent(&event))
        {
            switch (event.type)
            {
            case SDL_EVENT_QUIT:
                g_running = false;
                break;
            case SDL_EVENT_GAMEPAD_ADDED:
                if (!g_gamepad)
                    g_gamepad = SDL_OpenGamepad(event.gdevice.which);
                break;
            case SDL_EVENT_GAMEPAD_REMOVED:
                if (g_gamepad && SDL_GetGamepadID(g_gamepad) == event.gdevice.which)
                {
                    SDL_CloseGamepad(g_gamepad);
                    g_gamepad = nullptr;
                }
                break;
            default:
                break;
            }
        }
        UpdatePad();
        SDL_Delay(4);
    }
}

void ShutdownPlatform()
{
    SetAudioSink(nullptr);
    if (g_audio)
        SDL_DestroyAudioStream(g_audio);
    if (g_gamepad)
        SDL_CloseGamepad(g_gamepad);
    if (g_window)
        SDL_DestroyWindow(g_window);
    SDL_Quit();
}

PadState GetPadState()
{
    std::lock_guard lock(g_padMutex);
    return g_pad;
}

bool IsPadConnected(uint32_t userIndex)
{
    return userIndex == 0; // teclado sempre disponível como jogador 1
}

void SetPadVibration(uint16_t left, uint16_t right)
{
    if (g_gamepad)
        SDL_RumbleGamepad(g_gamepad, left, right, 200);
}
