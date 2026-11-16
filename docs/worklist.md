# Worklist — R36S Nativo (`amarqs182/celosia`)

Status: **todo**, **doing**, **done**, **parked**.
Alvo exclusivo: **R36S** (Rockchip RK3326, 4× Cortex-A35 aarch64, Mali-G31 MP2 via ARM `libMali.so` OpenGL ES 3.2, SDL2 KMSDRM, Linux `4.4.189`, 895 MiB RAM).
Auditoria técnica completa em [`r36s_native_audit.md`](r36s_native_audit.md) e notas de hardware/port em [`r36s_port.md`](r36s_port.md).

> **Diretriz:** O PS2Recomp e a herança do Celosia (PS Vita / macOS) foram o ponto de partida para rodar o jogo. O foco exclusivo passa a ser tratar o R36S como um sistema ARM64/Mali-G31 nativo, removendo verificações genéricas em execução e substituindo subsistemas presos à arquitetura do PS2.

## Baseline atual medido no R36S (A3_PGO_r2, Fase A)

- **Rota determinística de 130 s, janela casada `4500..5900` (`n=1401`):** `32,34 FPS` média / **`33,00 FPS` mediana**; frame time `p50 30,30 ms`, `p90 34,20 ms`, `p99 40,80 ms`; game mediana `30,30 ms` (média `30,91 ms`); render mediana `20,30 ms` (média `25,65 ms`).
- **CPU no trecho amostrado:** `cpu0 97,9%`, `cpu1 29,3%`, `cpu2 36,4%`, `cpu3 38,8%`; RSS média `337,2 MiB`.
- **Meta para 60 FPS (`16,67 ms`):** tomando as medianas da mesma rota, game precisa cair ~45% (`30,30 → 16,67 ms`) e render ~18% (`20,30 → 16,67 ms`). A média de render (`25,65 ms`) mostra cauda adicional a reduzir.
- **Distribuição histórica da `game` thread (`gp_b2_game.prof`, base anterior à Fase A; `1,428` funções, `99,68%` busy):** `hand_native` `66,38%` | `recompiled_MIPS` `31,65%` | `lifted_D1_D5` `1,97%`. Top 10 = `17,10%`, Top 50 = `43,11%`. Essa distribuição não deve ser tratada como perfil atualizado pós-A3_PGO.
- **Referência histórica:** Phase 6B pré-Fase A mediu `31,60 FPS`, game `31,60 ms`, render `29,70 ms`; não misturar essa rota/janela com a baseline A3_PGO_r2.

---

## Fase A — Eliminar Desperdício Comprovado (5/5 concluídos)

| ID | Item | Status | Onde / Observação | Validação |
|---|---|---|---|---|
| **A-1** | Desabilitar `PS2X_GUEST_READ_WATCH=0` e `PS2X_GUEST_WRITE_TRACE=0` no build release R36S | **done** | [`PS2Recomp/ps2xRuntime/CMakeLists.txt`](../PS2Recomp/ps2xRuntime/CMakeLists.txt#L16-L18) (`PS2X_ENABLE_GUEST_READ_WATCH` / `PS2X_ENABLE_GUEST_WRITE_TRACE`, desabilitados automaticamente quando `PS2X_LINUX_RELEASE=ON`) | Binário stripped encolheu `78,04 → 64,28 MB` (`-13,76 MB`); com PGO antigo (`pgo_merged`), `-Wcoverage-mismatch` descartou perfis nas funções cujo CFG mudou (`leaderNative` `141→91` arcos), mantendo `30,20` vs `30,38 FPS` medido |
| **A-2** | Desabilitar explicitamente `PS2X_ENABLE_RUNTIME_LOGS=OFF`, `PS2X_ENABLE_AGRESSIVE_LOGS=OFF` e `PS2X_ENABLE_IOP_RPC_TRACE=OFF` no Release R36S | **done** | [`PS2Recomp/ps2xRuntime/CMakeLists.txt`](../PS2Recomp/ps2xRuntime/CMakeLists.txt#L506-L550) (`PS2X_LINUX_RELEASE=ON` garante todas desligadas junto com `READ_WATCH=0` / `WRITE_TRACE=0`) | `A2` (`A1` + `RUNTIME_LOGS=OFF`): `30,24 / 30,96 FPS` (`p99` `44,40 → 43,81 ms`, `max` `130,4 → 62,6 ms`, `RSS` `-2,6 MiB`) com `pgo_merged` desatualizado |
| **A-3** | Comparar build original vs. build limpo com PGO casado no R36S | **done** | Corpus `pgo_phase_a` gerado no R36S (`SOTC_GCOV_DUMP_FRAME=5200`, `214` `.gcda` com `0` divergências `-Wcoverage-mismatch`); [`SOTC_OPTIMIZATION_LOG.md`](../SOTC_OPTIMIZATION_LOG.md) | **Rota casada `4500..5900` (`n=1401`):** `A0` `30,38 mean / 31,06 med FPS` (`game 32,20 ms`) → **`A3_PGO` `32,34 mean / 33,00 med FPS`** (**`+1,96 FPS` / `+6,45%`**, `game 30,30 ms` `-6,08%`, `p99 44,40→40,80 ms`, binário `63,43 MB` `-14,61 MB`) |
| **A-4** | Padronizar `-mtune=cortex-a35` e opções `+crypto+crc` no CMake ARM64 | **done** | [`PS2Recomp/CMakeLists.txt`](../PS2Recomp/CMakeLists.txt#L24-L26) (`PS2X_ARM64_MTUNE="cortex-a35"`; `PS2X_ARM64_ENABLE_CRYPTO=ON` + `PS2X_ARM64_ENABLE_CRC=ON` evitam sobrescrita por `-march=armv8-a+fp+simd`) | `-mtune=cortex-a35` já vinha via `CMAKE_CXX_FLAGS` em `A0–A2`; ativar `+crypto+crc` sem código novo usando intrínsecos gerou MD5 idêntico a `A2` (`330a444ed7002925d88c54fafcb1d9ab`) |
| **A-5** | Validar integridade visual e desempenho com gameplay real após Fase A | **done** | Capturas `SOTC_GL_SHOTS=300` (`shot_2400`, `shot_3000`, `shot_3600`, `shot_3900`) + implantação em `/roms/ports/.SotC_Celosia/ps2sotc_aarch64_pie_sdl` (`f8ee05adb7dfdc287b3f13df0c835f98`) | 0 artefatos visuais no menu, interior do Shrine (light rays aditivos), cutscene e terreno externo; pronto para **Fase B** (`cpu0` segue em `97,9%`) |

## Fase B — Reduzir o Trabalho Ativo da Game Thread (1/6 concluídos)

| ID | Item | Status | Onde / Observação | Validação |
|---|---|---|---|---|
| **B-1** | Auditar parks/joins e corrigir a classificação do replay | **done** | A3_PGO_r2, frame 5418: `worker91` `127 joins / 300 jobs`, média `0,112 ms/job ≈ 0,11 ms/quadro`; `mdslane` `51,48 ms / 600 quadros ≈ 0,086 ms/quadro`. Soma ≈ `0,20 ms/quadro` bloqueado. Os antigos `38–40%` de `parked in` eram inflados porque `park(kKickParticleEnd)` classificava o replay de `g_kept` como park do job. | A correção `park(kKickParticleEnd, 0u, true)` fica neste PR de telemetria; PR #36 foi fechado sem merge. Não tratar worker waits como o gargalo primário de FPS |
| **B-2** | Separar novos subsistemas para execução paralela | **parked** | O bloqueio agregado real de `worker91 + mdslane` foi ~`0,20 ms/quadro`; ainda não há prova de que uma reestruturação adicional do heap/worker supere custo de cópia, hand-over e dependências | Retomar somente se profiling por subsistema mostrar trabalho independente suficiente e ganho líquido depois de hand-over/merge |
| **B-3** | Identificar e colapsar fronteiras Nativo ↔ MIPS em caminhos quentes | **todo** | Não usar os `31,65%` de MIPS como alvo genérico. Primeiro obter frequência por target/site via `SOTC_CALLGUEST_STATS=600`; a opção força chamadas pelo caminho lento de contagem e desabilita o worker, portanto serve para contagem, não para FPS. Priorizar alvos quentes que aparecem também no perfil de CPU. | Medir chamadas/frame, sites, fração que cai em native e amostras self/inclusive antes de substituir qualquer subárvore; difftest antes de ativar |
| **B-4** | Otimizar os caminhos de colisão já existentes (árvore, leaves e finder) | **todo** | O código já tem `coll115` (árvore/leaf em SoA host), `fin125` (cache host de superfícies do finder, validação 138) e loop `obj125`; top histórico: `ClipCollisionDirectCallBack` `2,19%`, `ClipCollisionFinderCallBack` `1,97%`, `GetCollisionDBCallBack` `1,95%` = `6,11%`. | Medir variantes atuais com `SOTC_COLL115_LOG=1` (gera também `[fin125]`, `[fin125t]`, `[fin125e]` e `[obj125]`) e `SOTC_FIN138_LOG=1` em execuções de diagnóstico separadas; `SOTC_FIN125_CHECK=1` é verificação de exatidão, não um logger independente. Não projetar um novo broadphase antes de provar que a estrutura de árvore atual é o problema |
| **B-5** | Reduzir custo de traversal do display walk e geometria de personagens | **todo** | `DispDispObjGroupModel` `1,75%`, `manager_sub_0111C810` slopeProbe `1,72%`, `ExecCharaGeometry` `1,69%`, `manager_sub_010F3A08` solidDisp `1,63%`; medições próprias já existem em `SOTC_WALK79_LOG=1` (`[walk79]` por fase/callee) e `SOTC_CHARGEO80_LOG=120` (`[chargeo80]` por personagem/callee). | Separar tempo próprio de cada fase, tempo dos callees e contagem de objetos/layout/geodisp; só então escolher loop/callee para A/B |
| **B-6** | Revalidar a distribuição de CPU depois das otimizações B-3/B-5 | **todo** | Baseline A3_PGO_r2: `cpu0 97,9%`, demais núcleos `29,3/36,4/38,8%`; game thread `30,30 ms` mediana | Repetir perfil por núcleo e game p50/p90/p99 na mesma rota; objetivo game `≤16,67 ms`, sem piorar render thread |

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
