## StinkOS

### Descrição

Este projeto é um sistema operacional minimalista, desenvolvido especificamente para executar jogos selecionados a partir de um menu inicial. O objetivo principal é criar um ambiente de execução padronizado e eficiente para jogos, esperando que todos tenham um tamanho uniforme e sejam facilmente selecionáveis e jogáveis a partir do menu.

### Componentes do Projeto

1. **Bootloader (boot.s)**: Responsável pela inicialização do sistema, carregando o kernel e preparando o ambiente de execução.
2. **Kernel (kernel.c)**: Contém as funcionalidades básicas do sistema operacional, incluindo a exibição de mensagens e a infraestrutura necessária para executar os jogos.
3. **Makefile**: Script de automação que compila e monta o bootloader e o kernel, gerando a imagem binária do sistema operacional.

Este sistema operacional é projetado para funcionar em uma arquitetura x86 e é compilado e testado usando as ferramentas da coleção de softwares de código livre GNU, tais quais `i386-elf-gcc`, `i386-elf-as`, `i386-elf-ld` e o emulador `qemu-system-i386`.

### To-do List
+ **Terminar os drivers e bibliotecas de Vídeo (screen.h) e IO (io.h).**
+ **Estruturar as o arquivo binário que comportará todo o sistema operacional (os.bin).**
+ **Planejar a game engine que será futuramente integrada ao Kernel do OS.**
