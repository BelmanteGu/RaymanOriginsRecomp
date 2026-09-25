// Diagnóstico do XEX do Rayman Origins — reutiliza XenonUtils.
// Lista seções, conta mtctr/bctr, e localiza as funções save/restore de
// registradores pelos padrões de bytes do README do XenonRecomp.
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <vector>
#include <map>
#include <string>
#include <algorithm>
#include <file.h>
#include <image.h>
#include <disasm.h>
#include <ppc-inst.h>

static void findPattern(const Image& image, const char* name,
                        const uint8_t* pat, size_t patLen)
{
    int hits = 0;
    for (const auto& s : image.sections)
    {
        // Só código: seções como .reloc podem passar do fim da imagem mapeada.
        if (!(s.flags & SectionFlags_Code) || s.data == nullptr) continue;
        for (size_t i = 0; i + patLen <= s.size; ++i)
        {
            if (memcmp(s.data + i, pat, patLen) == 0)
            {
                if (hits < 8)
                    printf("  %-16s @ 0x%08zX  (section %s)\n",
                           name, s.base + i, s.name.c_str());
                ++hits;
            }
        }
    }
    if (hits == 0) printf("  %-16s NÃO ENCONTRADO\n", name);
    else if (hits > 8) printf("  %-16s ... (%d ocorrências no total)\n", name, hits);
}

int main(int argc, char** argv)
{
    setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc < 2) { printf("uso: diag <xex>\n"); return 1; }

    auto file = LoadFile(argv[1]);

    // Modo "callers": diag <xex> callers <endereço> — lista os bl que apontam para o endereço.
    if (argc >= 4 && strcmp(argv[2], "callers") == 0)
    {
        auto img = Image::ParseImage(file.data(), file.size());
        const Section* text = img.Find(".text");
        uint32_t target = strtoul(argv[3], nullptr, 16);
        int n = 0;
        for (uint32_t i = 0; i < text->size / 4; ++i)
        {
            const uint8_t* p = text->data + 4 * i;
            uint32_t w = (p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3];
            if ((w >> 26) != 18 || (w & 3) != 1) continue;
            int32_t off = (int32_t)((w & 0x03FFFFFC) << 6) >> 6;
            if (uint32_t(text->base + 4 * i + off) == target)
            {
                printf("  bl de 0x%08zX\n", text->base + 4 * i);
                ++n;
            }
        }
        printf("  %d chamadas\n", n);
        return 0;
    }

    // Modo "dump": diag <xex> dump <endereço> — desmonta 16 instruções antes e 2 depois.
    if (argc >= 4 && strcmp(argv[2], "dump") == 0)
    {
        auto img = Image::ParseImage(file.data(), file.size());
        size_t addr = strtoull(argv[3], nullptr, 16);
        ppc_insn in;
        for (size_t a = addr - 16 * 4; a <= addr + 2 * 4; a += 4)
        {
            auto* p = (const uint32_t*)img.Find(a);
            ppc::Disassemble(p, a, in);
            printf("  %s0x%08zX  %-8s %s\n", a == addr ? ">" : " ", a,
                   in.opcode ? in.opcode->name : "???", in.op_str);
        }
        return 0;
    }
    printf("Arquivo: %s (%zu bytes)\n", argv[1], file.size());

    auto image = Image::ParseImage(file.data(), file.size());
    printf("\n=== IMAGE ===\n");
    printf("base        = 0x%08zX\n", image.base);
    printf("size        = 0x%08X (%u)\n", image.size, image.size);
    printf("entry_point = 0x%08zX\n", image.entry_point);
    printf("sections    = %zu\n", image.sections.size());

    printf("\n=== SEÇÕES ===\n");
    for (const auto& s : image.sections)
    {
        const char* fl = s.flags == SectionFlags_Code ? "CODE" :
                         s.flags == SectionFlags_Data ? "DATA" : "none";
        printf("  %-12s base=0x%08zX size=0x%08X flags=%s\n",
               s.name.c_str(), s.base, s.size, fl);
    }

    // Conta mtctr, bctr e pares mtctr->bctr nas seções de código.
    printf("\n=== mtctr / bctr (indicador de jump tables) ===\n");
    ppc_insn insn, insn2;
    size_t nMtctr = 0, nBctr = 0, nPair = 0;
    for (const auto& s : image.sections)
    {
        if (!(s.flags & SectionFlags_Code) || s.data == nullptr) continue;
        size_t count = s.size / 4;
        auto* code = reinterpret_cast<const uint32_t*>(s.data);
        for (size_t i = 0; i < count; ++i)
        {
            ppc::Disassemble(&code[i], s.base + i * 4, insn);
            if (insn.opcode == nullptr) continue;
            if (insn.opcode->id == PPC_INST_MTCTR)
            {
                ++nMtctr;
                // procura bctr nas próximas 4 instruções
                for (size_t j = 1; j <= 4 && (i + j) < count; ++j)
                {
                    ppc::Disassemble(&code[i + j], s.base + (i + j) * 4, insn2);
                    if (insn2.opcode && insn2.opcode->id == PPC_INST_BCTR) { ++nPair; break; }
                    if (insn2.opcode && insn2.opcode->id == PPC_INST_BCTRL) break;
                }
            }
            else if (insn.opcode->id == PPC_INST_BCTR) ++nBctr;
        }
    }
    printf("  mtctr total          = %zu\n", nMtctr);
    printf("  bctr  total          = %zu\n", nBctr);
    printf("  mtctr->bctr (<=4 ins)= %zu   <- candidatos a jump table\n", nPair);

    // Histograma das 8 instruções antes de cada mtctr que precede um bctr.
    // Mostra os padrões reais de jump table deste compilador.
    printf("\n=== PADRÕES ANTES DE mtctr->bctr (top 15) ===\n");
    std::map<std::string, std::pair<int, size_t>> sigs; // assinatura -> (qtd, exemplo)
    for (const auto& s : image.sections)
    {
        if (!(s.flags & SectionFlags_Code) || s.data == nullptr) continue;
        size_t count = s.size / 4;
        auto* code = reinterpret_cast<const uint32_t*>(s.data);
        for (size_t i = 8; i < count; ++i)
        {
            ppc::Disassemble(&code[i], s.base + i * 4, insn);
            if (!insn.opcode || insn.opcode->id != PPC_INST_MTCTR) continue;
            bool pair = false;
            for (size_t j = 1; j <= 4 && (i + j) < count; ++j)
            {
                ppc::Disassemble(&code[i + j], s.base + (i + j) * 4, insn2);
                if (insn2.opcode && insn2.opcode->id == PPC_INST_BCTR) { pair = true; break; }
            }
            if (!pair) continue;
            std::string sig;
            for (size_t k = 8; k >= 1; --k)
            {
                ppc::Disassemble(&code[i - k], s.base + (i - k) * 4, insn2);
                sig += insn2.opcode ? insn2.opcode->name : "???";
                sig += ' ';
            }
            sig += "| mtctr";
            auto& e = sigs[sig];
            if (e.first++ == 0) e.second = s.base + i * 4;
        }
    }
    std::vector<std::pair<std::string, std::pair<int, size_t>>> sorted(sigs.begin(), sigs.end());
    std::sort(sorted.begin(), sorted.end(),
              [](auto& a, auto& b) { return a.second.first > b.second.first; });
    for (size_t n = 0; n < sorted.size() && n < 15; ++n)
        printf("  %4d x  (ex. 0x%08zX)  %s\n", sorted[n].second.first,
               sorted[n].second.second, sorted[n].first.c_str());
    printf("  (%zu assinaturas distintas)\n", sorted.size());

    printf("\n=== FUNÇÕES SAVE/RESTORE DE REGISTRADORES (padrões do README) ===\n");
    struct Pat { const char* name; std::vector<uint8_t> bytes; };
    std::vector<Pat> pats = {
        {"restgprlr_14", {0xE9,0xC1,0xFF,0x68}},
        {"savegprlr_14", {0xF9,0xC1,0xFF,0x68}},
        {"restfpr_14",   {0xC9,0xCC,0xFF,0x70}},
        {"savefpr_14",   {0xD9,0xCC,0xFF,0x70}},
        {"restvmx_14",   {0x39,0x60,0xFE,0xE0,0x7D,0xCB,0x60,0xCE}},
        {"savevmx_14",   {0x39,0x60,0xFE,0xE0,0x7D,0xCB,0x61,0xCE}},
        {"restvmx_64",   {0x39,0x60,0xFC,0x00,0x10,0x0B,0x60,0xCB}},
        {"savevmx_64",   {0x39,0x60,0xFC,0x00,0x10,0x0B,0x61,0xCB}},
    };
    for (auto& p : pats)
        findPattern(image, p.name, p.bytes.data(), p.bytes.size());

    return 0;
}
