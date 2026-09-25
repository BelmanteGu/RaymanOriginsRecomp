#!/bin/sh
# Mede quantas das instruções que faltam no XenonRecomp upstream cada fork implementa.
# Uso: sh coverage.sh  (lê recomp/recomp.log)
P=$(cd "$(dirname "$0")/../.." && pwd)
W=$P/tools/forks/cache
mkdir -p "$W"

grep "Unrecognized instruction" "$P/recomp/recomp.log" | awk '{print $NF}' | sort -u \
  | tr 'a-z' 'A-Z' | sed -e 's/\.$/_/' -e 's/^/PPC_INST_/' -e 's/_$//' > "$W/missing.txt"
# as variantes com ponto (vcmpgtsw.) aparecem no mesmo case que a sem ponto
sort -u -o "$W/missing.txt" "$W/missing.txt"
echo "instruções faltando: $(wc -l < "$W/missing.txt")"

for r in sonicnext-dev/XenonRecomp testdriveupgrade/XenonRecompUnlimited lawlietl4/LostOdysseyRecomp \
         IsaacMarovitz/XenonRecomp Nitch2024/XenonRecomp Skate-2-Team/XenonRecomp-sk8 Revan67/XenonRecomp \
         razecrs/XenonRecomp kernbyte/XenonRecomp gaiden-dev/XenonRecomp worleydl/XenonRecomp-dev; do
  f="$W/$(echo "$r" | tr '/' '_').cpp"
  [ -s "$f" ] || gh api "repos/$r/contents/XenonRecomp/recompiler.cpp" -H "Accept: application/vnd.github.raw" > "$f" 2>/dev/null
  n=0
  while read -r op; do
    grep -q "case $op:" "$f" && n=$((n+1))
  done < "$W/missing.txt"
  printf "%3d  %-40s %s linhas\n" "$n" "$r" "$(wc -l < "$f" | tr -d ' ')"
done
