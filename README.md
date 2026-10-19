# The Order: 1886 para Windows — TOport

**The Order: 1886 rodando nativo no Windows 10/11: câmera cinematográfica original do PS4, NVIDIA DLSS,
até 120 FPS, 23 idiomas e um launcher próprio.**

O jogo roda direto no processador do PC, sem emulação de CPU. Só o que muda entre PS4 e PC é traduzido:
as funções do sistema da Sony e a GPU, cujos comandos e shaders viram Vulkan. Sem janela de emulador,
sem scripts de configuração: você aponta a pasta do seu jogo e clica em JOGAR.

**Versão atual: [v0.1.2](https://github.com/IgorAugustoHH/The-Order-1886-Nativo-PC/releases/tag/v0.1.2) (pré-lançamento)** ·
**[⬇ Baixar a versão mais recente](https://github.com/IgorAugustoHH/The-Order-1886-Nativo-PC/releases/latest)**

![Londres vista do telhado, rodando nativo no PC](docs/screenshots/londres-telhado.jpg)

| | |
|---|---|
| ![Corredor](docs/screenshots/corredor.jpg) | ![Telhado em cutscene](docs/screenshots/telhado-cutscene.jpg) |

*Capturas do TOport numa RTX 2060, modo Original do PS4 (2.40:1) com DLSS.*

> **Nenhum arquivo do jogo vem incluído.** É preciso o seu próprio dump de **The Order: 1886**
> (CUSA00785), de uma cópia que você possui legalmente, **com a atualização 1.02 aplicada**
> (versão do jogo **01.02**). O TOport confere a versão e recusa outras.

## Recursos
- **Modo Original do PS4:** a câmera cinematográfica 2.40:1 com as faixas pretas nativas, exatamente
  como no console, e a iluminação global estável. Também há opções em tela cheia 16:9.
- **NVIDIA DLSS** no lugar da resolução temporal do próprio jogo: DLAA, Qualidade, Equilibrado,
  Desempenho e Ultra Desempenho.
- **Até 120 FPS:** limite de 30/60/90/120 no launcher, no menu ou com **F9** durante o jogo.
- **Carregamento acelerado:** texturas e iluminação chegam muito mais rápido, sem perder nada da cena.
- **Pré-carga de shaders:** ao abrir, o jogo já monta os shaders das últimas sessões.
- **23 idiomas** do sistema do PS4, com português do Brasil por padrão.
- **Menu dentro do jogo** com **Insert** (teclado): troque DLSS, resolução,
  limite de quadros e idioma sem sair do jogo.
- **Câmera livre nas cutscenes** (modo foto).
- Controle de PlayStation, Xbox ou teclado.

## Desempenho (i5-10400F, RTX 2060 6 GB, 16 GB de RAM)
| Configuração | Cenas pesadas |
|---|---|
| Original + TAA do jogo | ~45–50 FPS |
| Original + DLSS Qualidade | ~40–45 FPS |
| Original + DLSS Desempenho | até ~80 FPS |

## Requisitos
- **The Order: 1886 (CUSA00785) atualizado para a versão 01.02**: o dump do jogo base com o
  patch 1.02 já mesclado. As correções do executável só funcionam nessa versão.
- Windows 10 ou 11, 64 bits.
- GPU com Vulkan 1.3 e 6 GB de VRAM. DLSS exige NVIDIA RTX.
- 16 GB de RAM e CPU de 6 núcleos.
- **Arquivo de paginação do Windows num SSD/NVMe.** Num HD mecânico o jogo engasga quando a RAM aperta.

## Como jogar
1. Baixe `TOport-Windows.zip` em **[Releases](https://github.com/IgorAugustoHH/The-Order-1886-Nativo-PC/releases/latest)**.
2. Extraia em qualquer pasta (de preferência num SSD).
3. Abra **`TOport.exe`**.
4. Em **Jogo e idioma**, escolha a pasta do seu dump **01.02** (a que tem `eboot.bin`). O launcher
   mostra "✓ CUSA00785 · versão 01.02" quando estiver certo.
5. Clique em **JOGAR**.

Nada mais para instalar. A primeira vez prepara o jogo e compila os shaders enquanto você joga;
da segunda em diante tudo fica mais suave.

## Problemas conhecidos
- Algumas áreas ainda têm piscadas de iluminação.
- Pontos pretos no rosto de alguns personagens (corrigidos nos já mapeados).
- Uma cutscene com sincronia a revisar.
- 1440p e 4K ainda não são suportados.

Todos os capítulos estão sendo testados agora; cada versão nova traz mais correções.

## Problemas e feedback
Abra uma [issue](https://github.com/IgorAugustoHH/The-Order-1886-Nativo-PC/issues) com:
- o arquivo **`last_run.log`** da pasta do TOport;
- sua placa de vídeo, driver e quanto de RAM você tem;
- onde aconteceu (capítulo e cena) e, se der, um print.

**Não peça nem compartilhe arquivos do jogo nas issues.**

## Compilar (Windows)
1. MSYS2 com o ambiente CLANG64 e os pacotes listados em `packaging/windows/README.md`.
2. `bash build.sh` (com `MSYS2_ROOT` apontando para o MSYS2, se não estiver em `C:\msys64`).
3. `bash packaging/windows/package_toport.sh` gera `dist/TOport/` e `dist/TOport-Windows.zip`.
   Para desenvolvimento: `BB_GAME_DIR=<pasta do CUSA00785> python run.py`.

## Aviso legal
- Projeto de fã, sem fins lucrativos, **sem qualquer ligação, aprovação ou patrocínio** da Sony
  Interactive Entertainment, da Ready at Dawn Studios ou da NVIDIA.
- "PlayStation", "PS4" e os respectivos logotipos são marcas registradas da Sony Interactive
  Entertainment Inc. "The Order: 1886" é marca da Sony Interactive Entertainment LLC. Todas as
  outras marcas pertencem aos seus donos.
- **Nenhum arquivo do jogo é distribuído**: nem executável, nem dados, nem shaders, nem artes
  extraídas do disco. As imagens do launcher e deste README são capturas do TOport rodando.
  O cache de shaders é gerado na máquina de cada jogador.
- Este repositório não contém nem ensina a contornar proteções de cópia.
- O software é fornecido "como está", sem garantias. Use por sua conta e risco.

## Licença
GPL-2.0-or-later, herdada do BBPort e do shadPS4. Bibliotecas de terceiros: ver
[`README.bbport.md`](README.bbport.md). O DLSS segue a licença da NVIDIA incluída no pacote
(`licenses/NVIDIA-DLSS-LICENSE.txt`).

---

## Agradecimentos
O TOport só existe porque outras pessoas abriram o caminho e publicaram o seu trabalho. Obrigado a:

- **[BBPort / Bloodborne for Windows](https://github.com/Supermedo/bloodborne_pc)**, de
  **deadinside28** (Linux) e **Supermedo** (Windows): a base inteira deste port, do carregador nativo
  ao launcher e ao menu do jogo.
- **[shadPS4](https://github.com/shadps4-emu/shadPS4)** e toda a sua equipe: o núcleo de vídeo Vulkan
  e o recompilador de shaders. As anotações sobre The Order no
  [PR #4798](https://github.com/shadps4-emu/shadPS4/pull/4798) ajudaram na investigação das sombras.
- **illusion0001**, pelos [patches de The Order](https://github.com/illusion0001/PS4-PS5-Game-Patch)
  que mostraram onde ficam a resolução e a proporção da câmera do jogo.
- **Ready at Dawn**, por criar The Order: 1886 e pela palestra do SIGGRAPH 2015 sobre o renderizador,
  que guiou a integração do DLSS.
- **NVIDIA**, pelo DLSS.
- Projetos consultados, com ideias testadas aqui:
  [ShadowOfTheColossus-PC](https://github.com/AjncSystem/ShadowOfTheColossus-PC) (**AjncSystem**), os
  ports de **HayatoG** e o [bbhost](https://github.com/droogie/bbhost) (**droogie**).
- O **auxiliador (Codex)**, pelas correções de rosto, partículas, mira e pelo menu em português.
