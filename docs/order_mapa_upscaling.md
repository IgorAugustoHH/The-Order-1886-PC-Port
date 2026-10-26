# The Order: saída acima de 1080p (DLSS e FSR 3.1)

O jogo continua renderizando a cena em 1920x800 (modo Original). Com a resolução de saída em
1440p ou 4K, o quadro pronto (depois do tonemap e antes da interface) é ampliado pelo upscaler;
a interface e a cópia final são desenhadas no tamanho de saída.

- `bbport.ini`: `order_output` (1080, 1440, 2160), `order_upscaler` (dlss, fsr3, off; fsr4 e
  fsr411 só pelo ini, não testados), `sharpness` (0 a 1, vale na hora).
- Ambiente (run.py): `BB_ORDER_OUTPUT=WxH`, `BB_ORDER_UPSCALER`, `BB_ORDER_OUTPUT_GAME_TAA=1`
  com o anti-aliasing "TAA original + upscaler".
- Em 1080p (padrão) nada disso é ativado.

## Sequência do fim do quadro

Captura `frame_1792426922` (v0.1.3, modo Original 1920x800; 125 passes).

| Passes | O que faz | Saída |
| --- | --- | --- |
| #52-#61 | cena HDR (opacos, transparentes, partículas em 960x400) | `0x1055100000` RGBA16F 2x MSAA |
| #63 | resolve temporal (`customresolve_cs_msaa*_temporal`); também reduz a velocidade | `0x1049c00000` RGBA16F 1920x800 |
| #64-#68 | máximo da velocidade em blocos (960x400 até 60x25) | R16G16 |
| #69 | motion blur (ps `574764e2`): resolve + velocidade + blocos | `0x1055100000` RGBA16F 1920x800 |
| #70-#77 | DoF em meia resolução; #77 (ps `4bb962d1`) compõe | `0x1056450000` RGBA16F 1920x800 |
| #78-#105 | exposição, bloom e brilho de lente | |
| #106 | tonemap (grão/LUT 512x512) | RGBA8 sRGB 1920x800 |
| #107 | conversão final da cena | `0x10550f8000` A2B10G10R10 1920x800 |
| #108-#124 | interface (vs `94720a3237573b18`) | `0x10550f8000` |
| #0 (quadro seguinte) | cópia com faixas pretas para a tela (busca por posição de pixel) | RGBA8 sRGB 1920x1080 |

## Implementação (`vk_temporal_upscaler.cpp`, modo Order)

- O primeiro desenho da interface em #107 dispara o upscale. Os bits A2B10G10R10 do quadro são
  lidos como B8G8R8A8 sRGB (a cópia final do jogo faz o mesmo).
- Vetores de movimento e profundidade vêm do compute de preparação do resolve
  (`OrderDlss::PrepOnly`, `order_dlss_prep.comp`). Verificados em capturas: a escala 1 encaixa
  quadro a quadro, inclusive no movimento dos personagens; a velocidade do jogo satura em
  16 px por quadro (giros rápidos de câmera).
- A interface é desenhada no tamanho de saída. Durante ela, os samplers com filtro "point" viram
  bilineares, porque os ícones em 1,33x repetiam linhas de texel.
- A cópia final para a tela é feita pelo port (fundo preto e o quadro centralizado).
- Nitidez: um RCAS depois de qualquer upscaler. O RCAS do próprio FSR 3.1 fica desligado, e o
  DLSS ignora a nitidez desde a versão 2.5.1.

## Achados dos testes (2026-10-19)

- **TAA original + upscaler é o padrão.** Sem o resolve do jogo (só MSAA, o upscaler fazendo o
  trabalho temporal), partes de rostos somem (queixo, barba), com DLSS ou FSR 3.1. O resolve
  original filtra amostras dos vizinhos e aplica o TAA com limite de cor por vizinhança; "Pelo
  upscaler" fica como experimental.
- **DLSS usa o preset F (6) ao ampliar a saída.** O preset K deixava várias cópias das bordas em
  movimento (orelha, cabelo) mesmo com vetores e profundidade corretos. `BB_DLSS_PRESET` ainda
  substitui.
- O FSR 3.1 não mostrou rastro em nenhum modo.
- Motion blur (#69) e DoF (#77) podem ser desligados na hora (menu e launcher): o passe é
  trocado pela cópia da sua entrada.

Testado: 1440p e 4K, DLSS e FSR 3.1, TAA original + upscaler, com o eboot 01.02 de SHA-256
`88feb160…` (diferente do analisado no projeto; os shaders batem pelos hashes do resolve e do
rosto).

## Pendente

- Jitter: implementado (Halton no viewport dos passes da cena) só para "Pelo upscaler", e
  desligado por padrão (`jitter=0`). Com o TAA original ele borraria a imagem.
- FSR 4 / 4.1.1: caminho existe, sem teste.
