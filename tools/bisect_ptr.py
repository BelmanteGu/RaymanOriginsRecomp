#!/usr/bin/env python3
"""Bissecção das funções "por ponteiro" do tools/jumptables.

Encontra a menor quantidade N de funções por ponteiro (em ordem de endereço)
com a qual o boot muda de comportamento. "Bom" = o log do boot contém GOOD_MARK.
Usa diretórios próprios (recomp/bisect-config, recomp/ppc-bisect, build-bisect)
para não interferir no build principal.

Uso: python3 tools/bisect_ptr.py <total> [marca-de-bom]
"""
import os
import re
import shutil
import subprocess
import sys

P = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CFG = os.path.join(P, "recomp", "bisect-config")
PPC = os.path.join(P, "recomp", "ppc-bisect")
BUILD = os.path.join(P, "build-bisect")
TOTAL = int(sys.argv[1]) if sys.argv[1] != "gen" else 0
GOOD_MARK = sys.argv[2] if len(sys.argv) > 2 else "[fs] open d:\\localisation"


def run(cmd, **kw):
    return subprocess.run(cmd, shell=True, cwd=P, capture_output=True, text=True, **kw)


def generate(limit):
    os.makedirs(CFG, exist_ok=True)
    toml = open(os.path.join(P, "recomp/config/rayman.toml")).read()
    toml = toml.replace('out_directory_path = "../ppc"', 'out_directory_path = "../ppc-bisect.new"')
    env = dict(os.environ, JT_PTR_MODE="data", JT_PTR_LIMIT=str(limit))
    subprocess.run([f"{P}/tools/jumptables/rayman_jumptables", f"{P}/private/game/default.xex",
                    f"{CFG}/rayman_switch_tables.toml", f"{CFG}/functions.generated.toml"],
                   env=env, capture_output=True, check=True)
    gen = open(f"{CFG}/functions.generated.toml").read()
    begin, end = "# BEGIN functions (gerado)\n", "# END functions (gerado)"
    head, rest = toml.split(begin, 1)
    _, tail = rest.split(end, 1)
    open(f"{CFG}/rayman.toml", "w").write(head + begin + gen + end + tail)

    new = PPC + ".new"
    shutil.rmtree(new, ignore_errors=True)
    os.makedirs(new)
    os.makedirs(PPC, exist_ok=True)
    run(f'"{P}/tools/XenonRecomp/build/XenonRecomp/XenonRecomp" "{CFG}/rayman.toml" "{P}/tools/XenonRecomp/XenonUtils/ppc_context.h"')
    run(f'rsync -rc --delete "{new}/" "{PPC}/"')
    shutil.rmtree(new, ignore_errors=True)


def good(limit):
    generate(limit)
    if not os.path.exists(os.path.join(BUILD, "build.ninja")):
        run(f'cmake --preset macos-llvm -B "{BUILD}" -DRAYMAN_PPC_DIR="{PPC}"')
    b = run(f'ninja -C "{BUILD}" rayman')
    if b.returncode != 0:
        print(b.stdout[-2000:], b.stderr[-2000:])
        sys.exit("build falhou")
    r = run(f'perl -e \'alarm shift; exec @ARGV\' 60 "{BUILD}/rayman" private/game/default.xex', timeout=120)
    ok = GOOD_MARK in (r.stdout + r.stderr)
    print(f"N={limit}: {'bom' if ok else 'ruim'}", flush=True)
    return ok


if sys.argv[1] == "gen":  # só gera e compila a configuração N
    generate(int(sys.argv[2]))
    sys.exit(run(f'ninja -C "{BUILD}" rayman').returncode)

lo, hi = 0, TOTAL  # lo é bom (sem funções por ponteiro), hi é ruim
while hi - lo > 1:
    mid = (lo + hi) // 2
    if good(mid):
        lo = mid
    else:
        hi = mid

# A N-ésima função (índice hi-1) é a culpada.
generate(hi)
fns = re.findall(r"\{ address = (0x[0-9A-F]+), size = (0x[0-9A-F]+) \}, # ptr", open(f"{CFG}/functions.generated.toml").read())
print("culpada:", fns[hi - 1] if len(fns) >= hi else "?", "(N =", hi, ")")
