// Imports de vídeo (Vd*) que não dependem da GPU: modo de vídeo, gama, EDRAM,
// inicialização dos engines. O ring buffer e o VdSwap ficam para o backend de
// GPU (#12). Valores do Xenia (xboxkrnl_video.cc) e do Unleashed Recompiled.
#include <cstdio>
#include <cstring>
#include "function.h"
#include "video.h"
#include "xbox_defs.h"

GraphicsInterrupt g_graphicsInterrupt;

static void VdQueryVideoMode(XVIDEO_MODE* mode)
{
    memset(mode, 0, sizeof(*mode));
    mode->DisplayWidth = 1280;
    mode->DisplayHeight = 720;
    mode->IsInterlaced = 0u;
    mode->IsWidescreen = 1u;
    mode->IsHighDefinition = 1u;
    mode->RefreshRate = 0x42700000; // 60.0f
    mode->VideoStandard = 1;
    mode->Unknown4A = 0x4A;
    mode->Unknown01 = 0x01;
}

// Bit 0 = widescreen, bit 1 = largura >= 1024, bit 2 = largura >= 1920 (Xenia).
static uint32_t VdQueryVideoFlags() { return 0x3; }

static void VdGetCurrentDisplayGamma(be<uint32_t>* type, be<float>* power)
{
    type->set(1);
    power->set(2.22222233f);
}

static uint32_t VdPersistDisplay(uint32_t unknown, be<uint32_t>* result)
{
    (void)unknown;
    if (result)
        result->set(0);
    return 0;
}

static uint32_t VdIsHSIOTrainingSucceeded() { return 1; }
static uint32_t VdRetrainEDRAM(uint32_t a, uint32_t b, uint32_t c, uint32_t d, uint32_t e, uint32_t f) { (void)a; (void)b; (void)c; (void)d; (void)e; (void)f; return 0; }
static uint32_t VdRetrainEDRAMWorker(uint32_t unknown) { (void)unknown; return 0; }
static uint32_t VdInitializeEngines(uint32_t a, uint32_t b, uint32_t c, uint32_t d, uint32_t e) { (void)a; (void)b; (void)c; (void)d; (void)e; return 1; }
static void VdShutdownEngines() {}
static uint32_t VdSetDisplayMode(uint32_t mode) { (void)mode; return 0; }
static uint32_t VdSetDisplayModeOverride(uint32_t a, uint32_t b, uint32_t c, uint32_t d, uint32_t e) { (void)a; (void)b; (void)c; (void)d; (void)e; return 0; }
static void VdEnableDisableClockGating(uint32_t enabled) { (void)enabled; }
static uint32_t VdCallGraphicsNotificationRoutines(uint32_t unknown, void* args) { (void)unknown; (void)args; return 0; }

// Valores fictícios, como no Xenia.
static void VdGetSystemCommandBuffer(be<uint32_t>* p0, be<uint32_t>* p1)
{
    p0->set(0xBEEF0000);
    p1->set(0xBEEF0001);
}

static void VdSetGraphicsInterruptCallback(uint32_t callback, uint32_t userData)
{
    fprintf(stderr, "[gpu] interrupção gráfica: rotina=0x%08X dados=0x%08X\n", callback, userData);
    g_graphicsInterrupt.callback = callback;
    g_graphicsInterrupt.userData = userData;
}

GUEST_FUNCTION_HOOK(__imp__VdQueryVideoMode, VdQueryVideoMode);
GUEST_FUNCTION_HOOK(__imp__VdQueryVideoFlags, VdQueryVideoFlags);
GUEST_FUNCTION_HOOK(__imp__VdGetCurrentDisplayGamma, VdGetCurrentDisplayGamma);
GUEST_FUNCTION_HOOK(__imp__VdPersistDisplay, VdPersistDisplay);
GUEST_FUNCTION_HOOK(__imp__VdIsHSIOTrainingSucceeded, VdIsHSIOTrainingSucceeded);
GUEST_FUNCTION_HOOK(__imp__VdRetrainEDRAM, VdRetrainEDRAM);
GUEST_FUNCTION_HOOK(__imp__VdRetrainEDRAMWorker, VdRetrainEDRAMWorker);
GUEST_FUNCTION_HOOK(__imp__VdInitializeEngines, VdInitializeEngines);
GUEST_FUNCTION_HOOK(__imp__VdShutdownEngines, VdShutdownEngines);
GUEST_FUNCTION_HOOK(__imp__VdSetDisplayMode, VdSetDisplayMode);
GUEST_FUNCTION_HOOK(__imp__VdSetDisplayModeOverride, VdSetDisplayModeOverride);
GUEST_FUNCTION_HOOK(__imp__VdEnableDisableClockGating, VdEnableDisableClockGating);
GUEST_FUNCTION_HOOK(__imp__VdCallGraphicsNotificationRoutines, VdCallGraphicsNotificationRoutines);
GUEST_FUNCTION_HOOK(__imp__VdGetSystemCommandBuffer, VdGetSystemCommandBuffer);
GUEST_FUNCTION_HOOK(__imp__VdSetGraphicsInterruptCallback, VdSetGraphicsInterruptCallback);
