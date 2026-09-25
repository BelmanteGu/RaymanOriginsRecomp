#pragma once
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <span>
#include <xbox.h>

// Objetos do kernel do Xbox 360 no host: tabela de handles e "dispatcher"
// (eventos, semáforos, threads) com esperas simples, múltiplas e com timeout.
//
// Sincronização: uma trava global do dispatcher + uma condition variable.
// Toda mudança de estado sinalizável acontece com a trava e acorda todos os
// que esperam, que reavaliam a condição. É simples e suporta WaitAll/WaitAny
// e timeouts sem casos especiais; dá para otimizar depois se aparecer no perfil.

constexpr uint32_t STATUS_WAIT_0 = 0x00000000;
constexpr uint32_t STATUS_USER_APC = 0x000000C0;
constexpr uint32_t STATUS_TIMEOUT = 0x00000102;
constexpr uint32_t STATUS_INVALID_HANDLE = 0xC0000008;
constexpr uint32_t STATUS_OBJECT_TYPE_MISMATCH = 0xC0000024;

constexpr uint32_t CURRENT_PROCESS_HANDLE = 0xFFFFFFFF;
constexpr uint32_t CURRENT_THREAD_HANDLE = 0xFFFFFFFE;
constexpr uint32_t INVALID_HANDLE_VALUE = 0xFFFFFFFF;

// Tipos do DISPATCHER_HEADER (campo Type)
enum : uint8_t
{
    DISPATCHER_NOTIFICATION_EVENT = 0,
    DISPATCHER_SYNCHRONIZATION_EVENT = 1,
    DISPATCHER_MUTANT = 2,
    DISPATCHER_SEMAPHORE = 5,
    DISPATCHER_THREAD = 6,
};

struct KernelObject : std::enable_shared_from_this<KernelObject>
{
    virtual ~KernelObject() = default;

    // Chamados com a trava do dispatcher. Objetos não sinalizáveis (arquivos)
    // mantêm o padrão: nunca sinalizados.
    virtual bool IsSignaled() const { return false; }
    virtual void OnAcquired() {}

    // Estrutura do guest que espelha este objeto (0 se criado só por handle).
    uint32_t guestHeader = 0;
};

struct Event final : KernelObject
{
    bool manualReset;
    bool signaled;

    Event(bool manualReset, bool signaled) : manualReset(manualReset), signaled(signaled) {}
    bool IsSignaled() const override { return signaled; }
    void OnAcquired() override { if (!manualReset) signaled = false; }
};

struct Semaphore final : KernelObject
{
    int32_t count;
    int32_t maximum;

    Semaphore(int32_t count, int32_t maximum) : count(count), maximum(maximum) {}
    bool IsSignaled() const override { return count > 0; }
    void OnAcquired() override { count--; }
};

// ---- Dispatcher ----
std::unique_lock<std::mutex> LockDispatcher();
void NotifyDispatcher(); // chamar depois de mudar estado, com ou sem a trava

// Espera em um ou mais objetos. timeoutMs < 0 = infinito.
// Devolve STATUS_WAIT_0 + índice (WaitAny), STATUS_WAIT_0 (WaitAll) ou STATUS_TIMEOUT.
uint32_t WaitForObjects(std::span<const std::shared_ptr<KernelObject>> objects, bool waitAll, int64_t timeoutMs);

// Converte o timeout do guest (unidades de 100 ns; negativo = relativo,
// positivo = absoluto, nulo = infinito) para milissegundos.
int64_t GuestTimeoutToMs(const be<int64_t>* timeout);

// ---- Handles ----
uint32_t CreateHandle(std::shared_ptr<KernelObject> object);
std::shared_ptr<KernelObject> GetObject(uint32_t handle); // resolve CURRENT_THREAD_HANDLE
bool CloseHandle(uint32_t handle);
bool IsHandle(uint32_t value); // handles ficam abaixo de 0x10000, onde não há memória do guest

template<typename T>
std::shared_ptr<T> GetObjectAs(uint32_t handle)
{
    return std::dynamic_pointer_cast<T>(GetObject(handle));
}

// ---- Estruturas do guest (KEVENT, KSEMAPHORE...) ----
// Vincula um DISPATCHER_HEADER em memória do guest a um objeto do host, criando-o
// a partir do estado do header na primeira vez (ou se o guest o reinicializou).
std::shared_ptr<KernelObject> GetObjectForHeader(XDISPATCHER_HEADER* header);
// Aceita tanto ponteiro para header quanto handle (ObReferenceObjectByHandle devolve o handle).
std::shared_ptr<KernelObject> GetObjectForPointerOrHandle(uint32_t guestValue);
// Reflete o estado do objeto no SignalState do header do guest (com a trava do dispatcher).
void SyncHeaderSignalState(KernelObject& object);
