#pragma once
#include <atomic>
#include <cstdint>

// Callback de interrupção gráfica registrado pelo jogo (VdSetGraphicsInterruptCallback).
// O backend de GPU chama esta rotina a cada vsync e ao concluir comandos.
struct GraphicsInterrupt
{
    std::atomic<uint32_t> callback{ 0 };
    std::atomic<uint32_t> userData{ 0 };
};

extern GraphicsInterrupt g_graphicsInterrupt;
