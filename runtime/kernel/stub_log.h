#pragma once
#include <cstdint>

struct PPCContext;

// Loga a chamada de um import ainda não implementado (nome + r3..r6).
// Limita o volume por função para o log continuar legível.
void LogImportStub(const char* name, const PPCContext& ctx);
