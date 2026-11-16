# Worklist — R36S Nativo (`amarqs182/celosia`)

Status: **todo**, **doing**, **done**, **parked**.
Alvo exclusivo: **R36S** (Rockchip RK3326, 4× Cortex-A35 aarch64, Mali-G31 MP2 via ARM `libMali.so` OpenGL ES 3.2, SDL2 KMSDRM, Linux `4.4.189`, 895 MiB RAM).
Auditoria técnica completa em [`r36s_native_audit.md`](r36s_native_audit.md) e notas de hardware/port em [`r36s_port.md`](r36s_port.md).

> **Diretriz:** O PS2Recomp e a herança do Celosia (PS Vita / macOS) foram o ponto de partida para rodar o jogo. O foco exclusivo passa a ser tratar o R36S como um sistema ARM64/Mali-G31 nativo, removendo verificações genéricas em execução e substituindo subsistemas presos à arquitetura do PS2.

## Baseline atual medido no R36S (`A3_PGO_r2` / `PR #37`)

- **Rota determinística de 130 s, janela casada `4500..5900` (`n=1401`):**
  - **`A3_PGO_r2` (Fase A):** `32,34 FPS` média / **`33,00 FPS` mediana**; frame time `p50 30,30 ms`, `p90 34,20 ms`, `p99 40,80 ms`; game mediana `30,30 ms` (média `30,91 ms`); render mediana `20,30 ms` (média `25,65 ms`).
  - **`PR #37` (baseline Fase B pós-fix de telemetria, `9ea397c3bc7bdf2a2a861bb6f3f12cb4`):** `32,74 FPS` média / **`33,44 FPS` mediana** (`p50 29,90 ms`, `p90 34,00 ms`, `p99 39,40 ms`, game med `29,90 ms` / média `30,53 ms`, render med `19,60 ms` / média `25,75 ms`; para todo `f>=4500` com `n=1524`: `32,62` média / `33,33` med FPS, `game 30,00 / 30,64 ms`, `render 19,70 / 25,75 ms`).
- **CPU no trecho amostrado (`PR #37`, `t=80..120s`):** `cpu0 98,1%`, `cpu1 28,6%`, `cpu2 36,1%`, `cpu3 37,3%`; RSS média `340,9 MiB` (`A3_PGO_r2`: `cpu0 97,9%`, RSS `337,2 MiB`).
- **Meta para 60 FPS (`16,67 ms`):** mantendo o `PR #37` na mesma rota casada, `game` precisa cair ~44% (`29,90 → 16,67 ms`) e `render` precisa cair ~15% na mediana (`19,60 → 16,67 ms`) além de reduzir a média (`25,75 ms`).
- **Perfil Release pós-Fase A (`D0_PROF` / `a3_pgo_game.prof`, `43.755` amostras = `44 s`, `99,6%` EE busy):** Top 5 = `ClipCollisionDirectCallBack` (`2,80%`), `gamecore_sub_013CFBE0` (`1,97%`), `DispDispObjGroupModel` (`1,94%`), `manager_sub_010F3A08` (`1,93%`), `GetCollisionDBCallBack` (`1,89%`), `ClipCollisionFinderCallBack` (`1,89%`) — distribuição plana com no máximo `2,80%` por função.
- **Referência histórica:** Phase 6B pré-Fase A mediu `31,60 FPS`, game `31,60 ms`, render `29,70 ms`; `gp_b2_game.prof` (`66,38%` hand_native, `31,65%` recompiled_MIPS) antecede a Fase A. Não misturar janelas diferentes com a rota casada atual.

---

## Fase A — Eliminar Desperdício Comprovado (5/5 concluídos)

| ID | Item | Status | Onde / Observação | Validação |
|---|---|---|---|---|
| **A-1** | Desabilitar `PS2X_GUEST_READ_WATCH=0` e `PS2X_GUEST_WRITE_TRACE=0` no build release R36S | **done** | [`PS2Recomp/ps2xRuntime/CMakeLists.txt`](../PS2Recomp/ps2xRuntime/CMakeLists.txt#L16-L18) (`PS2X_ENABLE_GUEST_READ_WATCH` / `PS2X_ENABLE_GUEST_WRITE_TRACE`, desabilitados automaticamente quando `PS2X_LINUX_RELEASE=ON`) | Binário stripped encolheu `78,04 → 64,28 MB` (`-13,76 MB`); com PGO antigo (`pgo_merged`), `-Wcoverage-mismatch` descartou perfis nas funções cujo CFG mudou (`leaderNative` `141→91` arcos), mantendo `30,20` vs `30,38 FPS` medido |
| **A-2** | Desabilitar explicitamente `PS2X_ENABLE_RUNTIME_LOGS=OFF`, `PS2X_ENABLE_AGRESSIVE_LOGS=OFF` e `PS2X_ENABLE_IOP_RPC_TRACE=OFF` no Release R36S | **done** | [`PS2Recomp/ps2xRuntime/CMakeLists.txt`](../PS2Recomp/ps2xRuntime/CMakeLists.txt#L506-L550) (`PS2X_LINUX_RELEASE=ON` garante todas desligadas junto com `READ_WATCH=0` / `WRITE_TRACE=0`) | `A2` (`A1` + `RUNTIME_LOGS=OFF`): `30,24 / 30,96 FPS` (`p99` `44,40 → 43,81 ms`, `max` `130,4 → 62,6 ms`, `RSS` `-2,6 MiB`) com `pgo_merged` desatualizado |
| **A-3** | Comparar build original vs. build limpo com PGO casado no R36S | **done** | Corpus `pgo_phase_a` gerado no R36S (`SOTC_GCOV_DUMP_FRAME=5200`, `214` `.gcda` com `0` divergências `-Wcoverage-mismatch`); [`SOTC_OPTIMIZATION_LOG.md`](../SOTC_OPTIMIZATION_LOG.md) | **Rota casada `4500..5900` (`n=1401`):** `A0` `30,38 mean / 31,06 med FPS` (`game 32,20 ms`) → **`A3_PGO` `32,34 mean / 33,00 med FPS`** (**`+1,96 FPS` / `+6,45%`**, `game 30,30 ms` `-6,08%`, `p99 44,40→40,80 ms`, binário `63,43 MB` `-14,61 MB`) |
| **A-4** | Padronizar `-mtune=cortex-a35` e opções `+crypto+crc` no CMake ARM64 | **done** | [`PS2Recomp/CMakeLists.txt`](../PS2Recomp/CMakeLists.txt#L24-L26) (`PS2X_ARM64_MTUNE="cortex-a35"`; `PS2X_ARM64_ENABLE_CRYPTO=ON` + `PS2X_ARM64_ENABLE_CRC=ON` evitam sobrescrita por `-march=armv8-a+fp+simd`) | `-mtune=cortex-a35` já vinha via `CMAKE_CXX_FLAGS` em `A0–A2`; ativar `+crypto+crc` sem código novo usando intrínsecos gerou MD5 idêntico a `A2` (`330a444ed7002925d88c54fafcb1d9ab`) |
| **A-5** | Validar integridade visual e desempenho com gameplay real após Fase A | **done** | Capturas `SOTC_GL_SHOTS=300` (`shot_2400`, `shot_3000`, `shot_3600`, `shot_3900`) + implantação em `/roms/ports/.SotC_Celosia/ps2sotc_aarch64_pie_sdl` (`f8ee05adb7dfdc287b3f13df0c835f98`) | 0 artefatos visuais no menu, interior do Shrine (light rays aditivos), cutscene e terreno externo; pronto para **Fase B** (`cpu0` segue em `97,9%`) |

## Fase B — Reduzir o Trabalho Ativo da Game Thread (2/6 concluídos)

| ID | Item | Status | Onde / Observação | Validação |
|---|---|---|---|---|
| **B-1** | Auditar parks/joins e corrigir a classificação do replay | **done** | A3_PGO_r2, frame 5418: `worker91` `127 joins / 300 jobs`, média `0,112 ms/job ≈ 0,11 ms/quadro`; `mdslane` `51,48 ms / 600 quadros ≈ 0,086 ms/quadro`. Soma ≈ `0,20 ms/quadro` bloqueado. Os antigos `38–40%` de `parked in` eram inflados porque `park(kKickParticleEnd)` classificava o replay de `g_kept` como park do job. | `park(kKickParticleEnd, 0u, true)` no PR #37 (`32,74` média / `33,44` med FPS em `4500..5900`); PR #36 foi fechado sem merge. Worker waits não são o gargalo primário de FPS |
| **B-2** | Separar novos subsistemas para execução paralela | **parked** | O bloqueio agregado real de `worker91 + mdslane` foi ~`0,20 ms/quadro`; ainda não há prova de que uma reestruturação adicional do heap/worker supere custo de cópia, hand-over e dependências | Retomar somente se profiling por subsistema mostrar trabalho independente suficiente e ganho líquido depois de hand-over/merge (retestar `SOTC_CHAR104=1` na base `PR #37` se aplicável em B-5) |
| **B-3** | Identificar e colapsar fronteiras Nativo ↔ MIPS em caminhos quentes | **doing** | Em `DIAG` (`D5_DIAG`, `f=4200-4800`): `8006,5 calls/frame` (`5772,1` para nativos; ex.: `010a9ea8` `335,2`, `010c7b28` `311,4`, `01433450` `187,9`). Corrigido bug em [`decomp_collcensus.cpp`](../game/overrides/decomp_collcensus.cpp#L240-L248) onde `SOTC_COLL_CENSUS=0` / `SOTC_COLLISION_STATS=0` ligavam wrappers no `DIAG`, quebrando `sotc_decomp::isNative()` em 6 nativos (`0111c078`, `0112a250`, etc.). `00107000` (`SignalSema`, `213,4/f`) omite `"native"` por usar hook fora de `registry()`. | `D5_DIAG` confirmou `"native"` correto nos 6 nativos de colisão; `[ab]` inativo (`SOTC_AB` não setado) e `010f4890` (`ClipLayoutObjModel`) já marca `native`. Próximo passo em B-3/B-5: avaliar `gamecore_sub_013CFBE0` (`1,97%`), `010c7b28`, `01433450` e sítios quentes com difftest |
| **B-4** | Otimizar os caminhos de colisão já existentes (árvore, leaves e finder) | **done** | Diagnósticos limpos (`D5_DIAG`, `D9_COLL_LOG`, `D10_FIN125_CHECK`) + fix do contador `[collision] keep` em [`decomp_collision.cpp`](../game/overrides/decomp_collision.cpp#L86-L154) e [`decomp_hot44a.cpp`](../game/overrides/decomp_hot44a.cpp#L232-L238) (`native test` vs fallback `recompiled`). Em gameplay, `coll115` (`fallback 0.00, original 0.00`), `fin125` (`guest entries 0.00`) e `seg123` (`20–25% kept`, `SOTC_SEG123=1`) já cobrem os caminhos quentes; `ClipCollisionDirectCallBack` é apenas `2,80%` no `D0_PROF`. | A/B Release+PGO casado (`4500..5900` vs `PR #37` `32,74 / 33,44 FPS`): `B4_NOSEG123` (`SOTC_SEG123=0`) = `32,57 / 33,33` (`seg123` mantido ON); `B4_COLLDB` (`insertDirect`+`groupCall`, `0` diffs no difftest) = `32,35 / 33,33` (`-0,39 FPS`); `B4_COLL125` (cache 4 slots em `coll125.h`, `22078` clips com `differ 0`) = `32,30 / 33,00` (`-0,44 FPS`, `render` subiu `19,60 → 20,20 ms` por pressão no L2 de 256 KB do Cortex-A35). Ambos os patches descartados; colisão encerrada na base `PR #37` |
| **B-5** | Reduzir custo de traversal do display walk e geometria de personagens | **todo** | Diagnósticos já coletados (`D2_WALK79`, `D3_CHARGEO80`, `D0_PROF`): `[walk79]` `f=4560`: `all 5,06 ms` (`010f5920` `2,41/x48,4`, `010f4890` `0,41/x161,3`, `013fdd68` `0,24/x1`); `[chargeo80]` `f=5782`: `01a62600` `4,274 ms` e `01bc95d0` `3,828 ms` dominados por `010d9fc0`, `010bba60`, `010e21c0`, `01137a68`; `DispDispObjGroupModel` `1,94%`, `010F3A08` `1,93%`, `ExecCharaGeometry` `1,69%` | Próximo alvo de CPU: reavaliar `SOTC_CHAR104=1` na base `PR #37` (`A3_PGO`) e investigar redução de trabalho em `DispDispObjGroupModel` / `010F3A08` / `013CFBE0` sem inflar cache L2/I-cache |
| **B-6** | Revalidar a distribuição de CPU depois das otimizações B-3/B-5 | **todo** | Baseline `PR #37`: `cpu0 98,1%`, demais núcleos `28,6/36,1/37,3%`; game thread `29,90 ms` mediana (`30,53 ms` média) | Repetir perfil por núcleo e game p50/p90/p99 na mesma rota; objetivo game `≤16,67 ms`, sem piorar render thread |

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
