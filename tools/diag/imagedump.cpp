// Grava a imagem carregada do XEX num arquivo plano: offset = endereço - base.
// Usado pela análise estática (RTTI, strings, busca de funções). A saída é
// derivada do jogo: fica em private/.
#include <cstdio>
#include <cstdint>
#include <file.h>
#include <image.h>

int main(int argc, char** argv)
{
    if (argc < 3)
    {
        fprintf(stderr, "uso: %s default.xex saida.bin\n", argv[0]);
        return 1;
    }
    auto file = LoadFile(argv[1]);
    Image image = Image::ParseImage(file.data(), file.size());
    for (const auto& s : image.sections)
        printf("%-8s 0x%08zX-0x%08zX %s\n", s.name.c_str(), size_t(s.base), size_t(s.base + s.size),
               (s.flags & SectionFlags_Code) ? "code" : "data");
    FILE* f = fopen(argv[2], "wb");
    fwrite(image.data.get(), 1, image.size, f);
    fclose(f);
    printf("base 0x%08zX, %u bytes, entry 0x%08zX\n", image.base, image.size, image.entry_point);
}
