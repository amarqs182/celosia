# R36S port — plano de engenharia (consolidado)

> **Nota.** Este é o documento canônico do plano de engenharia do port para o
> R36S. A documentação de estado e arquitetura do port é `docs/R36S_PORT.md`;
> esse arquivo ainda não existe nesta branch, então use os documentos
> equivalentes publicados na branch `r36s`:
> [`docs/STATUS.md`](https://github.com/amarqs182/celosia/blob/r36s/docs/STATUS.md)
> e [`docs/r36s_port.md`](https://github.com/amarqs182/celosia/blob/r36s/docs/r36s_port.md).
>
> Este plano consolida três fontes: a base factual medida no device (PR #9), o
> processo de revisão de documentação (PR #1) e o estado real do port na
> branch `r36s`. Onde as revisões antigas divergiam, o texto abaixo reflete o
> que foi medido no alvo e o que o port realmente faz hoje.

## 1. Onde o port vive

Regra de repositório: o `upstream` (`HenryKun55/celosia`) é referência somente-leitura e **não** recebe push nem PR; toda contribuição vai para `amarqs182/celosia:main` (ver `CONTRIBUTING.md`).

- O trabalho de port está na branch `r36s`, **não** na `main`.
- A `main` atual **não contém** o port Linux: ela não compila o caminho
  Linux (renderer GL, shim de plataforma, backend SDL2/GLES).
- Contribuições entram na `main` por Pull Request, a partir de branches de
  trabalho separadas.
- Documentos que tratam do port R36S podem citar a branch `r36s` como fonte
  de estado; nunca tratar a `main` como se já tivesse o port Linux.

## 2. Fatos medidos no alvo (2026-11-09)

Valores lidos no próprio R36S, não tirados de datasheet:

| Item | Valor medido |
| --- | --- |
| Userland / ABI | `aarch64`, ELF64 (kernel `4.4.189`, Debian 13.7 trixie) |
| Board / SoC | `rockchip,rk3326-odroidgo3-linux` (RK3326) |
| CPU | 4x **Cortex-A35** (`CPU part 0xd04`), `asimd/aes/sha1/sha2/crc32`, **sem extensão Cryptographic** |
| RAM | `MemTotal 916.896 kB` (~**895 MiB**) + swap 524.284 kB |
| GPU | Mali Midgard (`arm,malit602`), OpenGL ES 3.2 disponível |
| Stack GL | **libmali** (Bifrost/Midgard), **não** Mesa userland |
| DRM | `card0` + `renderD128`, driver `rockchip-drm` |
| Framebuffer | 640x480 @ 32 bpp |
| Bibliotecas | SDL2 2.0.3200 + `libGLESv2` + `libEGL` (mesa) presentes; **sem X11/Wayland** |
| Toolchain do device | `gcc 12.4`, `glibc 2.41` |
| Ferramentas ausentes no device | `bsdtar` e `ninja` **ausentes** |
| Armazenamento | `/` em btrfs 11G, `/roms` em exfat 19G |

Correções que mudam o plano de build, e não apenas a redação:

- **O userland é aarch64, não ARM 32-bit.** `uname -m` retorna `aarch64` e o
  loader é `aarch64-linux-gnu`. O caminho `__arm__ && !__aarch64__` do
  runtime não é o alvo.
- **A pilha GL é libmali, não Mesa.** As bibliotecas Mesa existem no device,
  mas o caminho de GL ativo é o libmali (Bifrost/Midgard). Trate o alvo como
  libmali + OpenGL ES 3.2.
- **Item de GBM/kernel-oops.** Criar display GBM em `/dev/dri/renderD128` e
  inicializá-lo provoca NULL-deref no kernel (`Oops: 96000005`), deixa o
  processo em estado `D` e exige power cycle físico. O display deve ser
  propriedade do SDL2 (driver KMSDRM).
- **Orçamento de memória: 895 MiB**, não 1 GB.
- **Toolchain: `aarch64-linux-gnu`**, não `arm-linux-gnueabihf`.

## 3. Estado real do port (não é hipótese)

O que já foi medido rodando o port no device:

- O binário `ps2sotc_aarch64_pie_sdl` (~51,3 MB) **foi construído e executado
  no device**.
- `sceCdSearchFile` funciona host-side: 2 buscas OK (`MANAGER.XFF` e
  `GAMECORE.XFF`).
- 128 uploads de imagem (`frame:upload`) sem wrap.
- `sceCdRead = 0` no run inteiro (o guest não lê CD).
- `shownFps ~ 0`.
- **Após o `jalr` do loader (`source=0x1b5bcc` -> `target=0x1344788`) o guest
  não produz nenhum evento novo**: sem `vfs:open`, sem `printf`, sem `ios:*`.
- Sem `sceGsResetGraph` vindo do guest, `PMODE`/`SMODE2`/`DISPFB`/`DISPLAY`
  nunca são programados e o `GS::Present` não recebe pedido válido -> nenhuma
  imagem chega à tela.
- O ring de frames é `kFrames = 8`.

## 4. O que a árvore já dá

- Runtime recompilado pronto para alvo novo (o código do jogo é C++ puro).
- Backend raylib de desktop (base da camada de plataforma).
- Renderer nativo (`native_renderer_gl.cpp`), com a interface de backend
  (`native_backend.h`) como costura a preservar.
- Overrides do jogo (`game/overrides`) já inclusos em builds não-Vita.
- Build não-Vita funcional (o caminho Vita continua intacto e separado).

## 5. O que está no caminho

1. **O guest não executa após o `jalr` do loader.** É o bloqueio principal.
   Investigar a estrutura de módulo lida pelo loader contra o `.cmd`: o entry
   declarado é `isysGetXffEntryPointPreRelocation = 0x001b5cd0`, enquanto o
   `.text` do `GAMECORE.XFF` é relocável (offsets `0x6968`, base
   `0x40000000`). A divergência entre a posição esperada e a posição real do
   entry explica a ausência de eventos.
2. **Sem `sceGsResetGraph` não há `Present`.** A "primeira imagem" depende de
   resolver o item 1: só depois que o guest rodar e programar o display
   ambiente o `GS::Present` recebe pedido válido. Não é um problema de
   hookup de contexto GL.
3. **Shaders `#version 330 core` precisam de variante GLSL ES.** As
   `#version 330 core` estão tanto em `native_renderer_gl.cpp` quanto em
   `gs_hw_backend.cpp` (7 ocorrências) — não apenas no primeiro arquivo.
4. **`glMapBuffer`: os dois sítios ficam fora do caminho Linux.**
   `native_renderer_gl.cpp:3944` e `:4234` estão sob
   `#if defined(PS2X_NATIVE_VITA)` e portanto não compilam no alvo Linux. Se
   um dia forem portados, o equivalente GLES é `glMapBufferOES` /
   `glMapBufferRange`. **Não é bloqueador.**
5. **Toolchain:** `aarch64-linux-gnu` (não `arm-linux-gnueabihf`), com sysroot
   copiado do device.
6. **Orçamento de memória: 895 MiB**, não 1 GB. Pools de renderer, texturas e
   render targets precisam caber nesse número.
7. **Propriedade do display:** o SDL2 deve ser dono do dispositivo DRM/KMSDRM.
   **Não** criar display GBM em `/dev/dri/renderD128` — em teste anterior isso
   causou NULL-deref no kernel (oops), processo em estado `D` e power cycle.
8. **Ausência de `bsdtar` e `ninja` no device** afeta scripts e build local:
   usar `unzip`/`tar` no device e o `ninja` do host para buildar.

## 6. Checks de sanidade antes de começar

Rodar no R36S antes de qualquer mudança:

```sh
uname -m
getconf LONG_BIT
dpkg --print-architecture 2>/dev/null || true
file /bin/sh
ls -l /dev/dri
ldd --version
```

## 7. Sequência de trabalho (7 passos)

1. **Congelar o ambiente de build.** Toolchain `aarch64-linux-gnu` + sysroot
   copiado do device; build de referência no host para comparação.
2. **Shim de plataforma `platform/r36s` sobre SDL2 (KMSDRM).** A branch
   `r36s` já tem uma implementação em `platform/linux/raylib_linux.cpp`
   (`EndDrawing` -> `SDL_GL_SwapWindow`, linhas 673-676) e o PR #8
   (`feat/r36s-linux-sdl2-gles-shim`) traz outra implementação:
   **consolidar em uma só**, sem manter dois caminhos de plataforma.
3. **Variantes GLSL ES dos shaders** (item 5.3), com smoke de contexto e
   **context ownership entre threads**: o contexto GL é criado na thread que
   desenha e fica corrente sob demanda; um contexto SDL único não pode estar
   corrente em duas threads ao mesmo tempo.
4. **Forçar `SDL_VIDEODRIVER=kmsdrm`** no startup do R36S.
5. **Resolver o bloqueio do guest** (itens 5.1 e 5.2) para obter a primeira
   imagem.
6. **Defaults:** render scale 1 e teto de 30 fps.
7. **Medir e iterar** com o log de frames e o `[gl:mem]`, antes de cortar mais
   efeitos.

## 8. Status de validação

Já rodamos no device (logs em `build/`, **não versionados**):

- Build do binário do jogo para aarch64 (`ps2sotc_aarch64_pie_sdl`) e execução
  no R36S.
- Leitura do loader até o `jalr` para o entry do `GAMECORE.XFF`.
- Busca de arquivos no CD (`sceCdSearchFile`) e 128 uploads de imagem.
- Observação do stall: nenhum evento novo do guest depois do `jalr`, com
  `sceCdRead=0` e `shownFps ~ 0`.

Ainda não roda:

- Gameplay / primeira imagem na tela.
- Medição real de fps e de tempo de frame (o `shownFps` não mede swaps reais).
- Comparação de pictures, `SOTC_DIFFTEST` e o pipeline de setup/capture no
  alvo.
- Qualquer validação do caminho GLES no device (o renderer nativo ainda não
  foi exercitado no Linux).

## 9. Regras

- Contribuições na `main` entram por Pull Request.
- Nada de `git add -A`: staging explícito, arquivo por arquivo.
- Sem segredos no repositório: credenciais do device, tokens e chaves privadas
  não vão para o repositório nem para log commitado.
- Device: sem flash, sem instalar system-wide, sem apagar arquivos do
  sistema.
- Nunca rodar `ls /proc/[0-9]*/fd/*` no device (congela o R36S).
- `pkill` somente dentro de script atômico, que também restaura o estado.
- Restaurar o EmulationStation após cada teste.
- Não usar `ffmpeg -f kmsgrab` nem `fbdev` no device (congela o display).
