#!/usr/bin/env python3
"""Transplanta para o recompiler.cpp do upstream os `case` de instruções que
faltam, copiados de um fork (por padrão Nitch2024/XenonRecomp).

Só copia blocos cujos labels o upstream ainda não trata; labels que o upstream
já tem são removidos do bloco. Os blocos entram logo antes do `default:` do
switch principal de Recompiler::Recompile.

Uso: transplant.py <upstream.cpp> <fork.cpp> <missing.txt> <saida.cpp>
"""
import re
import sys

CASE = re.compile(r"^    case (PPC_INST_\w+):\s*$")


def main_switch(lines):
    """Intervalo [início, default) do switch (id) principal."""
    start = next(i for i, l in enumerate(lines) if l.rstrip() == "    switch (id)")
    end = next(i for i in range(start, len(lines)) if lines[i].rstrip() == "    default:")
    return start, end


def blocks(lines, start, end):
    """Lista de (labels, corpo) dentro do switch."""
    out, i = [], start
    while i < end:
        if not CASE.match(lines[i]):
            i += 1
            continue
        labels = []
        while i < end and CASE.match(lines[i]):
            labels.append(CASE.match(lines[i]).group(1))
            i += 1
        body = []
        while i < end and not CASE.match(lines[i]):
            body.append(lines[i])
            i += 1
        out.append((labels, body))
    return out


def main():
    up_path, fork_path, missing_path, out_path = sys.argv[1:5]
    up = open(up_path).read().split("\n")
    fork = open(fork_path).read().split("\n")
    missing = {l.strip() for l in open(missing_path) if l.strip()}
    # variantes com ponto (vcmpgtsw.) usam o mesmo label sem ponto
    missing |= {m.rstrip("_") for m in missing}

    us, ue = main_switch(up)
    have = {lab for labs, _ in blocks(up, us, ue) for lab in labs}

    fs, fe = main_switch(fork)
    new, got = [], set()
    for labs, body in blocks(fork, fs, fe):
        take = [l for l in labs if l in missing and l not in have]
        if not take:
            continue
        got |= set(take)
        new += [f"    case {l}:" for l in take] + body

    # o fork usa _MM_SHUFFLE (x86); o upstream usa a versão do SIMDe
    new = [l.replace("_MM_SHUFFLE(", "SIMDE_MM_SHUFFLE(") for l in new]

    header = ["    // ---- Instruções transplantadas de Nitch2024/XenonRecomp (tools/forks/transplant.py) ----"]
    result = up[:ue] + header + new + up[ue:]
    open(out_path, "w").write("\n".join(result))

    print(f"transplantados: {len(got)} labels")
    for m in sorted(missing - got - have):
        print(f"  NÃO encontrado no fork: {m}")


if __name__ == "__main__":
    main()
