# Botões de toque: mapa para os desenhos

Referência para desenhar os botões temáticos do controle na tela (Android). Hoje o app desenha círculos com as letras do Xbox. Quando os desenhos existirem, o app passa a usá-los no lugar.

## O que cada botão faz no jogo

O Rayman Origins só usa pular, atacar e correr. X e B atacam do mesmo jeito, qualquer gatilho ou botão de ombro corre, e o Y não é usado no gameplay ([StrategyWiki](https://strategywiki.org/wiki/Rayman_Origins/Controls)).

| Arquivo | Botão no Xbox | Ação no jogo | Nos menus | Ideia de desenho | Tamanho atual* |
|---|---|---|---|---|---|
| `btn_jump` | A | **Pular** (segurar no ar = planar com o cabelo) | confirmar | Rayman saltando / seta pra cima | raio 70 |
| `btn_attack` | X | **Atacar** (soco) | — | punho / explosão de impacto | raio 70 |
| `btn_run` | RT | **Correr** (segurar) | — | pés com rastro de velocidade / ≫ | raio 60 |
| `btn_back` | B | também ataca | **voltar** | seta de retorno | raio 70 |
| `btn_y` | Y | não usado | — | (pode sair do layout) | raio 70 |
| `btn_pause` | Start | **pausar** | abrir o menu | ❚❚ | raio 42 |
| `btn_select` | Back | menu secundário | — | ☰ | raio 42 |
| `btn_settings` | — | abre as configurações do app | — | engrenagem | raio 42 |
| `stick_base` | analógico esquerdo | **andar** (↓ abaixa, ↑ + atacar = golpe pra cima) | navegar | anel / base | raio 150 |
| `stick_knob` | analógico esquerdo | pino do analógico | — | bolinha | raio 63 |

\* Em pixels numa tela de 1080 px de altura, com tamanho 100%. O app escala tudo pelo controle deslizante "Tamanho dos controles".

## Formato dos arquivos

- **PNG com fundo transparente, 512×512 px** e o desenho centralizado, com uma margem de ~8% pra borda não cortar. **SVG** também serve, e aí eu converto.
- **Dois estados por botão:** normal (`btn_jump.png`) e pressionado (`btn_jump_pressed.png`). Se não houver o pressionado, o app clareia o normal.
- Formato **redondo**: os toques são testados num círculo do tamanho do botão.
- A opacidade é aplicada pelo app (controle deslizante), então desenhe **totalmente opaco**.
- Onde colocar: `android/app/src/main/res/drawable-nodpi/`, com esses nomes exatos, ou me mande os arquivos que eu coloco.

## Layout atual (paisagem)

```
 [☰ select]   [❚❚ pause]                          [⚙]

                                          (Y)
                                    (X)        (B)
   ( stick )                              (A)
                                    (RT)
```

O analógico é flutuante: aparece onde o polegar encosta, na metade esquerda da tela. Se quiser outro arranjo, desenhe por cima de um print do jogo e me mande.
