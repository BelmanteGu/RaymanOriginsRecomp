#include "objects.h"
#include <chrono>
#include <cstdio>
#include <unordered_map>
#include <vector>
#include "memory.h"

std::shared_ptr<KernelObject> GetCurrentThreadObject(); // thread.cpp

static std::mutex g_dispatcherMutex;
static std::condition_variable g_dispatcherCv;

std::unique_lock<std::mutex> LockDispatcher()
{
    return std::unique_lock(g_dispatcherMutex);
}

void NotifyDispatcher()
{
    g_dispatcherCv.notify_all();
}

int64_t GuestTimeoutToMs(const be<int64_t>* timeout)
{
    if (timeout == nullptr)
        return -1;

    int64_t value = timeout->get();
    if (value <= 0)
        return (-value + 9999) / 10000; // relativo, arredondando para cima

    // Absoluto: FILETIME (100 ns desde 1601).
    constexpr int64_t FILETIME_EPOCH_DIFFERENCE = 116444736000000000LL;
    int64_t now = std::chrono::duration_cast<std::chrono::duration<int64_t, std::ratio<1, 10000000>>>(
        std::chrono::system_clock::now().time_since_epoch()).count() + FILETIME_EPOCH_DIFFERENCE;
    return value > now ? (value - now + 9999) / 10000 : 0;
}

uint32_t WaitForObjects(std::span<const std::shared_ptr<KernelObject>> objects, bool waitAll, int64_t timeoutMs)
{
    auto lock = LockDispatcher();
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs < 0 ? 0 : timeoutMs);

    while (true)
    {
        if (waitAll)
        {
            bool all = true;
            for (const auto& object : objects)
                all &= object && object->IsSignaled();
            if (all)
            {
                for (const auto& object : objects)
                {
                    object->OnAcquired();
                    SyncHeaderSignalState(*object);
                }
                return STATUS_WAIT_0;
            }
        }
        else
        {
            for (size_t i = 0; i < objects.size(); i++)
            {
                if (objects[i] && objects[i]->IsSignaled())
                {
                    objects[i]->OnAcquired();
                    SyncHeaderSignalState(*objects[i]);
                    return STATUS_WAIT_0 + uint32_t(i);
                }
            }
        }

        if (timeoutMs == 0)
            return STATUS_TIMEOUT;
        if (timeoutMs < 0)
            g_dispatcherCv.wait(lock);
        else if (g_dispatcherCv.wait_until(lock, deadline) == std::cv_status::timeout)
            timeoutMs = 0; // uma última checagem antes de devolver STATUS_TIMEOUT
    }
}

// ---- Handles ----

static std::mutex g_handleMutex;
static std::unordered_map<uint32_t, std::shared_ptr<KernelObject>> g_handles;
static std::vector<uint32_t> g_freeHandles;
static uint32_t g_nextHandle = 0x1000;

uint32_t CreateHandle(std::shared_ptr<KernelObject> object)
{
    std::lock_guard lock(g_handleMutex);
    uint32_t handle;
    if (!g_freeHandles.empty())
    {
        handle = g_freeHandles.back();
        g_freeHandles.pop_back();
    }
    else
    {
        handle = g_nextHandle;
        g_nextHandle += 4;
        if (g_nextHandle >= 0x10000)
            fprintf(stderr, "[kernel] tabela de handles esgotada\n");
    }
    g_handles[handle] = std::move(object);
    return handle;
}

std::shared_ptr<KernelObject> GetObject(uint32_t handle)
{
    if (handle == CURRENT_THREAD_HANDLE)
        return GetCurrentThreadObject();

    std::lock_guard lock(g_handleMutex);
    auto it = g_handles.find(handle);
    return it != g_handles.end() ? it->second : nullptr;
}

bool CloseHandle(uint32_t handle)
{
    std::lock_guard lock(g_handleMutex);
    if (g_handles.erase(handle) == 0)
        return false;
    g_freeHandles.push_back(handle);
    return true;
}

bool IsHandle(uint32_t value)
{
    return (value != 0 && value < 0x10000) || value == CURRENT_THREAD_HANDLE;
}

// ---- Estruturas do guest ----

// Assinatura gravada em WaitListHead.Flink; Blink guarda o handle do objeto.
constexpr uint32_t HEADER_SIGNATURE = 0x5241594D; // "RAYM"

struct XKSEMAPHORE_LAYOUT
{
    XDISPATCHER_HEADER Header;
    be<int32_t> Limit;
};

std::shared_ptr<KernelObject> GetObjectForHeader(XDISPATCHER_HEADER* header)
{
    if (header == nullptr)
        return nullptr;

    auto lock = LockDispatcher();
    if (header->WaitListHead.Flink.get() == HEADER_SIGNATURE)
    {
        if (auto object = GetObject(header->WaitListHead.Blink.get()))
            return object;
    }

    std::shared_ptr<KernelObject> object;
    switch (header->Type)
    {
    case DISPATCHER_NOTIFICATION_EVENT:
    case DISPATCHER_SYNCHRONIZATION_EVENT:
        object = std::make_shared<Event>(header->Type == DISPATCHER_NOTIFICATION_EVENT, header->SignalState.get() != 0);
        break;
    case DISPATCHER_SEMAPHORE:
    {
        auto* semaphore = reinterpret_cast<XKSEMAPHORE_LAYOUT*>(header);
        object = std::make_shared<Semaphore>(int32_t(header->SignalState.get()), semaphore->Limit.get());
        break;
    }
    default:
        fprintf(stderr, "[kernel] tipo de DISPATCHER_HEADER não suportado: %u (0x%08X)\n",
                header->Type, g_memory.MapVirtual(header));
        return nullptr;
    }

    object->guestHeader = g_memory.MapVirtual(header);
    header->WaitListHead.Flink = HEADER_SIGNATURE;
    header->WaitListHead.Blink = CreateHandle(object);
    return object;
}

std::shared_ptr<KernelObject> GetObjectForPointerOrHandle(uint32_t guestValue)
{
    if (IsHandle(guestValue))
        return GetObject(guestValue);
    return GetObjectForHeader(static_cast<XDISPATCHER_HEADER*>(g_memory.Translate(guestValue)));
}

void SyncHeaderSignalState(KernelObject& object)
{
    if (object.guestHeader == 0)
        return;

    auto* header = static_cast<XDISPATCHER_HEADER*>(g_memory.Translate(object.guestHeader));
    if (auto* event = dynamic_cast<Event*>(&object))
        header->SignalState = event->signaled ? 1u : 0u;
    else if (auto* semaphore = dynamic_cast<Semaphore*>(&object))
        header->SignalState = uint32_t(semaphore->count);
}
