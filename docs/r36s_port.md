R36S Port — SotC Celosia (docs/r36s_port.md)

> Consolidação do port PS2 → R36S (Crouton Alpha). Números medidos nesta sessão; não inventar.
> Repositório host: D:\celosia_upstream. Device: R36S Debian 13 aarch64 192.168.2.121 ark/ark.
> Estado: boot OK, stall em tela de loading (deadlock semáforos + anel pad zero + VU1 zero + GS não exibido).
> Documento gerado via WSL python3 com LF. Não commitar, não buildar, não tocar no device neste passo.

---

## (a) Objetivo e constraints

- Portar Shadow of the Colossus (SotC) PS2 para R36S nativo usando base upstream r36s.
- Alvo aarch64 ELF64, sem emulação x86. Rodar como `SCUS_974.72` via EmulationStation.
- Stack de display: SDL2 + KMSDRM + GLES. Sem EGL / Mesa / GBM / DRM extras.
- Base: upstream `origin/main` + diff local `origin/main..HEAD` (20 arq / +2.418) ainda a auditar.
- Não deletar arquivos do jogo nem do sistema; não flash; não instalar system-wide.
- Escrever só em `/roms/ports/SotC_Celosia/` e `/tmp` (tmpfs 448M) — ocupações medidas.
- Restaurar EmulationStation após cada teste; não dar push para upstream.
- Medir por dump de RAM/GS/VRAM + logs. Sem adivinhar: confirmar via binário/log.
- Dois riscos mantêm: loop do kernel destrava semáforo errado; anel de pad nunca escrito.
- GS desenha conteúdo real que não chega à tela; VU1 nunca executa; tri sempre 0.
- Commits já existentes (não refazer): a02ac0d, 5380723, 11a9b6d, dd53b12.
- Fix já aplicado: enableHostClockSync incondicional (sotc_overrides.cpp:941) → gameFps=60 idle 35k/s → ~445/s.
- Fix já aplicado: hostFps real (ps2_runtime.cpp:3450-3452, SDL_GL_SwapWindow) ≈57-60.
- Fix já aplicado: PS2X_GS_NODRAW=0 obrigatório com SOTC_NATIVE=1; CMakeLists inclui game/overrides em builds não-Vita.
- Fix já aplicado: __APPLE__ guard no dyld.h (decomp_lines67.cpp).
- Critério de sucesso: [sotc] GAMECORE entered + tri>0 + anel pad !=0 + imagens no display (displayFbp).
- Método científico: cada hipótese H1-H4 com teste isolado e evidência literal (log/dump).

## (b) Ambiente

- Device R36S: Debian 13 aarch64, 192.168.2.121, ark/ark. WSL Ubuntu + sshpass no host.
- ES restaurado após cada run. /tmp é tmpfs 448M; atenção com dumps grandes de RAM/GS.
- Dir do jogo: /roms/ports/SotC_Celosia/ ; binário rodado como SCUS_974.72 (cópia do NICO.ELF).
- Binário: ps2sotc_aarch64_pie_sdl 51.350.952 B, md5 d9a17982dc91bcefb1beb895c27521b6.
- Build host: D:\tools\dev\ninja\ninja.exe -C D:/celosia_upstream/build/linux-aarch64 -j4 ps2EntryRunner.
- Build flags: LTO off (default upstream); sysroot D:\celosia_sysroot.
- CMakeLists inclui game/overrides em builds não-Vita (já aplicado).
- NICO.DAT 2.667.340.678 B (NÃO é ISO9660, sem CD001) — payload principal do jogo.
- NICO.DAT não é ISO9660: ausência de CD001 confirma arquivo bruto, não imagem ISO.
- NICO.ELF 242.940 B.
- GAMECORE.XFF 2.141.864 B.
- MANAGER.XFF 1.650.876 B.
- KERNEL.XFF 415.908 B.
- STARTUP.XFF 26 B.
- IOPRP300.IMG 275.345 B.
- MODULES/ contém: SIO2MAN, DBCMAN, SIO2D, DS1O_D, USBD, PL2303, LIBSD, MC2_D, SG2IOPM1.
- MODULES2/ contém: MCMAN, MCSERV (nunca carregados em nenhuma run medida).
- Linker script GAMECORE.XFF;1.cmd 77.395 B (mapa de símbolo).
- Linker script MANAGER.XFF;1.cmd 14.366 B (mapa de símbolo).
- Linker script KERNEL.XFF;1.cmd 127 B (mapa de símbolo).
- Símbolos-chave: FlushPadHistory=0x114b0a8, padSysGet=0x104e78 (vg. GAMECORE.XFF;1.cmd).
- memcards/mc0/mc1 vazios. Runtime cria Mcd001.ps2 em branco (0xFF) válido (MemoryCard2.cpp:116-123).
- CheckNewCard não bloqueia; tools/native_run.sh exige memcards/Mcd001.ps2.
- Host: D:\celosia_upstream\build\ guarda logs/dumps; ver seção (g) para lista completa.
- WSL Ubuntu no host com sshpass; acesso via ssh `bash -s` < arquivo (ver seção h).
- sysroot D:\celosia_sysroot reúne headers/libs aarch64 para cross-build.

## (c) Como rodar e medir

- Princípio: script atômico no device (parar ES, rodar com timeout, coletar log/dumps, restaurar ES).
- Variáveis que definem modo e instrumentação (sempre passar via arquivo + ssh 'bash -s' < arquivo):
- SOTC_NATIVE=0|1|2 : 0=emulado, 1=nativo IOP emulado? (ver H1), 2=nativo; matriz: A=1, B=2, C=0.
- PS2X_NATIVE_IOP_BOOT=0|1 : controla boot nativo no iop_subsystem.cpp:22-28 (fallback em SOTC_NATIVE).
- PS2X_NATIVE_IOP=0|1 : só transição em runtime (iop_subsystem.cpp:30-36 nativeIopAllowed).
- PS2X_GS_NODRAW=0 é obrigatório com SOTC_NATIVE=1 (senão GS não desenha nada).
- PS2X_NO_SPU2_AUDIO=1 desliga áudio SPU2.
- PS2X_STATS=1 imprime stats de frame/primitivas/GS.
- PS2X_TRACE_PAD=1 loga [pad2] input / botões.
- PS2X_INPUT_CLOCK=frames sincroniza script de input com frames.
- PS2X_INPUT_SCRIPT=<arquivo> replay de input (usado em runs com START/CROSS).
- PS2X_TRACE_SYSCALLS=1 instrumenta ps2_runtime.cpp:2104 (linhas [syscall] até [ld:enter]).
- PS2X_TRACE_SCHED=1 mostra threads id/pc/waitId e semáforos gid/waiters/signals.
- PS2X_DUMP_RAM_AT_SECONDS + PS2X_DUMP_RAM_PATH (matriz A/B/C: 70s, dump em t=35s).
- PS2X_DUMP_GSVRAM_PATH + PS2X_DUMP_GS_BUFFERS_AT (PPMs 640×512 + bbox; fb0 zero).
- PS2X_SNAPSHOT_DIR só gera index.txt no Linux (ExportImage stub raylib_linux.cpp:894).
- Exemplo de análise do anel de pad a partir do *_ram.bin (offset físico = VA, little-endian):
-   python3 -c "import struct; b=open('mx_A_ram.bin','rb').read(); print(hex(struct.unpack_from('<I',b,0x0118D638)[0])); print(hex(struct.unpack_from('<I',b,0x0118D63C)[0]))"
-   Blocos: 0x0118D680 + pad*0xF98 (pad=0,1). Head 0x0118D638, count 0x0118D63C. Analógico 00 00 00 00.
-   Esperado quando input funciona: head/count >0 e botões/analógico ≠ 00; medido sempre 00000000 (30 amostras).
- Trace de syscalls: contar `grep -c '\[syscall\]' tr_T1.log` e comparar com tamanho do log (1.065.761 B / 35.028 linhas).
- Scheduler: cruzar `grep '\[ee:sema\]' mx_A.log` (só 1 signal) com `grep '\[sched\]'` e waitId por thread.
- Sempre conferir md5 e tamanho do binário antes de empurrar para o device.
- Rodar 70s com dump em t=35s na matriz A/B/C para capturar estado estável antes do stall.
- Script de input permite testar START (0xfff7) e CROSS (0xbfff) sem tocar na tela.
- Se `grep -c` de syscall for >33 ou aparecer depois de [ld:enter], H3 precisa ser reaberta.
- Interpretar PPMs: contar px não-pretos; bbox x=8..639 y=192..447 indica conteúdo real.
- Conferir gsWait space em run sem input (A3: 417.1) como sinal de backpressure.

## (d) Hipóteses H1–H4 (tabela teste → resultado)

| Hipótese | Teste | Resultado |
| --- | --- | --- |
| H1 IOP nativo dropa pad | Matriz A/B/C (70s, dump t=35s): A SOTC_NATIVE=1 PS2X_NATIVE_IOP_BOOT=0 PS2X_NATIVE_IOP=0 (emulado, dropped=0), B SOTC_NATIVE=2 (nativo, dropped=2), C SOTC_NATIVE=0 (emulado) | TODAS zeradas: bloco 0x118d680 (2×0xF98) 100% zero; head 0x118d638=0, count 0x118d63c=0; 30 amostras 0x00000000; analógico 00 00 00 00; sceCdRead=0; mscal=0; vu1Instr=0; tri=0; printf para em ld output linker script GAMECORE.XFF;1.cmd |
| H1 detalhe | PS2X_NATIVE_IOP=0 sozinho | NÃO evita boot nativo: iop_subsystem.cpp:22-28 nativeIopBoot usa PS2X_NATIVE_IOP_BOOT com fallback em SOTC_NATIVE; :30-36 nativeIopAllowed usa PS2X_NATIVE_IOP só p/ transição runtime |
| H2 faltam .cmd | Verificar existência + relançar runs posteriores | .cmd existem (GAMECORE 77.395 / MANAGER 14.366 / KERNEL 127) e runs continuam travadas → H2 refutada |
| H3 syscalls 0x0d/0x0e | Trace PS2X_TRACE_SYSCALLS=1 (ps2_runtime.cpp:2104) | Só 33 linhas [syscall] no log inteiro (1.065.761 B / 35.028 linhas), todas antes de [ld:enter] (linhas 27-208). IDs: 0x74 (10x SetSyscall), 0xe (9), 0x5b (6), 0xd (3), 0x83 (2), 0x3c/0x3d/0x5a. Nenhuma syscall durante o stall |
| H3 enumeração kernel | PC/RA/a0 de 0xd e 0xe | v1=0xd: PC=0x106c98 RA=0x10f330 a0=1,2,3; v1=0xe: PC=0x106ca8 RA=0x10f348 a0=4,5,6,7,9,a,b,c,d = enumeração da tabela de syscalls do kernel no boot |
| H3 CD morto | Dispatcher.cpp sem case 0x07-0x0f; grep dos 37 sceCd* de CD.cpp | 0 call sites → código morto. Não é o bloqueio atual |
| H3 IOP nativo upstream | tools/native_run.sh:24 → SOTC_NATIVE=2 e lá o jogo funciona | IOP também é nativo no upstream; diferença não está no modo IOP em si |
| H4 input não chega | [pad2] input, padSysGet, run A3 sem input | Input chega ao libpad ([pad2] input buttons=0xfff7 START, 0xbfff CROSS) mas anel guest nunca escrito (padSysGet chamada 4.694×). Run A3 sem input: stall pior (t=60→151, 91s a 2.3 fps, gsWait space 417.1) |
| H4 primitivas | Contar ln/sprite sem input | Explosão não depende de input: A ln 1952→16160 sprite 123→1010; A3 38720/2419. Tela de loading cheia de linhas/sprites |

- H1 detalhe (boot nativo): iop_subsystem.cpp:22-28 nativeIopBoot usa PS2X_NATIVE_IOP_BOOT com fallback em SOTC_NATIVE.
- H1 detalhe (runtime): iop_subsystem.cpp:30-36 nativeIopAllowed usa PS2X_NATIVE_IOP só para transição em runtime.
- Logo PS2X_NATIVE_IOP=0 sozinho NÃO evita boot nativo. Era preciso PS2X_NATIVE_IOP_BOOT=0 + SOTC_NATIVE=1.
- Matriz A/B/C medida em 70s com dump RAM em t=35s; dropped=0 em A (emulado), dropped=2 em B (nativo).
- Em TODAS (A/B/C) o bloco 0x118d680 permanece 100% zero (2 blocos de 0xF98).
- Head 0x118d638 = 0, count 0x118d63c = 0, 30 amostras 0x00000000, analógico 00 00 00 00.
- sceCdRead = 0, mscal = 0, vu1Instr = 0, tri = 0 em todas as amostras da matriz.
- printf do guest para em `ld output linker script GAMECORE.XFF;1.cmd` (sem GAMECORE entered).
- H2 refutada: os .cmd existem e runs posteriores continuam travadas no mesmo ponto.
- H3 syscalls: só 33 linhas [syscall] no log inteiro (1.065.761 B / 35.028 linhas), linhas 27-208 antes de [ld:enter].
- IDs observados: 0x74 (10, SetSyscall), 0xe (9), 0x5b (6), 0xd (3), 0x83 (2), 0x3c/0x3d/0x5a. Nenhuma syscall durante o stall.
- v1=0xd (PC=0x106c98, RA=0x10f330, a0=1,2,3) e v1=0xe (PC=0x106ca8, RA=0x10f348, a0=4,5,6,7,9,a,b,c,d).
- Interpretação: enumeração da tabela de syscalls do kernel no boot. Dispatcher.cpp sem case 0x07-0x0f.
- Os 37 sceCd* de CD.cpp sem nenhum call site (grep = 0) → código morto no caminho atual.
- O IOP também é nativo no upstream (tools/native_run.sh:24 → SOTC_NATIVE=2) e lá o jogo funciona.
- H4: input chega ao libpad ([pad2] input buttons=0xfff7 START, 0xbfff CROSS) mas anel guest nunca escrito.
- padSysGet chamada 4.694× com anel zero — polling acontece, fila nunca enche.
- Run A3 sem input: stall pior (t=60→151, 91s a 2.3 fps, gsWait space 417.1).
- Explosão de primitivas não depende de input (A: ln 1952→16160 sprite 123→1010; A3: 38720/2419).
- Veredito: H1 refutada como causa única (emulado também zera). H2 refutada. H3 é enumeração normal. H4 é sintoma real.
- Próximo teste discriminante: forçar signal de gid=9/14 e observar se GAMECORE entered aparece.

## (e) Estado confirmado

- Boot chain OK: sceCdSearchFile \MANAGER.XFF;1 → lsn 0x100000 size 0x1930bc (0x327 setores).
- Boot chain OK: sceCdSearchFile \GAMECORE.XFF;1 → lsn 0x100328 size 0x20aea8 (0x416 setores).
- vfs:open OK (cdrom0: = cdRoot = elfDirectory, ps2_runtime.cpp:1452-1454; FileIO.cpp:77,160).
- ld output linker script .cdrom0:\GAMECORE.XFF;1.cmd. (mx_A.log:974) + vfs:open fd=12 (976).
- JALR 0x1b5bcc → 0x1344788 (mx_A.log:980) — trampolim do linker entrou.
- DEADLOCK DE SEMÁFOROS (achado central): 16 semáforos gid 1..16; gid=14/15 init count 0, max 127.
- 1 signal no log inteiro: [ee:sema] signal gid=6 cnt=0 waiters=1 (mx_A.log:10509).
- Loader id=6 (entry 0x1b34a0) espera gid=7 / gid=9, nunca acordado.
- Thread id=1 (entry 0x100010, pc=0x103ab0) dorme para sempre.
- Thread id=2 (entry 0x104ff0, pc=0x105018, waitId=3) dorme para sempre.
- Thread id=7 (entry 0x11b458, pc=0x11b488, espera gid=14 cnt=0) dorme para sempre.
- Kernel id=5 RUNNING pc=0x1b11a8. [sotc] GAMECORE entered (sotc_overrides.cpp:67-68) nunca aparece.
- VU1 nunca executa: mscal=0, vu1Instr=0, xgkick=0 em todas as amostras; 14 em A2, 35 em A3.
- Tri 0 sempre; só linhas (~1900/frame) e sprites (~120/frame) = tela de loading.
- GS desenha conteúdo real que não chega à tela: PPMs 640×512 → 20.640 / 81.965 / 20.570 px não-pretos.
- BBox GS: x=8..639 y=192..447 (e x=3..639 y=196..511) — conteúdo fora do centro, não é só fundo.
- fb0 100% preto (1.228.800 bytes zero). ExportImage stub no Linux (raylib_linux.cpp:894).
- PS2X_SNAPSHOT_DIR só gera index.txt no Linux por causa do stub acima.
- memcards/mc0/mc1 vazios; runtime cria Mcd001.ps2 em branco (0xFF) válido (MemoryCard2.cpp:116-123).
- CheckNewCard não bloqueia; MCMAN/MCSERV (MODULES2/) nunca carregados.
- NICO.DAT não é ISO9660 (sem CD001) — confirma leitura via cdrom0: / vfs, não via ISO.
- Formato PPMS indica framebuffer intermediário válido, mas displayFbp=0 impede tela final.
- Todas as evidências apontam stall antes do primeiro tri meshed, não depois.

## (f) Próximas etapas (ordem de prioridade)

1. Diff upstream `origin/main..HEAD` completo (20 arq / +2.418) — separar HACKs locais do núcleo upstream.
   - Filtrar: nativo IOP, linker .cmd, overrides sotc_overrides.cpp, clock sync, CMakeLists não-Vita.
   - Revisar iop_subsystem.cpp, Dispatcher.cpp, CD.cpp, FileIO.cpp e raylib_linux.cpp em primeiro lugar.
2. Deadlock gid 9 / gid 14: rastrear quem deveria dar signal em gid=7/9 e gid=14.
   - Comparar scheduler/semáforo com upstream que funciona; checar contagem init 0 vs 127.
   - Logar wakeup por gid com PS2X_TRACE_SCHED=1 em run curta de 35s.
3. CD/IOP: investigar ausência de CDROM.IRX / FILEIO.IRX e fioRead do .cmd no boot nativo.
   - Confirmar se lsn 0x100000/0x100328 são lidos e se fd=12 chega a ser lido de fato.
   - Checar IOPRP300.IMG 275.345 B — se contém os IRXs faltantes.
4. Anel de pad: por que libpad recebe (0xfff7 / 0xbfff) mas nunca escreve 0x0118D680.
   - Checar FlushPadHistory / padSysGet 0x104e78 e fila pad*0xF98; validar ordem de threads.
5. VU1: por que mscal/vu1Instr/xgkick = 0; verificar gate no bootstrap (VU1 kick antes do GAMECORE entered).
   - Checar interrupções VU1 e dependência de semáforo; comparar com tri=0.
   - Testar run com primitiva isolada de VU1 se houver harness.
6. GS → GLES: displayFbp=0; PPMs têm pixels reais mas fb0 zero e ExportImage stub.
   - Ligar swap chain KMSDRM/GLES e snapshots; decidir se PPMs vieram de VRAM ou de buffer intermediário.
7. Mcd001.ps2 com save válido: validar formato no device e confirmar se CheckNewCard/MCMAN afetam progressão.
   - Criar imagem em branco 0xFF válida e repetir run curta para isolar efeito de card vazio.

## (g) Artefatos no host

- Dir: D:\celosia_upstream\build\ (logs binários e dumps copiados após cada run).
- Logs: w50 / w51 / w52 / route / dump / iop / mx_A / mx_B / mx_C / a2_A2 / a2_A3 / tr_T1.log.
- mx_A.log 1.065.761 B / 35.028 linhas: 33 [syscall] linhas 27-208; ld output em 974, fd=12 em 976.
- mx_A.log: JALR em 980, signal gid=6 em 10509 (único signal em 16 semáforos).
- RAM: mx_A_ram.bin / mx_B_ram.bin / mx_C_ram.bin / *_ram.bin (dump em t=35s; offsets 0x0118D638/0x0118D63C/0x0118D680).
- GS: a2_gsbuf/*.ppm / *.png (640×512, 20.640/81.965/20.570 px não-pretos; bbox acima).
- Snapshots: PS2X_SNAPSHOT_DIR só index.txt (stub Linux) — não usar como prova de frame.
- Não commitado: não empurrar artefatos para upstream; só trazer de volta para o host para análise.
- Commits já existentes no repo: a02ac0d, 5380723, 11a9b6d, dd53b12 (não refazer).
- Manter nomes originais ao copiar do device (/tmp) para facilitar `grep` e diff.

## (h) Armadilhas do device (proibido)

- PROIBIDO ls /proc/[0-9]*/fd/* (congela o device / esgota fd). Nunca listar fd de processos alheios.
- PROIBIDO ffmpeg -f fbdev / -f kmsgrab sem teste isolado (rouba KMS e trava output).
- PROIBIDO pkill fora do script atômico (deixa ES morto e tela preta). Sempre via script com restore.
- Usar sempre arquivo + ssh 'bash -s' < arquivo (nunca comando longo inline com aspas escapadas no PowerShell).
- Script atômico: parar ES, rodar binário com timeout, capturar log/dumps, restaurar ES, retornar código.
- /tmp tmpfs 448M: limpar dumps antigos antes de nova run; não encher /tmp com PPMs grandes.
- Não escrever fora de /roms/ports/SotC_Celosia/ e /tmp; não alterar system-wide; não flash.
- Se travar no meio, restaurar ES via mesmo script atômico antes de qualquer nova tentativa.
- Não commitar, não push, não apagar nada do repo; só criar este docs/r36s_port.md.
- Em erro 402/sem crédito ou 429 de limite (50/dia free): trocar de modelo gratuito ou aguardar instrução (ver política).
- Preferir `wsl python3` para gerar/ler artefatos com LF; evitar CRLF de editores Windows.
- Validar sempre com `wc -l` e `grep -n '^#'` após gerar — as 8 seções (a-h) devem aparecer.
- Fim do documento. Proxima sessao: executar etapa 1 (diff upstream) antes de qualquer novo build.
- Revisao: este arquivo e a unica mudanca permitida nesta tarefa; nenhum outro arquivo deve ser editado.
- Conferência: wc -l deve estar entre 200 e 300; grep deve mostrar as 8 secoes (a)-(h).
- Arquivos novos permitidos nesta tarefa: so docs/r36s_port.md.
- Builds proibidos nesta tarefa: nao rodar ninja; nao tocar no device.
- Flush: garantir LF final e aguardar validacao do orquestrador.
