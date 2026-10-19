# TOport v0.1.0 — The Order: 1886 nativo no PC (pré-lançamento)

Primeira versão pública do **TOport**, o port nativo de **The Order: 1886** (PS4) para Windows,
construído sobre o [BBPort](https://github.com/Supermedo/bloodborne_pc) e o núcleo de vídeo do
[shadPS4](https://github.com/shadps4-emu/shadPS4). O código do jogo roda direto no processador;
só as funções do sistema e a GPU são traduzidas.

![Londres vista do telhado](https://raw.githubusercontent.com/IgorAugustoHH/The-Order-1886-PC-Port/main/docs/screenshots/londres-telhado.jpg)

## Destaques
- **Modo Original do PS4:** a câmera cinematográfica 2.40:1 com as faixas pretas nativas, como no
  console, e a iluminação global estável. Opções 16:9 em tela cheia também disponíveis.
- **NVIDIA DLSS** no lugar da resolução temporal do jogo: DLAA, Qualidade, Equilibrado, Desempenho e
  Ultra Desempenho. Numa RTX 2060: ~40–45 FPS em Qualidade e até ~80 FPS em Desempenho.
- **Carregamento acelerado:** texturas e iluminação chegam muito mais rápido, sem perder nada da cena.
- **Launcher TOport** com verificação do jogo, idiomas (23, português do Brasil por padrão) e todas
  as opções; menu dentro do jogo com **Select** (controle) ou **Insert** (teclado); **F9** troca o
  limite de quadros.
- Pré-carga de shaders, câmera livre nas cutscenes, correções de rostos e partículas.

## Como jogar
1. Baixe `TOport.zip` abaixo e extraia numa pasta (de preferência num SSD).
2. Abra `TOport.exe` e, em **Jogo e idioma**, aponte a pasta do **seu próprio dump** de
   The Order: 1886 (**CUSA00785, versão 01.02**).
3. Clique em **JOGAR**. A primeira execução prepara o jogo e compila os shaders enquanto você joga.

**Requisitos:** Windows 10/11 64 bits, GPU Vulkan 1.3 com 6 GB (DLSS: NVIDIA RTX), 16 GB de RAM.
**Dica:** deixe o arquivo de paginação do Windows num SSD/NVMe; num HD o jogo engasga.

## Estado
O jogo roda sem crash nos trechos testados, com cutscenes, combate e exploração. Todos os capítulos
estão sendo testados agora. Em andamento: algumas piscadas de iluminação em áreas específicas,
rostos com pontos pretos ainda não mapeados e uma cutscene com sincronia a revisar. 1440p e 4K
ainda não são suportados.

## Aviso legal
Projeto de fã, sem fins lucrativos e **sem ligação, aprovação ou patrocínio** da Sony Interactive
Entertainment, da Ready at Dawn Studios ou da NVIDIA. "PlayStation", "PS4" e "The Order: 1886" são
marcas dos seus donos. **Nenhum arquivo do jogo é distribuído**: é preciso o seu próprio dump de uma
cópia que você possui legalmente. Fornecido "como está", sem garantias. Licença GPL-2.0-or-later;
DLSS sob a licença da NVIDIA incluída no pacote.

Agradecimentos completos no [README](https://github.com/IgorAugustoHH/The-Order-1886-PC-Port#agradecimentos).
