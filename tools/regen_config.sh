#!/bin/sh
# Regenera as jump tables e os limites de função do Rayman e roda o XenonRecomp.
# Uso: sh tools/regen_config.sh
set -e
P=$(cd "$(dirname "$0")/.." && pwd)
CFG=$P/recomp/config
XEX=$P/private/game/default.xex
EXPECTED=1444bbea65a137160b34779dc9c850f7505cd103b5e86e6b613d01a862e9dfdc

# A configuração vale só para o XEX retail sem title update.
if [ ! -f "$XEX" ]; then
    echo "Coloque o default.xex da SUA cópia do jogo em private/game/"; exit 1
fi
if [ "$(shasum -a 256 "$XEX" | cut -d' ' -f1)" != "$EXPECTED" ]; then
    echo "default.xex com SHA-256 diferente do esperado ($EXPECTED)."
    echo "A configuração em recomp/config só vale para a versão retail sem title update."
    exit 1
fi

sh "$P/tools/jumptables/build.sh"
"$P/tools/jumptables/rayman_jumptables" "$XEX" \
    "$CFG/rayman_switch_tables.toml" "$CFG/functions.generated.toml" | grep -E "resolvidas|limites|!"

# Substitui o bloco entre os marcadores no rayman.toml.
python3 - "$CFG/rayman.toml" "$CFG/functions.generated.toml" <<'EOF'
import sys
toml_path, gen_path = sys.argv[1:3]
text = open(toml_path).read()
begin, end = "# BEGIN functions (gerado)\n", "# END functions (gerado)"
head, rest = text.split(begin, 1)
_, tail = rest.split(end, 1)
open(toml_path, "w").write(head + begin + open(gen_path).read() + end + tail)
EOF
rm "$CFG/functions.generated.toml"

rm -rf "$P/recomp/ppc" && mkdir -p "$P/recomp/ppc"
"$P/tools/XenonRecomp/build/XenonRecomp/XenonRecomp" "$CFG/rayman.toml" \
    "$P/tools/XenonRecomp/XenonUtils/ppc_context.h" > "$P/recomp/recomp.log" 2>&1
echo "recomp.log: $(grep -vc 'Recompiling functions' "$P/recomp/recomp.log") avisos"
