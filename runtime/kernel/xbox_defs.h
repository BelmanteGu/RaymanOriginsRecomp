#pragma once
#include <cstdint>

// Constantes do kernel do Xbox 360 (valores do XDK, os mesmos do Xenia).

// NTSTATUS
constexpr uint32_t X_STATUS_SUCCESS = 0x00000000;
constexpr uint32_t X_STATUS_UNSUCCESSFUL = 0xC0000001;
constexpr uint32_t X_STATUS_NOT_IMPLEMENTED = 0xC0000002;
constexpr uint32_t X_STATUS_INVALID_PARAMETER = 0xC000000D;
constexpr uint32_t X_STATUS_NO_MEMORY = 0xC0000017;
constexpr uint32_t X_STATUS_MEMORY_NOT_ALLOCATED = 0xC00000A0;
constexpr uint32_t X_STATUS_ACCESS_DENIED = 0xC0000022;

// Tipos de alocação
constexpr uint32_t X_MEM_COMMIT = 0x00001000;
constexpr uint32_t X_MEM_RESERVE = 0x00002000;
constexpr uint32_t X_MEM_DECOMMIT = 0x00004000;
constexpr uint32_t X_MEM_RELEASE = 0x00008000;
constexpr uint32_t X_MEM_FREE = 0x00010000;
constexpr uint32_t X_MEM_PRIVATE = 0x00020000;
constexpr uint32_t X_MEM_RESET = 0x00080000;
constexpr uint32_t X_MEM_TOP_DOWN = 0x00100000;
constexpr uint32_t X_MEM_NOZERO = 0x00800000;
constexpr uint32_t X_MEM_LARGE_PAGES = 0x20000000;
constexpr uint32_t X_MEM_HEAP = 0x40000000;
constexpr uint32_t X_MEM_16MB_PAGES = 0x80000000;

// Proteção de página
constexpr uint32_t X_PAGE_NOACCESS = 0x01;
constexpr uint32_t X_PAGE_READONLY = 0x02;
constexpr uint32_t X_PAGE_READWRITE = 0x04;
constexpr uint32_t X_PAGE_EXECUTE_READ = 0x20;
constexpr uint32_t X_PAGE_EXECUTE_READWRITE = 0x40;
constexpr uint32_t X_PAGE_NOCACHE = 0x200;
constexpr uint32_t X_PAGE_WRITECOMBINE = 0x400;
