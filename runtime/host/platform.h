#pragma once
#include <cstdint>

// Camada de plataforma (SDL3): janela, eventos, controle/teclado e saída de som.
// Precisa rodar na thread principal do processo (exigência do macOS).

struct PadState
{
    uint16_t buttons = 0;          // bits XAMINPUT_GAMEPAD_*
    uint8_t leftTrigger = 0, rightTrigger = 0;
    int16_t thumbLX = 0, thumbLY = 0, thumbRX = 0, thumbRY = 0;
};

bool InitPlatform();
// Processa eventos até a janela fechar (ou o jogo sair). Bloqueia.
void RunPlatformLoop();
void ShutdownPlatform();

// Controle do jogador 1: controle físico se houver, senão o teclado.
PadState GetPadState();
bool IsPadConnected(uint32_t userIndex);
void SetPadVibration(uint16_t left, uint16_t right);
