#pragma once
// Incluído à força (-include) em todo o código recompilado, antes do ppc_context.h.
// Redefine a chamada indireta para detectar alvos sem função recompilada: em vez
// de pular para o endereço 0 (crash sem pista), informa o endereço do guest.
#include <cstdint>

struct PPCContext;
[[noreturn]] void PpcMissingIndirectCall(uint32_t target, PPCContext& ctx);

#define PPC_CALL_INDIRECT_FUNC(x)                                   \
    do                                                              \
    {                                                               \
        uint32_t ppcTarget_ = (x);                                  \
        PPCFunc* ppcFunc_ = PPC_LOOKUP_FUNC(base, ppcTarget_);      \
        if (__builtin_expect(ppcFunc_ == nullptr, 0))               \
            PpcMissingIndirectCall(ppcTarget_, ctx);                \
        ppcFunc_(ctx, base);                                        \
    } while (0)
