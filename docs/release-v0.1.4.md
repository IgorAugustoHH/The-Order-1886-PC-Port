TOport v0.1.4: menos congelamentos, shaders pré-carregados de verdade e saída experimental em 1440p/4K. / Fewer freezes, shader preloading that really works and experimental 1440p/4K output.

### Corrigido / Fixed
- **Imagem congelando com o jogo aberto:** às vezes o jogo pede um número absurdo de índices num draw indireto. O PS4 ignora o excesso, mas no PC a GPU travava. Agora a contagem é limitada ao buffer de índices, como no hardware original (correção de [JulioCacko](https://github.com/JulioCacko/The-Order-1886_pc)). / **Picture freezing while the game kept running:** the game sometimes asks an indirect draw for a huge index count. The PS4 ignores the excess, but on PC the GPU stalled. The count is now clamped to the index buffer, as on the original hardware (fix by JulioCacko).
- **"Pré-carregar shaders" agora funciona:** a lista de pipelines recentes não era salva, então nada era pré-carregado. A partir da segunda vez que você abre o jogo, menos travadas na primeira vez em cada área. / **"Preload shaders" now works:** the recent pipelines list was never saved. From the second start on, fewer first-use stutters.

### Novo / New
- **Teclado e mouse:** agora funcionam sempre, junto com o controle (antes o teclado só funcionava sem controle conectado). WASD anda, o **mouse controla a câmera**, botão direito mira (L2), botão esquerdo atira (R2), Espaço X, Shift Círculo, E Quadrado, Q Triângulo, Enter/Esc Options. Mirando, a câmera segue o movimento do mouse; sem mirar, gira como as setas. / **Keyboard and mouse:** now always work, together with a gamepad. WASD moves, the **mouse turns the camera**, right button aims (L2), left button fires (R2), Space Cross, Shift Circle, E Square, Q Triangle, Enter/Esc Options.
- **Launcher mais simples:** a tela inicial mostra só o essencial; a aba Imagem foi dividida em Imagem do jogo, Saída acima de 1080p (experimental) e Efeitos. / **Simpler launcher:** the main page shows the essentials; the Graphics page is split into game picture, experimental output above 1080p and effects.

### Novo (experimental) / New (experimental)
- **Resolução de saída 1440p e 4K** com DLSS ou FSR 3.1, para monitores 1440p/4K: o jogo continua desenhando a cena em 1920x800 e o quadro pronto é ampliado. Em monitor 1080p não há ganho. Há um artefato conhecido no menu acima de 1080p. Baseado no trabalho de [mfrafael](https://github.com/mfrafael). / **1440p and 4K output** with DLSS or FSR 3.1, for 1440p/4K monitors: the scene stays at 1920x800 and the finished frame is upscaled. No gain on 1080p monitors. Known menu artifact above 1080p. Based on work by mfrafael.

### Se travar / If it crashes
Abra uma [issue](https://github.com/IgorAugustoHH/The-Order-1886-PC-Port/issues) com o arquivo `last_run.log` da pasta do TOport, sua placa de vídeo, driver e RAM, e onde aconteceu. Deixe o arquivo de paginação do Windows num SSD. / Open an issue with `last_run.log` from the TOport folder, your GPU, driver and RAM, and where it happened. Keep the Windows page file on an SSD.

Requer o seu próprio dump de The Order: 1886 (CUSA00785) **versão 01.02**. Nenhum arquivo do jogo incluído. / Requires your own The Order: 1886 dump (CUSA00785) **version 01.02**. No game files included.
