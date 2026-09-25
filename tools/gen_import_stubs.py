#!/usr/bin/env python3
"""Gera runtime/generated/import_stubs.cpp: um stub fraco (weak) para cada import
do kernel/XAM que o código recompilado chama (__imp__*).

Cada stub loga o nome e os 4 primeiros argumentos (r3..r6) e devolve 0 em r3.
Por serem weak, basta implementar a função de verdade em qualquer .cpp do
runtime (PPC_FUNC(__imp__Nome)) para que o linker use a versão real.

Uso: gen_import_stubs.py <recomp/ppc/ppc_func_mapping.cpp> <saida.cpp>
"""
import re
import sys

src, out = sys.argv[1:3]
names = sorted(set(re.findall(r"\b__imp__(\w+)\b", open(src).read())))
names = [n for n in names if not n.startswith("sub_")]

lines = [
    "// GERADO por tools/gen_import_stubs.py — não editar à mão.",
    "// Implementações reais em runtime/kernel/ substituem estes stubs (weak).",
    '#include "ppc_recomp_shared.h"',
    '#include "stub_log.h"',
    "",
    "#define RAYMAN_IMPORT_STUB(name) \\",
    "    __attribute__((weak)) PPC_FUNC(__imp__##name) \\",
    "    { \\",
    "        LogImportStub(#name, ctx); \\",
    "        ctx.r3.u64 = 0; \\",
    "    }",
    "",
]
lines += [f"RAYMAN_IMPORT_STUB({n})" for n in names]
lines.append("")
open(out, "w").write("\n".join(lines))
print(f"{len(names)} stubs gerados em {out}")
