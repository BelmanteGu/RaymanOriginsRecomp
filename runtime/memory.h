#pragma once
#include "ppc_recomp_shared.h"

// Espaço de endereçamento de 32 bits do Xbox 360 (4 GB), reservado de uma vez.
// Endereço do guest = deslocamento a partir de base.
struct GuestMemory
{
    uint8_t* base = nullptr;

    bool Init();

    void* Translate(uint32_t guest) const { return base + guest; }
    uint32_t MapVirtual(const void* host) const { return uint32_t(static_cast<const uint8_t*>(host) - base); }

    PPCFunc* FindFunction(uint32_t guest) const { return PPC_LOOKUP_FUNC(base, guest); }
    void InsertFunction(uint32_t guest, PPCFunc* host) { PPC_LOOKUP_FUNC(base, guest) = host; }
};

extern GuestMemory g_memory;
