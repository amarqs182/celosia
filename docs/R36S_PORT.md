# Port de Shadow of the Colossus para o R36S

> Status: em desenvolvimento — o jogo ainda não chega ao gameplay. Última atualização: 2026-11-09.

## 1. O que é este fork
- Este fork (`amarqs182/celosia`) é dedicado ao port de Shadow of the Colossus (PS2, USA, `SCUS-97472`) para o console portátil R36S (aarch64).
- `main`: base compartilhada com o projeto original (`HenryKun55/celosia`) e **branch onde as contribuições devem ser enviadas** (via PR).
- `r36s`: linha de trabalho do port. Contém o runtime adaptado, overrides do jogo, scripts de build/smoke e a documentação técnica de estado ([docs/STATUS.md](https://github.com/amarqs182/celosia/blob/r36s/docs/STATUS.md), [docs/r36s_port.md](https://github.com/amarqs182/celosia/blob/r36s/docs/r36s_port.md)). Ela fica atrás da `main` e recebe atualizações por merge.
- Regra de ouro: **não abra PR contra a `r36s`**; abra contra a `main`.

## 2. Objetivo técnico
- Rodar o jogo sem emular CPU x86: recompilação estática do código MIPS do jogo (PS2Recomp/Celosia) gera C++ nativo, executado como ELF **aarch64**.
- Backend de vídeo: **SDL2 + KMSDRM + GLES** (sem X11, sem Wayland).
- Restrições do projeto: nada de EGL/Mesa/GBM como dependência do port, nada de emulação de CPU, nada de interpretador de MIPS no caminho crítico.

## 3. Hardware confirmado (coletado no aparelho em 2026-11-09)
| Campo | Valor |
| --- | --- |
| Kernel | `Linux dArkOSen-r36 4.4.189 #1 SMP Thu Oct 1 02:03:12 UTC 2026 aarch64` |
| Distro | `Debian GNU/Linux 13 (trixie) 13.7` |
| Placa | `G80CA-MB V1.2-20250422 Panel 8` |
| Compatible | `rockchip,rk3326-odroidgo3-linux` / `rockchip,rk3326` |
| CPU | 4× Cortex-A35 (`CPU part 0xd04`), features `fp asimd evtstrm aes pmull sha1 sha2 crc32` |
| RAM | `MemTotal 916896 kB` (~896 MiB), `SwapTotal 524284 kB` |
| GPU | família Mali Midgard (`arm,malit602`, `arm,mali-midgard`) |
| DRM | `card0`, `renderD128`, driver `rockchip-drm` |
| Framebuffer | `640,480` @ `32` bpp |
| Backlight | presente |
| Armazenamento | `/` = `/dev/mmcblk0p2` btrfs 11G (1.8G livres), `/roms` = `/dev/mmcblk0p3` exfat 19G (15G livres) |

Bibliotecas presentes: `libSDL2-2.0.so.0.3200.x`, `libGLESv2.so.2`, `libEGL.so.1` (mesa), dev packages (`libSDL2.a`, `libSDL2main.a`).
Toolchain no device: `gcc 12.4.0`, `ldd/glibc 2.41`, `python 3.13.5`, `cmake`, `git`, `curl`, `unzip`, `tar`, `gzip`.
**Ausentes no device**: `ninja`, `bsdtar`. (Consequência prática: qualquer script que passe a depender de `bsdtar` precisa de fallback para `unzip`/`tar`.)
Display: painel 640×480 (fb0).

## 4. Layout do jogo no device (confirmado)
Diretório `/roms/ports/SotC_Celosia/`:
| Arquivo | Tamanho | MD5 |
| --- | --- | --- |
| `SCUS_974.72` | ELF do jogo (242.940 B) | `0c08d02e7919681366171f75a2da797d` |
| `GAMECORE.XFF` | 2.141.864 | `4c297ea066591cd4f09e76fa419942b5` |
| `MANAGER.XFF` | 1.650.876 | `fe2252e546e9ad80d8e5b133c90f1735` |
| `KERNEL.XFF` | 415.908 | `607aa94adfc1fc1023c2c7bff516a8c0` |
| `IOPRP300.IMG` | 275.345 | — |
| `GAMECORE.XFF;1.cmd` | 77.395 | — |
| `MANAGER.XFF;1.cmd` | 14.366 | — |
| `KERNEL.XFF;1.cmd` | 127 | — |

- Os `.cmd` são renomeados com sufixo `;1` porque o loader do guest espera nomes no formato ISO9660.
- O binário do port no device é `ps2sotc_aarch64_pie_sdl` (~51,3 MB) e backups `.bak_3dfc1c8d`, `.bak_5cf37e55`, `.bak_d9a17982`.
- Os md5 do device são idênticos aos do host (`build/watch/`) — não há mismatch de versão de assets.

## 5. Como o port é construído
- Host de build: Windows + WSL (Ubuntu); sysroot aarch64 em `D:\celosia_sysroot`; build em `D:\celosia_upstream\build\linux-aarch64`.
- Gerador: `D:\tools\dev\ninja\ninja.exe -C D:/celosia_upstream/build/linux-aarch64 -j4 ps2EntryRunner` (LTO desligado no port).
- Saída: ELF64 aarch64 PIE, ligado contra SDL2/GLES do sysroot.
- O binário **exige argv[1]** (o ELF do jogo): `./ps2sotc_aarch64_pie_sdl SCUS_974.72`.

## 6. Como testar no device
- Acesso por SSH (credenciais fora do repositório; nunca commite senhas).
- Use sempre script atômico via `ssh 'bash -s' < arquivo` — nunca cole comandos interativos soltos.
- Antes/depois de cada run: parar o EmulationStation para liberar o DRM e **restaurá-lo ao final** (é o frontend do device); verificar que voltou.
- Escrever somente em `/roms/ports/SotC_Celosia/` e `/tmp`.
- Script de referência: [tools/r36s_smoke.sh](https://github.com/amarqs182/celosia/blob/r36s/tools/r36s_smoke.sh) (existe na `r36s`).

## 7. Variáveis de ambiente do runtime
- `SOTC_NATIVE`
- `PS2X_NATIVE_IOP_BOOT`
- `PS2X_NATIVE_IOP`
- `PS2X_GS_NODRAW`
- `PS2X_NO_SPU2_AUDIO`
- `PS2X_STATS`
- `PS2X_TRACE_PAD`
- `PS2X_INPUT_CLOCK=frames`
- `PS2X_INPUT_SCRIPT`
- `PS2X_TRACE_SYSCALLS`
- `PS2X_TRACE_SCHED`
- `PS2X_TRACE_SEMA_PC`
- `SOTC_GAMECORE_ENTRY`
- `PS2X_CD_ASYNC`
- `PS2X_CD_LOG`
- `PS2X_DUMP_RAM_AT_SECONDS` / `PS2X_DUMP_RAM_AT_SECONDS_PATH`

Nota factual: `PS2X_GS_NODRAW` só suprime raster/VU1 quando `native_renderer::shown()` é verdadeiro (`gs_frontend.cpp`, `skipRaster()`), e o upload só é suprimido quando `skipRaster()` é true **e** `m_trxdir != 1`; no estado atual (nada é exibido) ele não tem efeito.

## 8. Estado atual do port (o que já funciona)
- O ELF do jogo é carregado e o runtime executa o guest; o loader (`IosCdvdManagerSimulation`) resolve `MANAGER.XFF` e `GAMECORE.XFF` via `sceCdSearchFile` (2 chamadas OK).
- Sistema de threads/semáforos do EE está funcional o suficiente para não travar: 16 semáforos mapeados.
- O renderer recebe 128 uploads de imagem e o anel de frames é de 8 (`native_renderer_gl.cpp`).

## 9. Bloqueio atual (o que falta)
- Após o `jalr` do loader (`source=0x1b5bcc → target=0x1344788`) o guest não produz nenhum evento novo: sem `vfs:open`, sem `printf`, sem `ios:*`, sem `guest-branch`.
- Sem `sceGsResetGraph` do guest, `PMODE/SMODE2/DISPFB/DISPLAY` nunca são programados → `GS::Present` não recebe pedido válido → nenhuma imagem chega à tela (`shownFps≈0`, VU1 parado).
- `sceCdRead=0` no run inteiro; só 2 `sceCdSearchFile` OK.
- `0x1344788` cai no meio de `gamecore_sub_01344708` (é um `jal`), enquanto o init aparente é `0x13447C8` (`initgc/initgcmain/initgcmaingp/initgcchartest`). O entry vem de `lw 0x1C($s1) → lw 0x10($v0) → jalr`.
- Investigação aberta: por que o guest não executa trabalho depois do `jalr` (estrutura de módulo lida pelo loader vs. `.cmd`).

## 10. Achados já confirmados (evita retrabalho)
| Achado | Veredito |
| --- | --- |
| `gid=14` → `sceMc2Init` (worker do memory card); `gid=3` → `PreparePowerOff` | **workers ociosos, não deadlock** |
| `GAMECORE.XFF` **não é comprimido** (nenhum fluxo zlib/gzip/xz/lz4/zstd válido) | `.text` em offset `0x6968`, tamanho `0x12b580`, base de carga `0x40000000`; `0x1339e68` é o espaço de vínculo do `.cmd` |
| O `.text` do XFF é **relocável** (6 tabelas, ~39k entradas, `R_MIPS_26`) | os `jal` não são armazenados como endereço absoluto — procurar os encodings do binário recompilado dentro do XFF não prova nada |
| Hipótese de "delta de base 0x40" entre `.text` do XFF e recompilação | **refutada** |
| `PS2X_GS_NODRAW=1` | não aliviou nenhum stall (ver item 7) |
| Experimentos de redirect do entry (`SOTC_GAMECORE_ENTRY`, opt-in): E1 `→0x13447c8`, E2 `→0x1344708` | estado final idêntico ao controle |

## 11. Regras do projeto
### Device (obrigatório)
- Nunca fazer flash, nunca instalar system-wide, nunca deletar arquivos.
- Nunca `ls /proc/[0-9]*/fd/*` (travou o aparelho) e nunca `ffmpeg -f fbdev`/`-f kmsgrab`.
- `pkill`/`kill` só dentro de script atômico, nunca solto.
- Restaurar o EmulationStation após cada teste e verificar.
- Testes no aparelho são feitos pelos mantenedores; contribuições devem ser validadas em build nativo/host.

### Git
- Contribuições: branch `feat/*`, `fix/*`, `refactor/*`, `perf/*`, `docs/*`, `hotfix/*` + **PR contra a `main`**.
- Commits no padrão Conventional Commits: `tipo(escopo): descrição`.
- Proibido `git add -A` / `git add .`; staging explícito por arquivo. Nunca commitar `build/`, logs, binários gerados ou `test_write.txt`.
- Proibido `git push --force` e reescrita de histórico compartilhado.
- Nada de segredos no repositório: sem senhas do device, tokens ou chaves.
- Cada PR deve descrever: problema, solução, como foi testado, limitações.

### Código
- AArch64 apenas; sem emulação de CPU x86; sem dependência nova sem justificativa.
- Preserve as convenções existentes; não misture refatoração com correção.
- Não declare teste que não foi executado.

## 12. Como contribuir (passo a passo)
1. Fork/clone e `git switch main` + `git pull --ff-only`.
2. Crie `feat/<assunto>` ou `fix/<assunto>`.
3. Implemente, rode build e testes aplicáveis.
4. Abra PR contra `main` com o template do item 11.
5. Não altere arquivos fora do escopo do PR.

## 13. Roadmap / próximos passos
1. Descobrir por que o guest não executa após o `jalr` do loader (dump da estrutura de módulo lida pelo loader).
2. Fazer o guest chamar `sceGsResetGraph`/`scePadInit` e validar o caminho de apresentação (`GS::Present`).
3. Investigar `sceCdRead` (nunca chamado) e o fluxo de leitura de dados do jogo.
4. Portar para a `r36s` as correções de runtime do upstream que sejam portáveis (ex.: correções de threading e diagnósticos).

## 14. Evidências e artefatos
- Os logs abaixo são artefatos do host: o diretório `build/` não é versionado (não entra no repositório); servem apenas como referência local.
- Logs de referência no host (`build/`): `fix2_game.log`, `sem_game.log`, `disc_D0.log`, `entry_E1.log`, `entry_E2.log`, `nod_F1.log`, `nod_F2.log`, `nod_F3.log`.
- Documentação técnica detalhada: [docs/STATUS.md](https://github.com/amarqs182/celosia/blob/r36s/docs/STATUS.md) e [docs/r36s_port.md](https://github.com/amarqs182/celosia/blob/r36s/docs/r36s_port.md) (na branch `r36s`).
- Ferramentas: [tools/r36s_smoke.sh](https://github.com/amarqs182/celosia/blob/r36s/tools/r36s_smoke.sh) (smoke test no device), `tools/xff.py` (leitura de container XFF).
