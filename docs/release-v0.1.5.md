TOport v0.1.5: fim das travadas de shader de iluminação no menu e no jogo. / No more lighting shader stutter in the menu and in game.

### Corrigido / Fixed
- **Travadas de 200 a 400 ms no menu e ao entrar em áreas novas:** o shader de iluminação das partículas (até 16 luzes por lote) recebia restos de dados nas luzes que o lote não usa, e cada combinação de resto virava uma variação nova do shader, compilada na hora. Agora as luzes não usadas são ignoradas, como no PS4; as usadas continuam iguais. Nos testes, as compilações durante o jogo caíram de mais de 1.000 para menos de 10 por sessão e as variações novas pararam de surgir a partir da segunda abertura. Mapeamento baseado na análise de artemkaVG no [shadPS4 #5218](https://github.com/shadps4-emu/shadPS4/pull/5218). / **200-400 ms stutters in the menu and new areas:** the particle lighting shader (up to 16 lights per batch) received leftover data in the lights a batch does not use, and each leftover combination became a new shader variant compiled on the spot. Unused lights are now ignored, as on PS4; used ones are unchanged. In testing, in-game compiles dropped from over 1,000 to under 10 per session. Mapping based on artemkaVG's analysis in shadPS4 #5218.
- **Menu a ~118 FPS** com o limite em 120. / **Menu at ~118 FPS** with the 120 limit.

### Mudado / Changed
- **Shaders de compute compilam em segundo plano:** quando surge uma variação nova, o jogo não para mais; aquele lote de partículas pode ficar sem luz por alguns quadros na primeira vez. / **Compute shaders compile in the background:** a new variant no longer stalls the game; that particle batch may be unlit for a few frames the first time.
- **Cache de pipelines do driver ligado:** shaders já vistos carregam em milissegundos nas próximas aberturas (com proteção de memória no salvamento). / **Driver pipeline cache on:** shaders seen before load in milliseconds on later starts (with a memory guard when saving).
- **Tela de pré-carregamento:** em vez de uma janela preta, um carrossel com três artes do TOport, a barra de progresso e o contador de shaders. / **Preload screen:** instead of a black window, a carousel of three TOport artworks, a progress bar and the shader count.
- A abertura caiu para cerca de 5 segundos de pré-carregamento. / Startup preload is down to about 5 seconds.

### Se travar / If it crashes
Abra uma [issue](https://github.com/IgorAugustoHH/The-Order-1886-PC-Port/issues) com o arquivo `last_run.log` da pasta do TOport, sua placa de vídeo, driver e RAM, e onde aconteceu. Deixe o arquivo de paginação do Windows num SSD. / Open an issue with `last_run.log` from the TOport folder, your GPU, driver and RAM, and where it happened. Keep the Windows page file on an SSD.

Requer o seu próprio dump de The Order: 1886 (CUSA00785) **versão 01.02**. Nenhum arquivo do jogo incluído. / Requires your own The Order: 1886 dump (CUSA00785) **version 01.02**. No game files included.
