TOport v0.1.5: a grande atualização de desempenho. Fim das travadas de shader, abertura mais rápida e uma tela de carregamento nova. / The big performance update. No more shader stutter, faster startup and a new loading screen.

> ⭐ **Gostou do TOport? Deixe uma estrela no [GitHub](https://github.com/IgorAugustoHH/The-Order-1886-PC-Port).** Isso ajuda mais gente a encontrar o projeto. / **Enjoying TOport? Star it on GitHub.** It helps more people find the project.

## Nesta versão / In this version

### Desempenho / Performance
- **Fim das travadas de 200 a 400 ms no menu e ao entrar em áreas novas.** O shader que ilumina as partículas (fumaça, faíscas, fogo; até 16 luzes por lote) recebia restos de dados nas luzes que o lote não usa. Cada combinação desses restos virava uma variação nova do shader, compilada na hora, e o jogo parava. Agora as luzes não usadas são ignoradas, como no PS4. As luzes usadas continuam iguais, e a imagem não muda. Nos testes, as compilações durante o jogo caíram de **mais de 1.000 para menos de 10 por sessão**. Momentos com travadas acima de 100 ms caíram de 14 para 0 a partir da terceira abertura. O mapeamento se baseia na análise de artemkaVG no [shadPS4 #5218](https://github.com/shadps4-emu/shadPS4/pull/5218). / **No more 200-400 ms stutters in the menu and new areas.** The particle lighting shader (smoke, sparks, fire; up to 16 lights per batch) received leftover data in the lights a batch does not use. Each leftover combination became a new shader variant compiled on the spot, and the game stalled. Unused lights are now ignored, as on PS4. Used lights are unchanged, so the picture does not change. In testing, in-game compiles dropped from **over 1,000 to under 10 per session**, and stutters above 100 ms went from 14 to 0 by the third start. Mapping based on artemkaVG's analysis in shadPS4 #5218.
- **Shaders de compute compilam em segundo plano.** Quando surge uma variação nova, o jogo não para mais. Na primeira vez, aquele lote de partículas pode ficar sem luz por alguns quadros. / **Compute shaders compile in the background.** A new variant no longer stalls the game. The first time, that particle batch may be unlit for a few frames.
- **Cache de pipelines do driver ligado.** Shaders já vistos carregam em milissegundos nas próximas aberturas, com proteção de memória no salvamento. / **Driver pipeline cache on.** Shaders seen before load in milliseconds on later starts, with a memory guard when saving.
- **Abertura mais rápida.** O pré-carregamento leva cerca de 5 segundos. / **Faster startup.** The preload takes about 5 seconds.
- **Menu a ~118 FPS** com o limite em 120. / **Menu at ~118 FPS** with the 120 limit.

### Novo / New
- **Tela de pré-carregamento.** Antes a janela ficava preta. Agora um carrossel com três artes do TOport mostra a barra de progresso e o contador de shaders. / **Preload screen.** The window used to stay black. A carousel of three TOport artworks now shows a progress bar and the shader count.
- **Botão do GitHub no launcher**, para acompanhar o projeto e deixar sua estrela. / **GitHub button in the launcher**, to follow the project and leave a star.

## Tudo o que melhorou desde a primeira versão / Everything improved since the first release
- **Imagem que congelava com o jogo rodando (v0.1.4).** Contagens absurdas de índices em draws indiretos agora são limitadas como no PS4. Correção de [JulioCacko](https://github.com/JulioCacko/The-Order-1886_pc). / **Picture freezing while the game kept running (v0.1.4).** Huge indirect index counts are now clamped as on PS4. Fix by JulioCacko.
- **Pré-carregamento de shaders que funciona de verdade (v0.1.4 e v0.1.5).** Os shaders das últimas sessões são preparados na abertura, e as variações de iluminação não se multiplicam mais. / **Shader preloading that really works (v0.1.4 and v0.1.5).** The last sessions' shaders are prepared at startup, and lighting variants no longer multiply.
- **Teclado e mouse junto com o controle (v0.1.4).** O mouse controla a câmera e mira, e você pode trocar entre teclado e controle quando quiser. / **Keyboard and mouse together with a gamepad (v0.1.4).** The mouse aims and turns the camera, and you can switch between keyboard and gamepad at any time.
- **Saída experimental em 1440p/4K com DLSS ou FSR 3.1 (v0.1.4).** Baseada no trabalho de [mfrafael](https://github.com/mfrafael). / **Experimental 1440p/4K output with DLSS or FSR 3.1 (v0.1.4).** Based on work by mfrafael.
- **Mais dumps 01.02 abrem (v0.1.2 e v0.1.3).** O modo foto e a resolução encontram o código pelo padrão das instruções. Não é mais preciso um eboot idêntico byte a byte. / **More 01.02 dumps start (v0.1.2 and v0.1.3).** Photo mode and resolution find the code by its instruction pattern, so a byte-identical eboot is no longer required.
- **Launcher em português e inglês, aviso de versão nova e menu no Insert (v0.1.1).** / **Launcher in Portuguese and English, update notice and menu on Insert (v0.1.1).**

### Ao atualizar / When updating
Na primeira abertura com a v0.1.5, o shader de iluminação monta o novo conjunto de variações, então essa sessão ainda pode ter algumas compilações. Da segunda abertura em diante, fica limpo. / On the first start with v0.1.5 the lighting shader builds its new set of variants, so that session may still compile a few. From the second start on it is clean.

### Se travar / If it crashes
Abra uma [issue](https://github.com/IgorAugustoHH/The-Order-1886-PC-Port/issues) com o arquivo `last_run.log` da pasta do TOport, sua placa de vídeo, driver e RAM, e onde aconteceu. Deixe o arquivo de paginação do Windows num SSD. / Open an issue with `last_run.log` from the TOport folder, your GPU, driver and RAM, and where it happened. Keep the Windows page file on an SSD.

Requer o seu próprio dump de The Order: 1886 (CUSA00785) **versão 01.02**. Nenhum arquivo do jogo incluído. / Requires your own The Order: 1886 dump (CUSA00785) **version 01.02**. No game files included.
