// Relatório de crash curto e imediato: endereço da falha traduzido para o guest,
// função recompilada onde aconteceu e registradores principais do guest.
// Sai com _exit para não esperar o ReportCrash do macOS (que leva minutos).
#include "crash_handler.h"
#include <csignal>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <dlfcn.h>
#include <initializer_list>
#include <unistd.h>
#include "memory.h"
#include "cpu/guest_context.h"

static void Print(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
static void Print(const char* fmt, ...)
{
    char buf[512];
    va_list args;
    va_start(args, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    if (n > 0)
        write(STDERR_FILENO, buf, size_t(n) < sizeof(buf) ? size_t(n) : sizeof(buf) - 1);
}

static void OnCrash(int sig, siginfo_t* info, void* ucontextRaw)
{
    uintptr_t pc = 0;
#if defined(__APPLE__) && defined(__aarch64__)
    pc = static_cast<ucontext_t*>(ucontextRaw)->uc_mcontext->__ss.__pc;
#elif defined(__APPLE__) && defined(__x86_64__)
    pc = static_cast<ucontext_t*>(ucontextRaw)->uc_mcontext->__ss.__rip;
#elif defined(__linux__) && defined(__aarch64__)
    pc = static_cast<ucontext_t*>(ucontextRaw)->uc_mcontext.pc;
#elif defined(__linux__) && defined(__x86_64__)
    pc = static_cast<ucontext_t*>(ucontextRaw)->uc_mcontext.gregs[REG_RIP];
#endif

    uintptr_t fault = reinterpret_cast<uintptr_t>(info->si_addr);
    uintptr_t base = reinterpret_cast<uintptr_t>(g_memory.base);

    Print("\n[crash] sinal %d (%s)\n", sig, strsignal(sig));
    if (base != 0 && fault >= base && fault - base < PPC_MEMORY_SIZE)
        Print("[crash] acesso ao endereço do guest 0x%08lX\n", (unsigned long)(fault - base));
    else
        Print("[crash] acesso ao endereço do host %p\n", info->si_addr);

    Dl_info dl{};
    if (pc != 0 && dladdr(reinterpret_cast<void*>(pc), &dl) && dl.dli_sname)
        Print("[crash] em %s+0x%lX\n", dl.dli_sname, (unsigned long)(pc - reinterpret_cast<uintptr_t>(dl.dli_saddr)));
    else
        Print("[crash] pc do host %p\n", reinterpret_cast<void*>(pc));

    if (PPCContext* ctx = GetPPCContext())
        Print("[crash] guest: lr=%08X r1=%08X r3=%08X r4=%08X r5=%08X r31=%08X\n",
              uint32_t(ctx->lr), ctx->r1.u32, ctx->r3.u32, ctx->r4.u32, ctx->r5.u32, ctx->r31.u32);

    _exit(128 + sig);
}

void InstallCrashHandler()
{
    struct sigaction action{};
    action.sa_sigaction = OnCrash;
    action.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&action.sa_mask);

    // Pilha alternativa: um estouro de pilha do host também precisa ser reportado.
    static uint8_t altStack[64 * 1024];
    stack_t ss{};
    ss.ss_sp = altStack;
    ss.ss_size = sizeof(altStack);
    sigaltstack(&ss, nullptr);

    for (int sig : { SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGTRAP, SIGABRT })
        sigaction(sig, &action, nullptr);
}
