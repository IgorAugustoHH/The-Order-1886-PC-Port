# The Order: mapa do fim do quadro para upscaling acima da resolução do jogo

Captura `frame_1792426922` (v0.1.3, modo Original 1920x800, DLAA, cutscene; 125 passes).
Base para pôr um upscaler (DLSS, FSR 3.1, FSR 4.1.1, FSR 1) com saída maior que 1920x800.

## Sequência depois da cena

| Passes | O que faz | Saída |
| --- | --- | --- |
| #52-#61 | cena HDR (opacos, transparentes, partículas em 960x400) | `0x1055100000` RGBA16F 2x MSAA |
| #63 | resolve temporal (ps `c1737167`), lê velocidade `0x104b538000` e histórico | `0x1049c00000` RGBA16F 1920x800 |
| #64-#68 | redução em R16G16 (960x400 até 60x25) | |
| #69 | composição de volta na cena | `0x1055100000` RGBA16F 1920x800 |
| #70-#77 | DoF e motion blur em meia resolução (960x400) | `0x1056450000` RGBA16F 1920x800 |
| #78-#80 | exposição (compute, reduz até 1x1) | `0x1053cc0000` R32F 1x1 |
| #81-#105 | bloom (960x400 até 240x100) e brilho de lente (256x256) | |
| #106 | tonemap: lê profundidade, HDR, bloom, brilho e uma textura 512x512 (grão/LUT) | `0x1057c28000` RGBA8 sRGB 1920x800 |
| #107 | conversão final da cena | `0x10550f8000` A2B10G10R10 1920x800 |
| #108-#124 | interface (vs `94720a3237573b18`, só raster): legendas, ícones, ping-pong de desfoque em RGBA8 1920x800 | `0x10550f8000` |
| #0 (quadro seguinte) | cópia com faixas pretas para o buffer de exibição | `0x1049400000` RGBA8 sRGB 1920x1080 |

## Ponto escolhido para o upscaler: saída de #77

- Entrada HDR já com DoF e motion blur, antes de bloom, tonemap e grão: a ordem que DLSS e FSR
  recomendam. O grão e a aberração entram depois, já na resolução de saída.
- Profundidade `0x104d508000` (D32 1920x800) e velocidade `0x104b538000` existem nesse ponto.
- Depois dele só há raster de tela cheia e computes de redução:
  - bloom e exposição podem continuar em 1920x800 (lendo a versão nativa);
  - tonemap (#106), conversão (#107), interface (#108-#124) e a cópia final (#0) passam a
    desenhar em proxies com o tamanho de saída (por exemplo 3840x1600 dentro de 3840x2160).
- Em modo upscaling, o resolve #63 deixa de ser temporal (só MSAA): o acúmulo temporal passa
  a ser do upscaler.

## Jitter

O jogo não tem jitter (`order_dlss.md`). Sem ele o upscale recupera pouco detalhe. Proposta:
deslocar o viewport em subpixel (sequência Halton) nos passes da cena (#2-#61) e informar o
mesmo deslocamento ao upscaler. Os computes que reconstroem posição pela profundidade usam a
matriz sem jitter: erro de no máximo meio pixel, a medir.
