#include "heap.h"
#include <cassert>
#include <cstring>
#include <o1heap.h>
#include "memory.h"

RuntimeHeap g_runtimeHeap;

// Cabeçalho próprio com o tamanho pedido (o o1heap não expõe o tamanho do bloco).
// 16 bytes para manter o alinhamento de 16 que o guest espera.
constexpr size_t HEADER_SIZE = 16;

void RuntimeHeap::Init(uint32_t guestBase, uint32_t size)
{
    heap_ = o1heapInit(g_memory.Translate(guestBase), size);
    assert(heap_ != nullptr);
}

void* RuntimeHeap::Alloc(size_t size)
{
    uint8_t* block;
    {
        std::lock_guard lock(mutex_);
        block = static_cast<uint8_t*>(o1heapAllocate(heap_, size + HEADER_SIZE));
    }
    if (block == nullptr)
        return nullptr;
    *reinterpret_cast<size_t*>(block) = size;
    return block + HEADER_SIZE;
}

void* RuntimeHeap::AllocZeroed(size_t size)
{
    void* ptr = Alloc(size);
    if (ptr != nullptr)
        memset(ptr, 0, size);
    return ptr;
}

void RuntimeHeap::Free(void* ptr)
{
    if (ptr == nullptr)
        return;
    std::lock_guard lock(mutex_);
    o1heapFree(heap_, static_cast<uint8_t*>(ptr) - HEADER_SIZE);
}

size_t RuntimeHeap::Size(const void* ptr) const
{
    return ptr ? *reinterpret_cast<const size_t*>(static_cast<const uint8_t*>(ptr) - HEADER_SIZE) : 0;
}
