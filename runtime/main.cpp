// RaymanPort — runtime mínimo para o teste de boot (Fase 3).
// Carrega o XEX na memória do guest, monta o contexto da thread principal e
// chama o entry point recompilado. Imports não implementados só logam (stubs).
#include <cstdio>
#include <cstring>
#include "memory.h"
#include "loader.h"

// Layout do bloco da thread, igual ao do Unleashed Recompiled (cpu/guest_thread.cpp).
constexpr uint32_t PCR_SIZE = 0xAB0;
constexpr uint32_t TLS_SIZE = 0x100;
constexpr uint32_t TEB_SIZE = 0x2E0;
constexpr uint32_t STACK_SIZE = 0x40000;

static void Store32(uint32_t guest, uint32_t value)
{
    *(uint32_t*)g_memory.Translate(guest) = __builtin_bswap32(value);
}

static void InitThreadContext(PPCContext& ctx, uint32_t cpuNumber, uint32_t threadId)
{
    uint32_t block = RuntimeAlloc(PCR_SIZE + TLS_SIZE + TEB_SIZE + STACK_SIZE, 0x1000);
    memset(g_memory.Translate(block), 0, PCR_SIZE + TLS_SIZE + TEB_SIZE + STACK_SIZE);

    uint32_t tls = block + PCR_SIZE;
    uint32_t teb = tls + TLS_SIZE;

    Store32(block + 0x0, tls);                              // PCR: ponteiro de TLS
    Store32(block + 0x100, teb);                            // PCR: ponteiro do TEB
    *(uint8_t*)g_memory.Translate(block + 0x10C) = uint8_t(cpuNumber);
    Store32(tls + 0x10, 0xFFFFFFFF);                        // entrada de TLS que o Unleashed também seta
    Store32(teb + 0x14C, threadId);                         // id da thread

    ctx = {};
    ctx.r1.u64 = teb + TEB_SIZE + STACK_SIZE;               // topo da pilha
    ctx.r13.u64 = block;                                    // r13 = PCR
    ctx.fpscr.loadFromHost();
}

int main(int argc, char** argv)
{
    const char* xexPath = argc > 1 ? argv[1] : "private/game/default.xex";

    if (!g_memory.Init())
        return 1;

    LoadedImage image;
    if (!LoadXexImage(xexPath, g_memory.base, image))
        return 1;

    PPCFunc* entry = g_memory.FindFunction(image.entryPoint);
    if (entry == nullptr)
    {
        fprintf(stderr, "[main] entry point 0x%08X não tem função recompilada\n", image.entryPoint);
        return 1;
    }

    PPCContext ctx;
    InitThreadContext(ctx, 0, 1);

    fprintf(stderr, "[main] chamando entry point 0x%08X\n", image.entryPoint);
    entry(ctx, g_memory.base);
    fprintf(stderr, "[main] entry point retornou (r3=0x%08X)\n", ctx.r3.u32);
    return 0;
}
