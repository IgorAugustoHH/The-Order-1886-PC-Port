TOport v0.1.2: corrige o erro "eboot diferente da imagem analisada" ao iniciar. / Fixes the "eboot diferente da imagem analisada" error at start.

### Corrigido / Fixed
- **Dumps 01.02 de outras ferramentas agora abrem:** o modo foto e a resolução de varejo exigiam um eboot idêntico byte a byte ao analisado. Agora conferem só as instruções originais nos endereços alterados, então qualquer dump CUSA00785 01.02 com o código certo funciona. / **01.02 dumps from other tools now start:** photo mode and retail resolution required an eboot identical byte for byte to the analyzed one. They now check only the original instructions at the patched addresses, so any CUSA00785 01.02 dump with the right code works.

### Se travar / If it crashes
Abra uma [issue](https://github.com/IgorAugustoHH/The-Order-1886-PC-Port/issues) com o arquivo `last_run.log` da pasta do TOport, sua placa de vídeo, driver e RAM, e onde aconteceu. Deixe o arquivo de paginação do Windows num SSD. / Open an issue with `last_run.log` from the TOport folder, your GPU, driver and RAM, and where it happened. Keep the Windows page file on an SSD.

Requer o seu próprio dump de The Order: 1886 (CUSA00785) **versão 01.02**. Nenhum arquivo do jogo incluído. / Requires your own The Order: 1886 dump (CUSA00785) **version 01.02**. No game files included.
