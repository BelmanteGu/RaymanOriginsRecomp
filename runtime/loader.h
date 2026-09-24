#pragma once
#include <cstdint>

struct LoadedImage
{
    uint32_t base = 0;
    uint32_t size = 0;
    uint32_t entryPoint = 0;
};

// Descriptografa/descomprime o XEX (via XenonUtils) e copia a imagem para a
// memória do guest em guestBase + base. Devolve false se não conseguir.
bool LoadXexImage(const char* path, uint8_t* guestBase, LoadedImage& out);
