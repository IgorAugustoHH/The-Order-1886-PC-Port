TOport v0.1.3: corrige o erro de resolução (`proporcao original diverge` / `bytes originais divergem`) ao iniciar em 1920x800 ou 1920x1080. / Fixes the resolution error at start in 1920x800 or 1920x1080.

### Corrigido / Fixed
- **Resolução em dumps 01.02 diferentes:** o patch de resolução agora encontra o código pelo padrão das instruções, aceitando eboots limpos ou com outros patches do GoldHEN, em vez de exigir os bytes exatos do dump analisado ([#2](https://github.com/IgorAugustoHH/The-Order-1886-PC-Port/issues/2)). / **Resolution on other 01.02 dumps:** the resolution patch now finds the code by its instruction pattern, accepting clean eboots or ones with other GoldHEN patches, instead of requiring the analyzed dump's exact bytes.

### Se travar / If it crashes
Abra uma [issue](https://github.com/IgorAugustoHH/The-Order-1886-PC-Port/issues) com o arquivo `last_run.log` da pasta do TOport, sua placa de vídeo, driver e RAM, e onde aconteceu. Deixe o arquivo de paginação do Windows num SSD. / Open an issue with `last_run.log` from the TOport folder, your GPU, driver and RAM, and where it happened. Keep the Windows page file on an SSD.

Requer o seu próprio dump de The Order: 1886 (CUSA00785) **versão 01.02**. Nenhum arquivo do jogo incluído. / Requires your own The Order: 1886 dump (CUSA00785) **version 01.02**. No game files included.
