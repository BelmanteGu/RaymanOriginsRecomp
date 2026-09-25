// Imports de sincronização: eventos, semáforos, esperas, handles, critical
// sections, spinlocks, SLIST e DPC. Critical sections e spinlocks seguem o
// Unleashed Recompiled (GPL-3.0); o resto usa o dispatcher de objects.cpp.
#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>
#include <vector>
#include "cpu/guest_context.h"
#include "function.h"
#include "objects.h"
#include "xbox_defs.h"

// ---- Eventos ----

static uint32_t NtCreateEvent(be<uint32_t>* handle, void* attributes, uint32_t eventType, uint32_t initialState)
{
    // eventType 0 = NotificationEvent (reset manual), 1 = SynchronizationEvent (auto-reset)
    auto event = std::make_shared<Event>(eventType == 0, initialState != 0);
    handle->set(CreateHandle(event));
    (void)attributes;
    return X_STATUS_SUCCESS;
}

static uint32_t SetEventObject(const std::shared_ptr<KernelObject>& object, bool signal)
{
    auto* event = dynamic_cast<Event*>(object.get());
    if (!event)
        return 0;
    uint32_t previous;
    {
        auto lock = LockDispatcher();
        previous = event->signaled ? 1 : 0;
        event->signaled = signal;
        SyncHeaderSignalState(*event);
    }
    if (signal)
        NotifyDispatcher();
    return previous;
}

static uint32_t NtSetEvent(uint32_t handle, be<uint32_t>* previousState)
{
    auto object = GetObject(handle);
    if (!object)
        return STATUS_INVALID_HANDLE;
    uint32_t previous = SetEventObject(object, true);
    if (previousState)
        previousState->set(previous);
    return X_STATUS_SUCCESS;
}

static uint32_t NtClearEvent(uint32_t handle)
{
    auto object = GetObject(handle);
    if (!object)
        return STATUS_INVALID_HANDLE;
    SetEventObject(object, false);
    return X_STATUS_SUCCESS;
}

static uint32_t KeSetEvent(XDISPATCHER_HEADER* event, uint32_t increment, uint32_t wait)
{
    (void)increment; (void)wait;
    return SetEventObject(GetObjectForHeader(event), true);
}

static uint32_t KeResetEvent(XDISPATCHER_HEADER* event)
{
    return SetEventObject(GetObjectForHeader(event), false);
}

// ---- Semáforos ----

static uint32_t NtCreateSemaphore(be<uint32_t>* handle, void* attributes, int32_t initialCount, int32_t maximumCount)
{
    handle->set(CreateHandle(std::make_shared<Semaphore>(initialCount, maximumCount)));
    (void)attributes;
    return X_STATUS_SUCCESS;
}

static uint32_t NtReleaseSemaphore(uint32_t handle, int32_t releaseCount, be<int32_t>* previousCount)
{
    auto semaphore = GetObjectAs<Semaphore>(handle);
    if (!semaphore)
        return STATUS_INVALID_HANDLE;
    {
        auto lock = LockDispatcher();
        if (previousCount)
            previousCount->set(semaphore->count);
        semaphore->count = std::min(semaphore->count + releaseCount, semaphore->maximum);
        SyncHeaderSignalState(*semaphore);
    }
    NotifyDispatcher();
    return X_STATUS_SUCCESS;
}

// ---- Esperas ----

static uint32_t NtWaitForSingleObjectEx(uint32_t handle, uint32_t waitMode, uint32_t alertable, be<int64_t>* timeout)
{
    std::shared_ptr<KernelObject> object = GetObject(handle);
    if (!object)
        return STATUS_INVALID_HANDLE;
    (void)waitMode; (void)alertable;
    return WaitForObjects({ &object, 1 }, false, GuestTimeoutToMs(timeout));
}

static uint32_t NtWaitForMultipleObjectsEx(uint32_t count, be<uint32_t>* handles, uint32_t waitType,
                                           uint32_t waitMode, uint32_t alertable, be<int64_t>* timeout)
{
    std::vector<std::shared_ptr<KernelObject>> objects(count);
    for (uint32_t i = 0; i < count; i++)
    {
        objects[i] = GetObject(handles[i].get());
        if (!objects[i])
            return STATUS_INVALID_HANDLE;
    }
    (void)waitMode; (void)alertable;
    return WaitForObjects(objects, waitType == 0, GuestTimeoutToMs(timeout)); // 0 = WaitAll, 1 = WaitAny
}

static uint32_t KeWaitForSingleObject(uint32_t object, uint32_t waitReason, uint32_t waitMode, uint32_t alertable, be<int64_t>* timeout)
{
    std::shared_ptr<KernelObject> target = GetObjectForPointerOrHandle(object);
    if (!target)
        return STATUS_INVALID_HANDLE;
    (void)waitReason; (void)waitMode; (void)alertable;
    return WaitForObjects({ &target, 1 }, false, GuestTimeoutToMs(timeout));
}

static uint32_t KeWaitForMultipleObjects(uint32_t count, be<uint32_t>* objects, uint32_t waitType, uint32_t waitReason,
                                         uint32_t waitMode, uint32_t alertable, be<int64_t>* timeout, void* waitBlocks)
{
    std::vector<std::shared_ptr<KernelObject>> targets(count);
    for (uint32_t i = 0; i < count; i++)
    {
        targets[i] = GetObjectForPointerOrHandle(objects[i].get());
        if (!targets[i])
            return STATUS_INVALID_HANDLE;
    }
    (void)waitReason; (void)waitMode; (void)alertable; (void)waitBlocks;
    return WaitForObjects(targets, waitType == 0, GuestTimeoutToMs(timeout));
}

// ---- Handles e Ob* ----

static uint32_t NtClose(uint32_t handle)
{
    if (handle == INVALID_HANDLE_VALUE || handle == CURRENT_THREAD_HANDLE)
        return X_STATUS_SUCCESS;
    return CloseHandle(handle) ? X_STATUS_SUCCESS : STATUS_INVALID_HANDLE;
}

static uint32_t NtDuplicateObject(uint32_t sourceHandle, be<uint32_t>* targetHandle, uint32_t options)
{
    auto object = GetObject(sourceHandle);
    if (!object)
        return STATUS_INVALID_HANDLE;
    targetHandle->set(CreateHandle(object));
    if (options & 0x1) // DUPLICATE_CLOSE_SOURCE
        CloseHandle(sourceHandle);
    return X_STATUS_SUCCESS;
}

// O "objeto" devolvido é o próprio handle (como no Unleashed); as funções Ke*
// aceitam tanto handle quanto ponteiro de header.
static uint32_t ObReferenceObjectByHandle(uint32_t handle, uint32_t objectType, be<uint32_t>* object)
{
    if (handle == CURRENT_THREAD_HANDLE)
    {
        if (auto self = GetObject(handle))
            handle = CreateHandle(self);
    }
    else if (!GetObject(handle))
    {
        return STATUS_INVALID_HANDLE;
    }
    object->set(handle);
    (void)objectType;
    return X_STATUS_SUCCESS;
}

static void ObDereferenceObject(uint32_t object)
{
    (void)object;
}

// ---- Primitivas atômicas ----
// A libc++ do macOS 14 não tem std::atomic_ref: usamos os builtins do Clang.

static bool CompareExchange(uint32_t* p, uint32_t& expected, uint32_t desired)
{
    return __atomic_compare_exchange_n(p, &expected, desired, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
}

static void AtomicStore(uint32_t* p, uint32_t value)
{
    __atomic_store_n(p, value, __ATOMIC_RELEASE);
}

// Espera ativa com recuo: gira pouco, depois cede a CPU, depois dorme.
static void Backoff(uint32_t& spins)
{
    if (++spins < 64)
        return;
    if (spins < 1024)
        std::this_thread::yield();
    else
        std::this_thread::sleep_for(std::chrono::microseconds(50));
}

// ---- Critical sections (Unleashed: campos do host, dono = r13 da thread) ----

static uint32_t CurrentThreadTag()
{
    return GetPPCContext()->r13.u32;
}

static void RtlInitializeCriticalSection(XRTL_CRITICAL_SECTION* cs)
{
    cs->Header.Absolute = 0;
    cs->LockCount = -1;
    cs->RecursionCount = 0;
    cs->OwningThread = 0;
}

static void RtlInitializeCriticalSectionAndSpinCount(XRTL_CRITICAL_SECTION* cs, uint32_t spinCount)
{
    cs->Header.Absolute = uint8_t((spinCount + 255) >> 8);
    cs->LockCount = -1;
    cs->RecursionCount = 0;
    cs->OwningThread = 0;
}

static bool TryAcquire(XRTL_CRITICAL_SECTION* cs, uint32_t self)
{
    uint32_t previous = 0;
    if (CompareExchange(&cs->OwningThread, previous, self) || previous == self)
    {
        cs->RecursionCount++;
        return true;
    }
    return false;
}

static void RtlEnterCriticalSection(XRTL_CRITICAL_SECTION* cs)
{
    const uint32_t self = CurrentThreadTag();
    uint32_t spins = 0;
    while (!TryAcquire(cs, self))
        Backoff(spins);
}

static uint32_t RtlTryEnterCriticalSection(XRTL_CRITICAL_SECTION* cs)
{
    return TryAcquire(cs, CurrentThreadTag()) ? 1 : 0;
}

static void RtlLeaveCriticalSection(XRTL_CRITICAL_SECTION* cs)
{
    if (--cs->RecursionCount != 0)
        return;
    AtomicStore(&cs->OwningThread, 0);
}

// ---- Spinlocks ----

static uint32_t KfAcquireSpinLock(uint32_t* spinLock)
{
    const uint32_t self = CurrentThreadTag();
    uint32_t spins = 0;
    while (true)
    {
        uint32_t expected = 0;
        if (CompareExchange(spinLock, expected, self))
            return 0; // IRQL anterior
        Backoff(spins);
    }
}

static void KfReleaseSpinLock(uint32_t* spinLock, uint32_t oldIrql)
{
    AtomicStore(spinLock, 0);
    (void)oldIrql;
}

static void KeAcquireSpinLockAtRaisedIrql(uint32_t* spinLock)
{
    KfAcquireSpinLock(spinLock);
}

static void KeReleaseSpinLockFromRaisedIrql(uint32_t* spinLock)
{
    AtomicStore(spinLock, 0);
}

static void KeEnterCriticalRegion() {}
static void KeLeaveCriticalRegion() {}
static void KeLockL2() {}
static void KeUnlockL2() {}

// ---- SLIST (lista atômica) ----

// SLIST_HEADER: Next (be32), Depth (be16), Sequence (be16), tratado como 64 bits.
static uint32_t InterlockedPopEntrySList(uint64_t* header)
{
    uint64_t current = __atomic_load_n(header, __ATOMIC_ACQUIRE);
    while (true)
    {
        uint64_t hostOrder = __builtin_bswap64(current); // bytes big-endian: os 4 primeiros são Next
        uint32_t next = uint32_t(hostOrder >> 32);
        if (next == 0)
            return 0;
        uint16_t depth = uint16_t(hostOrder >> 16);
        uint16_t sequence = uint16_t(hostOrder);
        uint32_t following = static_cast<be<uint32_t>*>(g_memory.Translate(next))->get();
        uint64_t updated = (uint64_t(following) << 32) | (uint64_t(uint16_t(depth - 1)) << 16) | uint16_t(sequence + 1);
        if (__atomic_compare_exchange_n(header, &current, __builtin_bswap64(updated), false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
            return next;
    }
}

// ---- DPC ----
// Usados por callbacks de interrupção (GPU/áudio). Ainda não temos quem dispare.
static void KeInitializeDpc(uint32_t dpc, uint32_t routine, uint32_t context)
{
    fprintf(stderr, "[kernel] KeInitializeDpc dpc=0x%08X rotina=0x%08X ctx=0x%08X\n", dpc, routine, context);
}

static uint32_t KeInsertQueueDpc(uint32_t dpc, uint32_t arg1, uint32_t arg2)
{
    fprintf(stderr, "[kernel] KeInsertQueueDpc dpc=0x%08X (ignorado)\n", dpc);
    (void)arg1; (void)arg2;
    return 0;
}

GUEST_FUNCTION_HOOK(__imp__NtCreateEvent, NtCreateEvent);
GUEST_FUNCTION_HOOK(__imp__NtSetEvent, NtSetEvent);
GUEST_FUNCTION_HOOK(__imp__NtClearEvent, NtClearEvent);
GUEST_FUNCTION_HOOK(__imp__KeSetEvent, KeSetEvent);
GUEST_FUNCTION_HOOK(__imp__KeResetEvent, KeResetEvent);
GUEST_FUNCTION_HOOK(__imp__NtCreateSemaphore, NtCreateSemaphore);
GUEST_FUNCTION_HOOK(__imp__NtReleaseSemaphore, NtReleaseSemaphore);
GUEST_FUNCTION_HOOK(__imp__NtWaitForSingleObjectEx, NtWaitForSingleObjectEx);
GUEST_FUNCTION_HOOK(__imp__NtWaitForMultipleObjectsEx, NtWaitForMultipleObjectsEx);
GUEST_FUNCTION_HOOK(__imp__KeWaitForSingleObject, KeWaitForSingleObject);
GUEST_FUNCTION_HOOK(__imp__KeWaitForMultipleObjects, KeWaitForMultipleObjects);
GUEST_FUNCTION_HOOK(__imp__NtClose, NtClose);
GUEST_FUNCTION_HOOK(__imp__NtDuplicateObject, NtDuplicateObject);
GUEST_FUNCTION_HOOK(__imp__ObReferenceObjectByHandle, ObReferenceObjectByHandle);
GUEST_FUNCTION_HOOK(__imp__ObDereferenceObject, ObDereferenceObject);
GUEST_FUNCTION_HOOK(__imp__RtlInitializeCriticalSection, RtlInitializeCriticalSection);
GUEST_FUNCTION_HOOK(__imp__RtlInitializeCriticalSectionAndSpinCount, RtlInitializeCriticalSectionAndSpinCount);
GUEST_FUNCTION_HOOK(__imp__RtlEnterCriticalSection, RtlEnterCriticalSection);
GUEST_FUNCTION_HOOK(__imp__RtlTryEnterCriticalSection, RtlTryEnterCriticalSection);
GUEST_FUNCTION_HOOK(__imp__RtlLeaveCriticalSection, RtlLeaveCriticalSection);
GUEST_FUNCTION_HOOK(__imp__KfAcquireSpinLock, KfAcquireSpinLock);
GUEST_FUNCTION_HOOK(__imp__KfReleaseSpinLock, KfReleaseSpinLock);
GUEST_FUNCTION_HOOK(__imp__KeAcquireSpinLockAtRaisedIrql, KeAcquireSpinLockAtRaisedIrql);
GUEST_FUNCTION_HOOK(__imp__KeReleaseSpinLockFromRaisedIrql, KeReleaseSpinLockFromRaisedIrql);
GUEST_FUNCTION_HOOK(__imp__KeEnterCriticalRegion, KeEnterCriticalRegion);
GUEST_FUNCTION_HOOK(__imp__KeLeaveCriticalRegion, KeLeaveCriticalRegion);
GUEST_FUNCTION_HOOK(__imp__KeLockL2, KeLockL2);
GUEST_FUNCTION_HOOK(__imp__KeUnlockL2, KeUnlockL2);
GUEST_FUNCTION_HOOK(__imp__InterlockedPopEntrySList, InterlockedPopEntrySList);
GUEST_FUNCTION_HOOK(__imp__KeInitializeDpc, KeInitializeDpc);
GUEST_FUNCTION_HOOK(__imp__KeInsertQueueDpc, KeInsertQueueDpc);
