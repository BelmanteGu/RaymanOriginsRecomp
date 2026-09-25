#include "memory.h"
#include <cstdio>
#include <sys/mman.h>

GuestMemory g_memory;

// Usado pelo xpointer<T> da XenonUtils para traduzir ponteiros do guest.
extern "C" void* MmGetHostAddress(uint32_t ptr)
{
    return g_memory.base + ptr;
}

bool GuestMemory::Init()
{
    // Tenta um endereço alinhado a 4 GB (como o Unleashed); senão, qualquer lugar.
    base = (uint8_t*)mmap((void*)0x100000000ull, PPC_MEMORY_SIZE, PROT_READ | PROT_WRITE,
                          MAP_ANON | MAP_PRIVATE, -1, 0);
    if (base == (uint8_t*)MAP_FAILED)
        base = (uint8_t*)mmap(nullptr, PPC_MEMORY_SIZE, PROT_READ | PROT_WRITE,
                              MAP_ANON | MAP_PRIVATE, -1, 0);
    if (base == (uint8_t*)MAP_FAILED)
    {
        base = nullptr;
        fprintf(stderr, "[memory] mmap de 4 GB falhou\n");
        return false;
    }

    // Página zero inacessível: ponteiro nulo do guest vira crash imediato em vez de lixo.
    mprotect(base, 4096, PROT_NONE);

    for (size_t i = 0; PPCFuncMappings[i].guest != 0; i++)
    {
        if (PPCFuncMappings[i].host != nullptr)
            InsertFunction(uint32_t(PPCFuncMappings[i].guest), PPCFuncMappings[i].host);
    }

    fprintf(stderr, "[memory] guest base = %p\n", (void*)base);
    return true;
}
