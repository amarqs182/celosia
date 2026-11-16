# Worklist — R36S Nativo (`amarqs182/celosia`)

Status: **todo**, **doing**, **done**, **parked**.
Alvo exclusivo: **R36S** (Rockchip RK3326, 4× Cortex-A35 aarch64, Mali-G31 MP2 via ARM `libMali.so` OpenGL ES 3.2, SDL2 KMSDRM, Linux `4.4.189`, 895 MiB RAM).
Auditoria técnica completa em [`r36s_native_audit.md`](r36s_native_audit.md) e notas de hardware/port em [`r36s_port.md`](r36s_port.md).

> **Diretriz:** O PS2Recomp e a herança do Celosia (PS Vita / macOS) foram o ponto de partida para rodar o jogo. O foco exclusivo passa a ser tratar o R36S como um sistema ARM64/Mali-G31 nativo, removendo verificações genéricas em execução e substituindo subsistemas presos à arquitetura do PS2.

## Baseline atual medido no R36S (`main` @ `000597c`)

- **Gameplay pesado (`docs/r36s_port.md`, Phase 6B, `draws 1148..2346`):** **`31,60 FPS`** med | `game` **`31,60 ms`** | `render` **`29,70 ms`** (paralelo, `wait 0,0 ms`) | `present` **`14,30 ms`**.
- **Cenas 3D pesadas (`bench_latest.log`, `draws >= 1000`, `n=20`):** **`32,88 FPS`** avg (`30,89 ms`), `game 30,86 ms`, `render 27,75 ms`, `present 14,07 ms`, `wait 0,01 ms`.
- **Meta para 60 FPS (`16,67 ms`):** reduzir **~47% da `game` thread** e **~44% da `render` thread** em paralelo.
- **Distribuição da `game` thread (`gp_b2_game.prof`, `1,428` funções, `99,68%` busy):** `hand_native` **`66,38%`** | `recompiled_MIPS` **`31,65%`** | `lifted_D1_D5` **`1,97%`**. Top 10 = `17,10%`, Top 50 = `43,11%`.

---

## Fase A — Eliminar Desperdício Comprovado (5/5 concluídos)

| ID | Item | Status | Onde / Observação | Validação |
|---|---|---|---|---|
| **A-1** | Desabilitar `PS2X_GUEST_READ_WATCH=0` e `PS2X_GUEST_WRITE_TRACE=0` no build release R36S | **done** | [`PS2Recomp/ps2xRuntime/CMakeLists.txt`](../PS2Recomp/ps2xRuntime/CMakeLists.txt#L16-L18) (`PS2X_ENABLE_GUEST_READ_WATCH` / `PS2X_ENABLE_GUEST_WRITE_TRACE`, desabilitados automaticamente quando `PS2X_LINUX_RELEASE=ON`) | Binário stripped encolheu `78,04 → 64,28 MB` (`-13,76 MB`); com PGO antigo (`pgo_merged`), `-Wcoverage-mismatch` descartou perfis nas funções cujo CFG mudou (`leaderNative` `141→91` arcos), mantendo `30,20` vs `30,38 FPS` medido |
| **A-2** | Desabilitar explicitamente `PS2X_ENABLE_RUNTIME_LOGS=OFF`, `PS2X_ENABLE_AGRESSIVE_LOGS=OFF` e `PS2X_ENABLE_IOP_RPC_TRACE=OFF` no Release R36S | **done** | [`PS2Recomp/ps2xRuntime/CMakeLists.txt`](../PS2Recomp/ps2xRuntime/CMakeLists.txt#L506-L550) (`PS2X_LINUX_RELEASE=ON` garante todas desligadas junto com `READ_WATCH=0` / `WRITE_TRACE=0`) | `A2` (`A1` + `RUNTIME_LOGS=OFF`): `30,24 / 30,96 FPS` (`p99` `44,40 → 43,81 ms`, `max` `130,4 → 62,6 ms`, `RSS` `-2,6 MiB`) com `pgo_merged` desatualizado |
| **A-3** | Comparar build original vs. build limpo com PGO casado no R36S | **done** | Corpus `pgo_phase_a` gerado no R36S (`SOTC_GCOV_DUMP_FRAME=5200`, `214` `.gcda` com `0` divergências `-Wcoverage-mismatch`); [`SOTC_OPTIMIZATION_LOG.md`](../SOTC_OPTIMIZATION_LOG.md) | **Rota casada `4500..5900` (`n=1401`):** `A0` `30,38 mean / 31,06 med FPS` (`game 32,20 ms`) → **`A3_PGO` `32,34 mean / 33,00 med FPS`** (**`+1,96 FPS` / `+6,45%`**, `game 30,30 ms` `-6,08%`, `p99 44,40→40,80 ms`, binário `63,43 MB` `-14,61 MB`) |
| **A-4** | Padronizar `-mtune=cortex-a35` e opções `+crypto+crc` no CMake ARM64 | **done** | [`PS2Recomp/CMakeLists.txt`](../PS2Recomp/CMakeLists.txt#L24-L26) (`PS2X_ARM64_MTUNE="cortex-a35"`; `PS2X_ARM64_ENABLE_CRYPTO=ON` + `PS2X_ARM64_ENABLE_CRC=ON` evitam sobrescrita por `-march=armv8-a+fp+simd`) | `-mtune=cortex-a35` já vinha via `CMAKE_CXX_FLAGS` em `A0–A2`; ativar `+crypto+crc` sem código novo usando intrínsecos gerou MD5 idêntico a `A2` (`330a444ed7002925d88c54fafcb1d9ab`) |
| **A-5** | Validar integridade visual e desempenho com gameplay real após Fase A | **done** | Capturas `SOTC_GL_SHOTS=300` (`shot_2400`, `shot_3000`, `shot_3600`, `shot_3900`) + implantação em `/roms/ports/.SotC_Celosia/ps2sotc_aarch64_pie_sdl` (`f8ee05adb7dfdc287b3f13df0c835f98`) | 0 artefatos visuais no menu, interior do Shrine (light rays aditivos), cutscene e terreno externo; pronto para **Fase B** (`cpu0` segue em `97,9%`) |

## Fase B — Reorganizar a Execução Nativa (0/6 concluídos)

| ID | Item | Status | Onde / Observação | Validação |
|---|---|---|---|---|
| **B-1** | Mapear os principais pontos de *park* e *join* do worker | **todo** | [`sotc_worker.cpp`](../game/overrides/sotc_worker.cpp) (`bench_latest.log` f=4218: **`62%` dos 300 jobs estacionados** em `011697e8`, `381` chamadas exclusivas de `iosFree`, `193` waits; `[mdslane]` `14,52–74,96 ms` na `game` thread) | Identificar todas as dependências de heap/grupos/colisão/motion que bloqueiam o worker |
| **B-2** | Separar subsistemas que podem operar em memória independente | **todo** | `sotc_worker.cpp` / partículas / vento / geometria / consultas | Usar snapshots imutáveis + arenas thread-local por lote em vez de tornar o heap PS2 thread-safe |
| **B-3** | Eliminar round-trips `callGuest` e fronteiras Nativo ↔ MIPS nos caminhos quentes | **todo** | [`ps2_runtime.h`](../PS2Recomp/ps2xRuntime/include/ps2_runtime.h#L739-L770), `recompiled_MIPS` (`31,65%`) + `lifted_D1_D5` (`1,97%`) | Colapsar subárvores completas em chamadas diretas `Subsistema Nativo → Funções Nativas` |
| **B-4** | Modernizar o broadphase de colisão | **todo** | `ClipCollisionDirectCallBack` (`2,19%`), `ClipCollisionFinderCallBack` (`1,97%`), `GetCollisionDBCallBack` (`1,95%`) = **`6,11%`** da `game` thread | Organização espacial própria, cache intra-frame e execução em lote preservando física necessária |
| **B-5** | Reduzir traversals repetidos de geometria e objetos | **todo** | `DispDispObjGroupModel` (`1,75%`), `manager_sub_0111C810` (`1,72%`), `ExecCharaGeometry` (`1,69%`), `GetLayoutClass` (`1,08%`) | Eliminar caminhadas redundantes na árvore PS2 por quadro |
| **B-6** | Medir distribuição real do trabalho nos quatro núcleos Cortex-A35 | **todo** | `[worker91]`, `[mdslane]`, `[prof129]`, estatísticas por núcleo no R36S | Comprovar redução real de tempo da `game` thread rumo a `≤ 16,67 ms` |

## Fase C — Extrair o Máximo da Mali-G31 (0/6 concluídos)

| ID | Item | Status | Onde / Observação | Validação |
|---|---|---|---|---|
| **C-1** | Separar CPU de renderização, tempo de GPU e apresentação nas métricas | **todo** | [`native_renderer_gl.cpp`](../PS2Recomp/ps2xRuntime/src/lib/native/native_renderer_gl.cpp) (`render` med `29,70 ms`, `present` `14,30 ms`) | Isolar quanto dos ~30 ms é CPU (`plan`/`passes`/Driver) vs GPU Mali-G31 |
| **C-2** | Medir utilização das unidades da Mali-G31 | **todo** | Contadores / profiling compatíveis com Bifrost `r13p0` | Identificar gargalo entre vértice, fragmento (aritmética vs textura/memória) |
| **C-3** | Analisar shaders GLES com Mali Offline Compiler | **todo** | Shaders `#version 310 es` em [`native_renderer_gl.cpp`](../PS2Recomp/ps2xRuntime/src/lib/native/native_renderer_gl.cpp#L84-L86) | Medir ocupação de registradores e pressão FP32 vs FP16 por shader |
| **C-4** | Aplicar `mediump`/FP16 onde comprovadamente seguro | **todo** | Cabeçalho `precision highp float/int` global hoje força FP32 sem anotação explícita | Manter `highp` em transformações/posições de mundo; testar `mediump` em iluminação/cores/partículas/fog com comparação visual |
| **C-5** | Reduzir cópias de geometria e buffers | **todo** | `bench_pass7_120s.log` (`[gl:phases]`): `plan 4,01–4,03 ms`, `passes 10,66–10,81 ms`, **`run copies 7.112–13.025 KB/frame`** | Eliminar cópias entre arenas, conversões e recriação de buffers por frame |
| **C-6** | Investigar alpha test e passes redundantes | **todo** | `[shadow]` **`724,7/s`** second passes de alpha test | Avaliar corte direto no fragment shader sem second pass validando pixel a pixel e impacto em *early-Z* na Mali-G31 |

## Fase D — Independência Definitiva do PS2 (0/6 concluídos)

| ID | Item | Status | Onde / Observação | Validação |
|---|---|---|---|---|
| **D-1** | Substituir progressivamente o kernel guest nos caminhos quentes | **todo** | `isysGroupForFirst`/`Next`/`Exit`, `isysWakeupSubThread`, `SignalSema`, `iosSendMsg` (`>3,15%` no Top 20 não-nativo) | Migrar coordenação de subthreads/grupos para primitivas nativas |
| **D-2** | Eliminar estruturas guest desnecessárias | **todo** | Layouts de objetos/listas na RAM 32 MB usados apenas por conveniência PS2 | Substituir por structs nativas alinhadas para cache ARM64 |
| **D-3** | Modernizar gerenciamento de memória e alocação | **todo** | `iosFree` / `m_guestHeap` | Alocadores host dedicados por tempo de vida (frame/cena/entidade) |
| **D-4** | Migrar áudio SPU2 para implementação otimizada em NEON | **todo** | `[audio-ticks]`: `mix voices 142,6 ms + cores 162,8 ms = 305,4 ms / 5 s` (~`6,1%` de 1 core) em [`sound_mixer.cpp`](../PS2Recomp/ps2xIOP/src/native/sound_mixer.cpp#L755-L815) | Vetorizar mixagem/ADPCM/reverb sem perda de fidelidade sonora |
| **D-5** | Remover módulos, shaders e flags exclusivos de Vita/macOS | **todo** | `Metal`, `vitaGL`, Cg, caminhos `#if defined(__vita__)` / `__APPLE__` | Manutenção/limpeza pura (baixo ganho direto em FPS, mas simplifica build e código) |
| **D-6** | Reavaliar a necessidade de cada camada do `PS2Recomp` | **todo** | `PS2_GUEST_MEM_TRAMP` (baixa prioridade salvo evidência em perfil), dispatcher, camadas de compatibilidade | Manter apenas o mínimo necessário onde código do jogo ainda não foi reescrito |

---

## Histórico — Concluído no R36S (Fases 1–6B)

| Etapa | Resultado medido no R36S | Referência |
|---|---|---|
| **Pass 7 (`SOTC_EE_TRUST=1`)** | `game` med `36,00 → 27,00 ms` (-25%), FPS med `26,80 → 35,50` em gameplay | [`SOTC_OPTIMIZATION_LOG.md`](../SOTC_OPTIMIZATION_LOG.md#L20-L37) |
| **Pass 8A–9 (`SOTC_RELEASE=1`, PGO, hot order, Mali-G31 TBDR, `SOTC_FPS_CAP=60`)** | Rota determinística `gp 4458..5298`: `25,60 → 29,40 FPS` med (`game 38,90 → 33,80 ms`), menus `60,0 FPS` | [`SOTC_OPTIMIZATION_LOG.md`](../SOTC_OPTIMIZATION_LOG.md#L64-L153) |
| **Pass 9.5 (`callNative`, `spriteDrawSprite`, `pgo_pass95`)** | `1,196` nativos; `29,60 FPS` med mesmo saindo para `3,041–3,221 draws` externos | [`SOTC_OPTIMIZATION_LOG.md`](../SOTC_OPTIMIZATION_LOG.md#L160-L189) |
| **Phase 6 & 6B (correção visual, `meshoptimizer` indexado, terreno atômico, fix cutscene `decomp_char122`)** | `held terrain 1212 → 0`, fim do freeze na cutscene da águia, `31,60 FPS` med (`29,10–34,30 FPS`) no santuário/pátio | [`SOTC_OPTIMIZATION_LOG.md`](../SOTC_OPTIMIZATION_LOG.md#L195-L228) |
