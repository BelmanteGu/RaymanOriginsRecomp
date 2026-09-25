#pragma once
#include <cstdint>
#include <string>
#include "ppc_recomp_shared.h"

// Formata uma string de formato do guest com argumentos variádicos a partir do
// argumento firstArg (0 = r3). Devolve false se o formato for inválido.
bool FormatGuestVarargs(PPCContext& ctx, const char* format, int32_t firstArg, std::string& out);
