#pragma once
#include <cstdint>
#include <mutex>
#include <vector>

// Alocador de páginas para uma faixa do espaço do guest, com a semântica de
// reserva/commit do kernel do Xbox 360 (NtAllocateVirtualMemory e afins).
// A memória do host já está toda mapeada como RW: aqui só controlamos quais
// páginas estão livres, reservadas ou commitadas, e zeramos no commit.
class PageHeap
{
public:
    enum State : uint8_t { Free = 0, Reserved = 1, Committed = 2 };

    struct RegionInfo
    {
        uint32_t baseAddress;     // início da faixa consultada (alinhado à página)
        uint32_t allocationBase;  // início da reserva a que pertence (0 se livre)
        uint32_t allocationProtect;
        uint32_t regionSize;      // páginas contíguas com o mesmo estado, em bytes
        State state;
        uint32_t protect;
    };

    // pageSize: granularidade de commit/consulta. allocAlign: alinhamento de
    // reservas sem endereço fixo (64 KB no Xbox, mesmo para páginas de 4 KB).
    PageHeap(uint32_t base, uint32_t size, uint32_t pageSize, uint32_t allocAlign);

    bool Contains(uint32_t address) const { return address >= base_ && address - base_ < size_; }
    uint32_t PageSize() const { return pageSize_; }

    // Reserva (e opcionalmente commita) uma região nova. alignment 0 = allocAlign.
    // Devolve 0 se não houver espaço.
    uint32_t Alloc(uint32_t size, uint32_t alignment, bool commit, bool topDown, uint32_t protect);

    // Reserva e/ou commita num endereço fixo. Commitar dentro de uma reserva
    // existente é o uso normal (reserva grande, commit por partes).
    bool AllocFixed(uint32_t address, uint32_t size, bool reserve, bool commit, uint32_t protect, bool* wasCommitted);

    bool Decommit(uint32_t address, uint32_t size);
    // Libera a reserva inteira que começa em address. Devolve o tamanho liberado.
    bool Release(uint32_t address, uint32_t* releasedSize);

    bool Query(uint32_t address, RegionInfo& out) const;
    // Tamanho da reserva que começa em address (0 se não for início de reserva).
    uint32_t AllocationSize(uint32_t address) const;

private:
    uint32_t PageIndex(uint32_t address) const { return (address - base_) / pageSize_; }
    uint32_t PageAddress(uint32_t index) const { return base_ + index * pageSize_; }
    bool RangeIs(uint32_t first, uint32_t count, State state) const;
    void Commit(uint32_t first, uint32_t count, uint32_t protect);

    const uint32_t base_, size_, pageSize_, allocAlign_;
    std::vector<State> state_;
    std::vector<uint32_t> allocBase_;   // página -> endereço base da reserva
    std::vector<uint32_t> protect_;
    std::vector<uint32_t> allocPages_;  // página inicial de reserva -> nº de páginas (0 nas demais)
    uint32_t hint_ = 0;                 // próxima-busca (next fit) para reservas bottom-up
    mutable std::mutex mutex_;
};
