// Detector de jump tables para o Rayman Origins (X360).
//
// O XenonAnalyse procura sequências contíguas e exatas (calibradas no Sonic
// Unleashed) e lê operandos por posição fixa. O compilador do Rayman reordena
// lis/addi/rlwinm e intercala nops, então nada casa. Aqui, para cada bctr,
// achamos o "cmplwi crN, rIdx, MAX / bgt crN, default" que o guarda e
// simulamos o bloco até o bctr com um avaliador de registradores mínimo.
// A ordem das instruções e os nops deixam de importar.
//
// Saída: TOML no mesmo formato do XenonAnalyse ([[switch]] base/r/default/labels).
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include <map>
#include <cstdlib>
#include <set>
#include <algorithm>
#include <file.h>
#include <image.h>
#include <disasm.h>
#include <ppc-inst.h>

namespace
{
    // Valor simbólico de um registrador dentro do bloco do switch.
    struct Val
    {
        enum Kind { Unknown, Const, Index, Loaded, Target } kind = Unknown;
        uint32_t c = 0;       // Const: valor; Target: base relativa
        uint32_t shift = 0;   // Index: escala do índice; Loaded/Target: shift do elemento
        uint32_t table = 0;   // Loaded/Target: endereço da tabela
        uint32_t elem = 0;    // Loaded/Target: tamanho do elemento (1, 2, 4)
        bool relative = false;
    };

    struct Switch
    {
        uint32_t base = 0;
        uint32_t bctr = 0; // endereço do bctr (chave das tabelas no ReXGlue)
        uint32_t r = 0;
        uint32_t def = 0;
        std::vector<uint32_t> labels;
        const char* kind = "";
    };

    uint32_t be32(const uint8_t* p) { return (p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3]; }
    uint32_t be16(const uint8_t* p) { return (p[0] << 8) | p[1]; }

    bool inText(const Section& text, uint32_t a)
    {
        return a >= text.base && a < text.base + text.size && (a & 3) == 0;
    }

    enum class Fail { None, NoGuard, Unsupported, IndexClobbered, BadTable, LabelOutside };

    // Tenta resolver o switch que termina no bctr em bctrAddr.
    Fail Resolve(const Image& image, const Section& text, uint32_t bctrAddr, Switch& out)
    {
        auto insnAt = [&](uint32_t a, ppc_insn& in) {
            ppc::Disassemble(image.Find(a), a, in);
            return in.opcode != nullptr;
        };

        // 1. Guarda: bgt crN, default seguido (para trás) de cmplwi crN, rIdx, MAX.
        ppc_insn in;
        uint32_t bgtAddr = 0, cr = 0, def = 0, idx = 0, max = 0;
        for (int i = 1; i <= 24 && !bgtAddr; ++i)
        {
            uint32_t a = bctrAddr - 4 * i;
            if (!insnAt(a, in)) continue;
            if (in.opcode->id == PPC_INST_BCTR || in.opcode->id == PPC_INST_BLR)
                break; // passou do início do bloco
            if (in.opcode->id == PPC_INST_BGT)
            {
                bgtAddr = a;
                cr = in.operands[0];
                def = in.operands[1];
            }
        }
        if (!bgtAddr) return Fail::NoGuard;

        uint32_t cmpAddr = 0;
        // O escalonador pode afastar o cmplwi do bgt (visto: 6 instruções).
        for (int i = 1; i <= 12 && !cmpAddr; ++i)
        {
            if (!insnAt(bgtAddr - 4 * i, in)) continue;
            if (in.opcode->id == PPC_INST_BLR || in.opcode->id == PPC_INST_BCTR || in.opcode->id == PPC_INST_B)
                break;
            if (in.opcode->id == PPC_INST_CMPLWI && in.operands[0] == cr)
            {
                idx = in.operands[1];
                max = in.operands[2];
                cmpAddr = bgtAddr - 4 * i;
            }
        }
        if (!cmpAddr) return Fail::NoGuard;

        // O índice não pode ser reescrito entre o cmplwi e o bgt.
        for (uint32_t a = cmpAddr + 4; a < bgtAddr; a += 4)
        {
            if (!insnAt(a, in)) return Fail::Unsupported;
            const char* n = in.opcode->name;
            if (n[0] == 'f' || !strncmp(n, "lf", 2) || !strncmp(n, "st", 2) || !strncmp(n, "cmp", 3))
                continue;
            if (in.operands[0] == idx) return Fail::IndexClobbered;
        }

        // 2. Simula o bloco entre o bgt e o bctr.
        Val regs[32];
        regs[idx].kind = Val::Index;

        // Aliases do índice: "mr rX, rIdx" ou "mr rIdx, rX" logo antes do cmplwi
        // (ex.: mr r31,r4 / cmplwi r4 / ... rlwinm r0,r31).
        for (int i = 1; i <= 8; ++i)
        {
            if (!insnAt(cmpAddr - 4 * i, in)) continue;
            if (in.opcode->id == PPC_INST_BLR || in.opcode->id == PPC_INST_BCTR) break;
            if (in.opcode->id == PPC_INST_MR)
            {
                if (in.operands[1] == idx) regs[in.operands[0]].kind = Val::Index;
                else if (in.operands[0] == idx) regs[in.operands[1]].kind = Val::Index;
            }
        }
        uint32_t ctrReg = UINT32_MAX;

        for (uint32_t a = bgtAddr + 4; a < bctrAddr; a += 4)
        {
            if (!insnAt(a, in)) return Fail::Unsupported;
            const uint32_t* o = in.operands;
            Val v;
            switch (in.opcode->id)
            {
            case PPC_INST_NOP:
                continue;
            case PPC_INST_LIS:
                v.kind = Val::Const; v.c = o[1] << 16;
                break;
            case PPC_INST_LI:
                v.kind = Val::Const; v.c = o[1];
                break;
            case PPC_INST_ADDI:
                if (regs[o[1]].kind != Val::Const) return Fail::Unsupported;
                v.kind = Val::Const; v.c = regs[o[1]].c + o[2];
                break;
            case PPC_INST_RLWINM:
            {
                // Só aceitamos shift-left puro: rlwinm rA, rS, SH, 0, 31-SH.
                const Val& s = regs[o[1]];
                uint32_t sh = o[2];
                if (o[3] != 0 || o[4] != 31 - sh) return Fail::Unsupported;
                if (s.kind == Val::Index && s.shift == 0) { v = s; v.shift = sh; }
                else if (s.kind == Val::Loaded && s.shift == 0) { v = s; v.shift = sh; }
                else return Fail::Unsupported;
                break;
            }
            case PPC_INST_LWZX:
            case PPC_INST_LHZX:
            case PPC_INST_LBZX:
            {
                uint32_t elem = in.opcode->id == PPC_INST_LWZX ? 4 :
                                in.opcode->id == PPC_INST_LHZX ? 2 : 1;
                const Val& ra = regs[o[1]];
                const Val& rb = regs[o[2]];
                const Val* tbl = ra.kind == Val::Const ? &ra : rb.kind == Val::Const ? &rb : nullptr;
                const Val* ix = ra.kind == Val::Index ? &ra : rb.kind == Val::Index ? &rb : nullptr;
                if (!tbl || !ix || (1u << ix->shift) != elem) return Fail::Unsupported;
                v.kind = Val::Loaded; v.table = tbl->c; v.elem = elem;
                break;
            }
            case PPC_INST_ADD:
            {
                const Val& ra = regs[o[1]];
                const Val& rb = regs[o[2]];
                const Val* base = ra.kind == Val::Const ? &ra : rb.kind == Val::Const ? &rb : nullptr;
                const Val* ld = ra.kind == Val::Loaded ? &ra : rb.kind == Val::Loaded ? &rb : nullptr;
                if (!base || !ld) return Fail::Unsupported;
                v = *ld; v.kind = Val::Target; v.relative = true; v.c = base->c;
                break;
            }
            case PPC_INST_MTCTR:
                ctrReg = o[0];
                continue;
            default:
            {
                // Instrução não relacionada intercalada pelo escalonador.
                // Stores, float e compares não escrevem em GPR: ignora.
                const char* n = in.opcode->name;
                if (n[0] == 'f' || !strncmp(n, "lf", 2) || !strncmp(n, "st", 2) || !strncmp(n, "cmp", 3))
                    continue;
                // O resto: assume que operands[0] é o GPR de destino e o invalida.
                v.kind = Val::Unknown;
                break;
            }
            }

            // Todas as instruções acima escrevem em operands[0].
            if (o[0] == idx) return Fail::IndexClobbered;
            regs[o[0]] = v;
        }

        if (ctrReg == UINT32_MAX) return Fail::Unsupported;
        const Val& t = regs[ctrReg];

        // 3. Lê a tabela e calcula os labels.
        uint32_t count = max + 1;
        const auto* tbl = static_cast<const uint8_t*>(image.Find(t.table));
        if (!tbl) return Fail::BadTable;

        out = {};
        out.base = bgtAddr + 4;
        out.bctr = bctrAddr;
        out.r = idx;
        out.def = def;
        out.labels.reserve(count);

        if (t.kind == Val::Loaded && t.elem == 4 && t.shift == 0)
        {
            out.kind = "absolute";
            for (uint32_t i = 0; i < count; ++i)
                out.labels.push_back(be32(tbl + 4 * i));
        }
        else if (t.kind == Val::Target && t.relative)
        {
            out.kind = t.elem == 1 ? (t.shift ? "computed" : "byteoffset") : "shortoffset";
            for (uint32_t i = 0; i < count; ++i)
            {
                uint32_t e = t.elem == 1 ? tbl[i] : t.elem == 2 ? be16(tbl + 2 * i) : be32(tbl + 4 * i);
                out.labels.push_back(t.c + (e << t.shift));
            }
        }
        else
        {
            return Fail::Unsupported;
        }

        for (uint32_t l : out.labels)
            if (!inText(text, l)) return Fail::LabelOutside;
        if (!inText(text, def)) return Fail::LabelOutside;

        return Fail::None;
    }
}

// Jump table absoluta sem guarda cmplwi/bgt logo antes (o índice foi validado
// antes ou é garantido pelo compilador). Padrão: lis/addi (tabela) +
// rlwinm rX, rIdx, 2, 0, 29 + lwzx + mtctr + bctr. Sem guarda não há tamanho:
// contamos entradas enquanto apontam para dentro da função [fnStart, fnEnd).
// Uma entrada fora da função indica tabela de ponteiros de função: rejeitada.
static Fail ResolveUnguarded(const Image& image, const Section& text, uint32_t bctrAddr,
                             uint32_t fnStart, uint32_t fnEnd, Switch& out)
{
    ppc_insn in;
    auto insnAt = [&](uint32_t a) {
        ppc::Disassemble(image.Find(a), a, in);
        return in.opcode != nullptr;
    };

    // Início da janela: logo depois do último desvio antes do bctr.
    uint32_t windowStart = bctrAddr;
    for (int i = 1; i <= 16; i++)
    {
        uint32_t a = bctrAddr - 4 * i;
        if (a < fnStart || !insnAt(a))
            break;
        uint32_t id = in.opcode->id;
        const char* n = in.opcode->name;
        if (n[0] == 'b' && id != PPC_INST_BCTR) // qualquer desvio encerra o bloco
            break;
        if (id == PPC_INST_BCTR)
            break;
        windowStart = a;
    }

    Val regs[32];
    uint32_t idx = UINT32_MAX, idxSetAt = 0, ctrReg = UINT32_MAX;
    for (uint32_t a = windowStart; a < bctrAddr; a += 4)
    {
        if (!insnAt(a))
            return Fail::Unsupported;
        const uint32_t* o = in.operands;
        Val v;
        switch (in.opcode->id)
        {
        case PPC_INST_NOP:
            continue;
        case PPC_INST_LIS:
            v.kind = Val::Const; v.c = o[1] << 16;
            break;
        case PPC_INST_ADDI:
            if (regs[o[1]].kind != Val::Const) { v.kind = Val::Unknown; break; }
            v.kind = Val::Const; v.c = regs[o[1]].c + o[2];
            break;
        case PPC_INST_RLWINM:
            if (o[2] == 2 && o[3] == 0 && o[4] == 29 && regs[o[1]].kind == Val::Unknown && idx == UINT32_MAX)
            {
                idx = o[1];
                idxSetAt = a;
                v.kind = Val::Index; v.shift = 2;
            }
            else
                v.kind = Val::Unknown;
            break;
        case PPC_INST_LWZX:
        {
            const Val& ra = regs[o[1]];
            const Val& rb = regs[o[2]];
            const Val* tbl = ra.kind == Val::Const ? &ra : rb.kind == Val::Const ? &rb : nullptr;
            const Val* ix = ra.kind == Val::Index ? &ra : rb.kind == Val::Index ? &rb : nullptr;
            if (!tbl || !ix || ix->shift != 2) { v.kind = Val::Unknown; break; }
            v.kind = Val::Loaded; v.table = tbl->c; v.elem = 4;
            break;
        }
        case PPC_INST_MTCTR:
            ctrReg = o[0];
            continue;
        default:
        {
            const char* n = in.opcode->name;
            if (n[0] == 'f' || !strncmp(n, "lf", 2) || !strncmp(n, "st", 2) || !strncmp(n, "cmp", 3))
                continue;
            v.kind = Val::Unknown;
            break;
        }
        }
        if (idx != UINT32_MAX && o[0] == idx && a > idxSetAt)
            return Fail::IndexClobbered; // o switch usa rIdx no bctr
        regs[o[0]] = v;
    }

    if (ctrReg == UINT32_MAX || idx == UINT32_MAX)
        return Fail::NoGuard;
    const Val& t = regs[ctrReg];
    if (t.kind != Val::Loaded || t.elem != 4)
        return Fail::NoGuard;

    const auto* tbl = static_cast<const uint8_t*>(image.Find(t.table));
    if (!tbl)
        return Fail::BadTable;

    out = {};
    out.base = windowStart;
    out.bctr = bctrAddr;
    out.r = idx;
    out.kind = "absolute-unguarded";
    for (uint32_t i = 0; i < 1024; i++)
    {
        uint32_t label = be32(tbl + 4 * i);
        if (label < fnStart || label >= fnEnd || (label & 3))
            break;
        out.labels.push_back(label);
    }
    if (out.labels.size() < 2)
        return Fail::LabelOutside;
    out.def = out.labels[0];
    (void)text;
    return Fail::None;
}

int main(int argc, char** argv)
{
    setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc < 3)
    {
        printf("uso: rayman_jumptables <xex> <saida.toml>\n");
        return 1;
    }

    auto file = LoadFile(argv[1]);
    auto image = Image::ParseImage(file.data(), file.size());
    const Section* text = image.Find(".text");
    if (!text) { printf("sem .text\n"); return 1; }

    std::map<uint32_t, uint32_t> pdataFns; // início -> tamanho
    const Section* pdata = image.Find(".pdata");
    for (uint32_t off = 0; pdata && off + 8 <= pdata->size; off += 8)
    {
        uint32_t begin = be32(pdata->data + off);
        uint32_t data = be32(pdata->data + off + 4);
        pdataFns[begin] = ((data >> 8) & 0x3FFFFF) * 4; // FunctionLength (bits 8..29)
    }

    std::set<uint32_t> starts;
    for (auto& [b, s] : pdataFns) starts.insert(b);
    // Símbolos já conhecidos da imagem, como os thunks de import ("__imp__NtCreateFile"):
    // uma função "por ponteiro" no mesmo endereço substituiria o import na tabela.
    for (const auto& symbol : image.symbols) starts.insert(uint32_t(symbol.address));
    for (uint32_t i = 0; i < text->size / 4; ++i)
    {
        uint32_t w = be32(text->data + 4 * i);
        if ((w >> 26) == 18 && (w & 3) == 1) // bl (não absoluto)
        {
            int32_t off = (int32_t)((w & 0x03FFFFFC) << 6) >> 6;
            uint32_t t = text->base + 4 * i + off;
            if (inText(*text, t)) starts.insert(t);
        }
    }


    // Limites da função que contém um endereço: entrada do .pdata ou, sem ela,
    // o intervalo entre inícios conhecidos.
    auto functionBounds = [&](uint32_t address, uint32_t& start, uint32_t& end) {
        auto p = pdataFns.upper_bound(address);
        if (p != pdataFns.begin())
        {
            --p;
            if (address < p->first + p->second) { start = p->first; end = p->first + p->second; return; }
        }
        auto it = starts.upper_bound(address);
        end = it == starts.end() ? text->base + text->size : *it;
        start = it == starts.begin() ? text->base : *std::prev(it);
    };

    std::vector<Switch> found;
    std::map<Fail, int> fails;
    std::vector<uint32_t> unsupported; // bctr com guarda mas padrão desconhecido

    auto* code = reinterpret_cast<const uint32_t*>(text->data);
    for (uint32_t i = 0; i < text->size / 4; ++i)
    {
        uint32_t a = text->base + 4 * i;
        ppc_insn in;
        ppc::Disassemble(&code[i], a, in);
        if (!in.opcode || in.opcode->id != PPC_INST_BCTR) continue;

        Switch sw;
        Fail f = Resolve(image, *text, a, sw);
        if (f == Fail::NoGuard)
        {
            uint32_t fnStart, fnEnd;
            functionBounds(a, fnStart, fnEnd);
            Fail g = ResolveUnguarded(image, *text, a, fnStart, fnEnd, sw);
            if (g == Fail::None)
                f = Fail::None;
        }
        if (f == Fail::None) found.push_back(std::move(sw));
        else
        {
            ++fails[f];
            if (f != Fail::NoGuard) unsupported.push_back(a);
            else
            {
                // Sem guarda: suspeito se houver leitura indexada de tabela logo antes.
                for (uint32_t k = 1; k <= 10 && k <= i; ++k)
                {
                    ppc_insn p;
                    ppc::Disassemble(&code[i - k], a - 4 * k, p);
                    if (!p.opcode) continue;
                    if (p.opcode->id == PPC_INST_BLR || p.opcode->id == PPC_INST_BCTR) break;
                    if (p.opcode->id == PPC_INST_LWZX || p.opcode->id == PPC_INST_LHZX || p.opcode->id == PPC_INST_LBZX)
                    {
                        unsupported.push_back(a);
                        break;
                    }
                }
            }
        }
    }

    std::string out = "# Gerado por tools/jumptables/rayman_jumptables (Rayman Origins X360)\n\n";
    std::map<std::string, int> kinds;
    char buf[64];
    for (const auto& sw : found)
    {
        ++kinds[sw.kind];
        snprintf(buf, sizeof(buf), "# %s\n[[switch]]\n", sw.kind); out += buf;
        snprintf(buf, sizeof(buf), "base = 0x%X\n", sw.base); out += buf;
        snprintf(buf, sizeof(buf), "r = %u\n", sw.r); out += buf;
        snprintf(buf, sizeof(buf), "default = 0x%X\n", sw.def); out += buf;
        out += "labels = [\n";
        for (uint32_t l : sw.labels) { snprintf(buf, sizeof(buf), "    0x%X,\n", l); out += buf; }
        out += "]\n\n";
    }

    FILE* f = fopen(argv[2], "wb");
    if (!f) { printf("não consegui abrir %s\n", argv[2]); return 1; }
    fwrite(out.data(), 1, out.size(), f);
    fclose(f);

    // Limites de função. O recompilador confia no .pdata; funções-folha sem
    // .pdata são analisadas estaticamente e cortadas no switch (parece tail
    // call). Início real = maior início conhecido (.pdata ou alvo de bl) <= base;
    // fim = próximo início conhecido. Se o .pdata já cobre os labels, nada a fazer.
    if (argc >= 4)
    {
        std::string fnOut = "functions = [\n";
        int emitted = 0, covered = 0, broken = 0, ctrFns = 0, ptrFns = 0;
        std::set<uint32_t> done;

        // ---- 1. Todos os inícios conhecidos, incluindo os alcançados só por ponteiro ----
        // Endereços de código em dados (vtables, callbacks) e pares lis/addi no código:
        // funções reais mesmo sem bl nem .pdata.
        std::set<uint32_t> dataPtrs;
        for (const auto& s : image.sections)
        {
            if ((s.flags & SectionFlags_Code) || s.data == nullptr) continue;
            if (s.base + s.size > image.base + image.size) continue; // ex.: .reloc fora da imagem
            for (uint32_t off = 0; off + 4 <= s.size; off += 4)
            {
                uint32_t v = be32(s.data + off);
                if (inText(*text, v)) dataPtrs.insert(v);
            }
        }

        std::set<uint32_t> switchLabels;
        std::map<uint32_t, uint32_t> switchReach; // base do switch -> maior label
        for (const auto& sw : found)
        {
            uint32_t hi = 0;
            for (uint32_t l : sw.labels) { switchLabels.insert(l); hi = std::max(hi, l); }
            switchReach[sw.base] = hi;
        }

        std::set<uint32_t> pointerTargets(dataPtrs.begin(), dataPtrs.end());
        const char* ptrMode = getenv("JT_PTR_MODE"); // bissecção: data | code | all (padrão)
        bool useCodePtrs = !ptrMode || strcmp(ptrMode, "data") != 0;
        if (ptrMode && !strcmp(ptrMode, "code")) pointerTargets.clear();
        for (uint32_t i = 0; useCodePtrs && i + 1 < text->size / 4; ++i)
        {
            uint32_t w = be32(text->data + 4 * i);
            if ((w >> 26) != 15 || ((w >> 16) & 0x1F) != 0) continue; // lis rD, hi
            uint32_t rd = (w >> 21) & 0x1F, hi = w << 16;
            for (uint32_t j = 1; j <= 6 && i + j < text->size / 4; ++j)
            {
                uint32_t w2 = be32(text->data + 4 * (i + j));
                uint32_t op = w2 >> 26;
                if ((op == 14 || op == 24) && ((w2 >> 16) & 0x1F) == rd) // addi/ori rX, rD, lo
                {
                    uint32_t lo = w2 & 0xFFFF;
                    uint32_t target = op == 14 ? hi + uint32_t(int32_t(int16_t(lo))) : (hi | lo);
                    if (inText(*text, target)) pointerTargets.insert(target);
                    break;
                }
            }
        }

        auto isTerminatorOrPad = [](uint32_t w) {
            return w == 0x4E800020 || w == 0x4E800420 || w == 0 || ((w >> 26) == 18 && (w & 1) == 0);
        };
        // Alvos de desvio condicional são labels internos: bc nunca sai da função.
        std::set<uint32_t> branchTargets;
        for (uint32_t i = 0; i < text->size / 4; ++i)
        {
            uint32_t w = be32(text->data + 4 * i);
            if ((w >> 26) == 16 && (w & 3) == 0)
                branchTargets.insert(text->base + 4 * i + uint32_t(int32_t(int16_t(w & 0xFFFC))));
        }
        // Dentro de uma função do .pdata (tamanho exato), só o início é função.
        auto insidePdataFunction = [&](uint32_t address) {
            auto p = pdataFns.upper_bound(address);
            if (p == pdataFns.begin()) return false;
            --p;
            return address > p->first && address < p->first + p->second;
        };

        std::set<uint32_t> allStarts = starts;
        std::vector<uint32_t> newStarts;
        for (uint32_t t : pointerTargets)
        {
            if (starts.count(t) || switchLabels.count(t) || t == text->base) continue;
            if (branchTargets.count(t) || insidePdataFunction(t)) continue;
            // Entre um switch e o último caso dele: é corpo da função, não um início.
            bool insideSwitch = false;
            for (const auto& [base, hi] : switchReach)
                if (t > base && t <= hi) { insideSwitch = true; break; }
            if (insideSwitch) continue;
            if (!isTerminatorOrPad(be32(text->data + (t - 4 - text->base)))) continue;
            newStarts.push_back(t);
            allStarts.insert(t);
        }
        if (const char* limit = getenv("JT_PTR_LIMIT")) // bissecção: só as N primeiras
            newStarts.resize(std::min<size_t>(newStarts.size(), strtoul(limit, nullptr, 10)));

        // ---- 2. Medição comum: acompanha o alvo mais distante dos desvios internos ----
        // (e dos labels de switch); termina num blr/bctr/tail call além dele ou no
        // próximo início conhecido. Um b para um início conhecido é tail call.
        auto nextKnownStart = [&](uint32_t address) {
            auto it = allStarts.upper_bound(address);
            return it == allStarts.end() ? text->base + text->size : *it;
        };
        auto walkFunction = [&](uint32_t start, uint32_t minEnd, bool* ctrBranch, uint32_t* firstRet) {
            uint32_t nextStart = nextKnownStart(start);
            uint32_t maxT = std::max(start, minEnd);
            for (uint32_t pos = start; pos < nextStart; pos += 4)
            {
                uint32_t w = be32(text->data + (pos - text->base));
                uint32_t op = w >> 26;
                auto reach = switchReach.find(pos);
                if (reach != switchReach.end()) maxT = std::max(maxT, reach->second);
                bool terminator = w == 0x4E800020 || w == 0x4E800420 || w == 0;
                if (op == 16 && (w & 3) == 0)
                {
                    maxT = std::max(maxT, pos + uint32_t(int32_t(int16_t(w & 0xFFFC))));
                    if (ctrBranch && ((w >> 21) & 0x4) == 0) *ctrBranch = true; // BO: decrementa CTR
                }
                else if (op == 18 && (w & 3) == 0)
                {
                    uint32_t t = pos + uint32_t((int32_t)((w & 0x03FFFFFC) << 6) >> 6);
                    if (t > pos && t < nextStart && !allStarts.count(t)) maxT = std::max(maxT, t);
                    else terminator = true; // tail call ou salto para trás
                }
                if (terminator)
                {
                    if (firstRet && !*firstRet) *firstRet = pos;
                    if (pos >= maxT) return pos + 4;
                }
            }
            return nextStart;
        };

        // ---- 3a. Funções-folha (sem .pdata) com jump table ----
        for (const auto& sw : found)
        {
            auto it = allStarts.upper_bound(sw.base);
            --it;
            uint32_t start = *it;
            uint32_t lo = sw.def, hi = sw.def;
            for (uint32_t l : sw.labels) { lo = std::min(lo, l); hi = std::max(hi, l); }

            auto pd = pdataFns.find(start);
            if (pd != pdataFns.end() && lo >= start && hi < start + pd->second) { ++covered; continue; }
            if (done.count(start)) continue;

            uint32_t end = walkFunction(start, hi + 4, nullptr, nullptr);
            if (lo < start || hi >= end)
            {
                printf("  ! switch 0x%X: labels [0x%X,0x%X] fora de [0x%X,0x%X)\n", sw.base, lo, hi, start, end);
                ++broken;
                continue;
            }
            done.insert(start);
            snprintf(buf, sizeof(buf), "    { address = 0x%X, size = 0x%X },\n", start, end - start);
            fnOut += buf;
            ++emitted;
        }

        // ---- 3b. "Switches" com contador: mtctr rN + sequência de bdz/bdnz ----
        // O analisador do recompilador para no primeiro blr e transforma os alvos
        // seguintes em funções falsas.
        for (uint32_t start : starts)
        {
            if (pdataFns.count(start) || done.count(start)) continue;
            bool ctrBranch = false;
            uint32_t firstRet = 0;
            uint32_t end = walkFunction(start, 0, &ctrBranch, &firstRet);
            if (!ctrBranch || !firstRet || end <= firstRet + 4) continue;
            done.insert(start);
            snprintf(buf, sizeof(buf), "    { address = 0x%X, size = 0x%X }, # bdz\n", start, end - start);
            fnOut += buf;
            ++ctrFns;
        }

        // ---- 3c. Funções alcançadas só por ponteiro ----
        for (uint32_t start : newStarts)
        {
            if (done.count(start)) continue;
            uint32_t end = walkFunction(start, 0, nullptr, nullptr);
            snprintf(buf, sizeof(buf), "    { address = 0x%X, size = 0x%X }, # ptr\n", start, end - start);
            fnOut += buf;
            ++ptrFns;
        }

        fnOut += "]\n";
        printf("limites: %d cobertos pelo .pdata, %d funções com switch, %d com bdz, %d por ponteiro, %d problemáticos\n",
               covered, emitted, ctrFns, ptrFns, broken);

        // Dicas no formato do manifesto do ReXGlue ([functions] e [[switch_tables]]).
        if (argc >= 5)
        {
            std::string hints = "# Gerado por tools/jumptables (Rayman Origins X360) para o ReXGlue.\n\n[functions]\n";
            // O ReXGlue não aceita funções sobrepostas. As funções de switch/bdz vêm
            // primeiro na lista (tamanho deduzido da estrutura) e têm prioridade: uma
            // candidata "por ponteiro" que cair dentro de uma já aceita é descartada.
            std::vector<std::pair<uint32_t, uint32_t>> entries; // (início, tamanho), na ordem de prioridade
            size_t pos = 0;
            while ((pos = fnOut.find("{ address = ", pos)) != std::string::npos)
            {
                unsigned address = 0, size = 0;
                sscanf(fnOut.c_str() + pos, "{ address = 0x%X, size = 0x%X }", &address, &size);
                entries.emplace_back(address, size);
                pos += 12;
            }
            std::map<uint32_t, uint32_t> accepted; // início -> fim
            int dropped = 0;
            for (auto [address, size] : entries)
            {
                uint32_t end = address + size;
                auto next = accepted.lower_bound(address);
                bool overlaps = (next != accepted.end() && next->first < end) ||
                                (next != accepted.begin() && std::prev(next)->second > address);
                if (overlaps) { ++dropped; continue; }
                accepted[address] = end;
            }
            for (auto [address, end] : accepted)
            {
                snprintf(buf, sizeof(buf), "0x%08X = { size = 0x%X }\n", address, end - address);
                hints += buf;
            }
            printf("dicas ReXGlue: %zu funções (%d sobrepostas descartadas), %zu tabelas\n", accepted.size(), dropped, found.size());
            for (const auto& sw : found)
            {
                snprintf(buf, sizeof(buf), "\n[[switch_tables]]\naddress = 0x%08X\n", sw.bctr);
                hints += buf;
                snprintf(buf, sizeof(buf), "register = %u\nlabels = [", sw.r);
                hints += buf;
                for (size_t i = 0; i < sw.labels.size(); i++)
                {
                    snprintf(buf, sizeof(buf), "%s0x%08X", i ? ", " : "", sw.labels[i]);
                    hints += buf;
                }
                hints += "]\n";
            }
            FILE* hf = fopen(argv[4], "wb");
            fwrite(hints.data(), 1, hints.size(), hf);
            fclose(hf);
        }

        FILE* ff = fopen(argv[3], "wb");
        fwrite(fnOut.data(), 1, fnOut.size(), ff);
        fclose(ff);
    }

    printf("jump tables resolvidas: %zu\n", found.size());
    for (auto& [k, n] : kinds) printf("  %-12s %d\n", k.c_str(), n);
    printf("bctr não resolvidos:\n");
    const char* names[] = { "ok", "sem guarda cmplwi/bgt (chamada indireta?)", "padrão não suportado",
                            "índice sobrescrito", "tabela inválida", "label fora do .text" };
    for (auto& [k, n] : fails) printf("  %-42s %d\n", names[(int)k], n);
    if (!unsupported.empty())
    {
        printf("com guarda mas não resolvidos (investigar):\n");
        for (uint32_t a : unsupported) printf("  0x%08X\n", a);
    }
    return 0;
}
