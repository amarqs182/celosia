# Auditoria Técnica de Desempenho — Shadow of the Colossus / R36S

**Pergunta central:** quanto desempenho ainda está sendo desperdiçado por manter decisões arquiteturais do PS2 e um runtime genérico, em vez de explorar diretamente o hardware do R36S (`amarqs182/celosia`, `main`, commit base `000597c`)?

> **Escopo do fork:** Celosia (PS Vita / macOS) foi apenas o ponto de partida. Este fork foca **exclusivamente o R36S** (Rockchip RK3326, 4× Cortex-A35 aarch64, Mali-G31 MP2 via ARM `libMali.so` GLES 3.2, SDL2 KMSDRM, Linux `4.4.189`, 895 MiB RAM).

---

## 1. Diagnóstico Executivo e Orçamento para 60 FPS

O projeto já eliminou uma grande parcela da emulação tradicional (GS, VU1, VIF, SPU2 e IOP rodam no host, e 2/3 da `game` thread já estão em C++), mas ainda conserva abstrações, estruturas de memória, regras de sincronização e algoritmos concebidos para o PS2.

### Baseline medido no R36S (`docs/r36s_port.md`, `bench_latest.log`, `gp_b2_game.prof`)
- **Gameplay pesado (Phase 6B, `f=3738..4278` e `5118..5358`, `15` amostras, `draws 1148..2346`):**
  - FPS mediano: **`31,60`** (`29,10–34,30`)
  - `game` thread (mediana): **`31,60 ms`**
  - `render` thread (mediana, paralela): **`29,70 ms`** (`wait 0.0 ms`)
  - `present` (assíncrono): **`14,30 ms`**
- **Cenas 3D em `bench_latest.log`:**
  - `draws >= 500` (`n=28`): `frame avg 28,56 ms` (`37,41 fps`), `game 28,54 ms`, `render 25,09 ms`, `present 13,28 ms`, `wait 0,01 ms`.
  - `draws >= 1000` (`n=20`): `frame avg 30,89 ms` (`32,88 fps`), `game 30,86 ms`, `render 27,75 ms`, `present 14,07 ms`, `wait 0,01 ms`, `draws 1751`.
- **Saturação (`bench_prof129_125s.log`):** nas janelas de gameplay 30 FPS (`w 3`, `w 5`, `w 6`), a `game` thread passa **`99,5%–99,87%`** do tempo executando (`run`), com `wait` de `0,15%–0,48%`.

**Tamanho do desafio até 60 FPS (`16,67 ms` por quadro):**
As threads `game` e `render` rodam em paralelo (`wait 0,0–0,01 ms`), portanto seus tempos não se somam diretamente. Para atingir 60 FPS constantes em gameplay pesado (`31,6 ms` / `29,7 ms`), é necessário reduzir aproximadamente:
- **~47% do tempo da `game` thread** (`31,6 ms → ≤ 16,67 ms`).
- **~44% do tempo da `render` thread** (`29,7 ms → ≤ 16,67 ms`).

Otimizar apenas CPU sem reduzir `render` fará o jogo bater no teto paralelo da GPU/render thread em torno de ~35 FPS.

---

## 2. Desperdício Confirmado no Runtime Atual

### 2.1 Instrumentação de memória e logs ativos no build R36S (prioridade máxima)
- Em [`ps2_runtime_macros.h`](../PS2Recomp/ps2xRuntime/include/ps2_runtime_macros.h#L631-L645):
  - `PS2X_GUEST_READ_WATCH` e `PS2X_GUEST_WRITE_TRACE` são zerados apenas para `#if defined(__vita__)`; fora do Vita (Linux/R36S) ambos têm padrão **`1`**.
  - Como `g_ps2ReadWatchMap` em [`ps2_runtime.h`](../PS2Recomp/ps2xRuntime/include/ps2_runtime.h#L251-L252) é **`volatile`**, todo `READ*` do código recompilado/lifted executa leitura volátil + comparação + desvio, e todo `WRITE*` verifica `g_ps2WatchWriteEnabled`.
- Em [`PS2Recomp/ps2xRuntime/CMakeLists.txt`](../PS2Recomp/ps2xRuntime/CMakeLists.txt#L13-L14):
  - `PS2X_ENABLE_RUNTIME_LOGS`, `PS2X_ENABLE_AGRESSIVE_LOGS` e `PS2X_ENABLE_IOP_RPC_TRACE` têm padrão **`ON`**.
  - `PS2X_LINUX_RELEASE=ON` define `SOTC_RELEASE=1`, mas **não desliga** `PS2X_GUEST_READ_WATCH`, `PS2X_GUEST_WRITE_TRACE` nem `PS2X_ENABLE_RUNTIME_LOGS`. No `build/linux-aarch64/CMakeCache.txt` atual do R36S (`PS2X_LINUX_RELEASE=ON`), `PS2X_ENABLE_RUNTIME_LOGS:BOOL` permaneceu **`ON`**.
- **Ação:** configurar produção R36S com `-DPS2X_GUEST_READ_WATCH=0`, `-DPS2X_GUEST_WRITE_TRACE=0`, `-DPS2X_ENABLE_RUNTIME_LOGS=OFF`, `-DPS2X_ENABLE_AGRESSIVE_LOGS=OFF`, `-DPS2X_ENABLE_IOP_RPC_TRACE=OFF` e **medir o ganho real por A/B no R36S** (sem assumir percentual prévio).

### 2.2 Trampolins de memória (`PS2X_GUEST_MEM_TRAMP=0`): prioridade baixa
- O caminho normal dos 32 MB de RAM guest em [`ps2_runtime_macros.h`](../PS2Recomp/ps2xRuntime/include/ps2_runtime_macros.h#L787-L807) já é inline (`if (PS2_EXPECT(_addr < PS2_RAM_SIZE, 1)) return load<T>(rdram, _addr);`), sem chamada externa.
- Os slow paths atendem apenas endereços fora da RAM principal (I/O, scratchpad, espelhos kseg). O trampoline existente foi feito especificamente para a ABI ARMv7 Thumb-2 do PS Vita; só faz sentido construir equivalente aarch64 se o profiler provar peso relevante desses slow paths.

### 2.3 Dispatch de funções (`callGuestFn`): colapsar fronteiras, não remover cegamente
- `PS2Runtime::callGuestFn()` e `callGuestKnown()` em [`ps2_runtime.h`](../PS2Recomp/ps2xRuntime/include/ps2_runtime.h#L739-L770) são `always_inline`, e o projeto já possui `tailGuestKnown()` e encadeamento `callNative`/`callDirect`.
- O verdadeiro custo está nas **fronteiras repetidas `Nativo → Guest dispatch → MIPS recompilado → Guest dispatch → Nativo`**, convertendo subsistemas inteiros em chamadas nativas diretas sem retorno ao contexto R5900.

---

## 3. O Maior Problema: Ainda Executamos o Jogo Como um PS2

### 3.1 Distribuição real da `game` thread (`gp_b2_game.prof`)
Em `26,375` amostras (`26 s`, `idle 84` = `0,32%`, `busy 26,291` = `99,68%`, **`1,428` funções distintas**):
- **`hand_native`** (`351` endereços em `game/overrides/`): **`66,38%`** (`17,452` amostras)
- **`recompiled_MIPS`**: **`31,65%`** (`8,322` amostras)
- **`lifted_D1_D5`** (`621` endereços em `decomp_lift*`): **`1,97%`** (`517` amostras)

Participação acumulada extremamente plana: Top 10 = **`17,10%`**, Top 20 = **`26,45%`**, Top 50 = **`43,11%`**, Top 100 = **`58,37%`**, Top 200 = **`73,80%`**.

| Top nativos (`gp_b2_game.prof`) | % busy | Top não-nativos (`gp_b2_game.prof`) | % busy |
| --- | --- | --- | --- |
| `ClipCollisionDirectCallBack @0112d590` | `2,19%` | `gamecore_sub_013E3840 @013e3840` | `1,08%` |
| `ClipCollisionFinderCallBack @01125cc0` | `1,97%` | `isysGroupForFirst @001c7110` | `0,74%` |
| `GetCollisionDBCallBack @01124c88` | `1,95%` | `isysWakeupSubThread @001c9a40` | `0,71%` |
| `DispDispObjGroupModel @010f52c8` | `1,75%` | `SignalSema @00107000` | `0,64%` |
| `manager_sub_0111C810 @0111c810` | `1,72%` | `gamecore_sub_013CF258 @013cf258` | `0,56%` |
| `ExecCharaGeometry @010bf8f0` | `1,69%` | `modelDisp @01097118` | `0,45%` |
| `manager_sub_010F3A08 @010f3a08` | `1,63%` | `modelSetSurface @01099768` | `0,43%` |
| `gamecore_sub_013CFBE0 @013cfbe0` | `1,61%` | `manager_sub_010C7B28 @010c7b28` | `0,34%` |
| `manager_sub_010F3CF0 @010f3cf0` | `1,35%` | `GetMotionResourceOriginalFrames @010b3af8` | `0,30%` |
| `ClipCollisionFinder @01125dc8` | `1,25%` | `iosSendMsg @001b0fc0` | `0,27%` |

**Lição crítica:** distinguir **linguagem nativa (C++)** de **arquitetura nativa**. Ter `66,38%` em C++ ainda reproduz estruturas na RAM de 32 MB, árvores de colisão/objetos originais, traversals repetidos, organização de heap PS2 e callbacks/semáforos `isys` (`>3,15%` no Top 20 não-nativo). Só as três callbacks de colisão no topo somam **`6,11%`**. O ganho grande virá de reorganizar subsistemas inteiros (broadphase espacial, cache intra-frame, processamento em lote e eliminação de trabalho redundante), não de micro-substituições isoladas.

---

## 4. Paralelismo nos 4× Cortex-A35 (`sotc_worker.cpp`)

- Em `bench_latest.log` (frame `4218`), `worker91` teve **`62%` dos 300 jobs estacionados (`parked in 62%`)** em `011697e8` (`manager_sub_011697E8`), com `381` chamadas exclusivas de heap (`001b0328` `iosFree` de `0113ac64` `manager_sub_0113AAE0`), `1,524` chamadas retidas para o fim e `193` esperas de join (`0,187 ms/job`).
- Em `[mdslane]`, joins forçaram a `game` thread a executar `1,258` jobs em `146` joins (`14,52 ms`) em um trecho e `5,396` jobs em `724` joins (`74,96 ms`) em outro.
- **Direção correta:** em vez de tentar tornar o espaço/heap PS2 thread-safe para qualquer endereço guest, separar trabalho em **snapshots imutáveis** + buffers/memória independentes por worker (partículas/vento, geometria/animação, consultas independentes) entregues em lote.

---

## 5. Renderização (Mali-G31 MP2) e CPU-Target Específico

1. **Planejamento e cópias (`bench_pass7_120s.log`, `[gl:phases]` / `[gl:draws]`):**
   - `1,482` draws reduzidos a `197–206` GL draw calls, mas com `plan 4,01–4,03 ms`, `passes 10,66–10,81 ms` (`draw calls 6,09–6,22 ms`) e `run copies 7.112–13.025 KB/frame` (~`416–762 MiB/s` caso rodasse a 60 FPS). É necessário auditar cópias entre arenas, conversões de formato e recriação de buffers na submissão.
2. **Alpha test em 2 passes (`[shadow]` / `[r93]`):**
   - `bench_latest.log` registra `724,7/s` second passes de alpha test. Qualquer simplificação de passo único precisa de comparação pixel a pixel e medição de impacto no *early-Z* da Mali-G31.
3. **Precisão de shaders (`native_renderer_gl.cpp`):**
   - [`native_renderer_gl.cpp`](../PS2Recomp/ps2xRuntime/src/lib/native/native_renderer_gl.cpp#L84-L86) usa `precision highp float; precision highp int;` global no `#version 310 es`.
   - A GPU Mali-G31 (Bifrost) suporta operações FP16 com dobro de vazão em operações elegíveis. Deve-se usar **Mali Offline Compiler** para analisar os shaders reais e separar: `highp` em transformações/posições de mundo vs `mediump` validado visualmente em iluminação, cores, partículas, máscaras e fog.
4. **Flags de CPU para Cortex-A35 (`PS2Recomp/CMakeLists.txt`):**
   - Hoje usa `-march=armv8-a+fp+simd` genérico.
   - Testar primeiro `-march=armv8-a+fp+simd -mtune=cortex-a35`, depois separadamente `+crc+crypto` (`AGENTS.md`: A35 tem `aes pmull sha1 sha2 crc32`), evitando expandir `-Os` cegamente para `-O3` em código frio por causa da I-cache pequena do A35 (usar PGO real do R36S para guiar unidades quentes).
5. **Áudio SPU2 (`[audio-ticks]`):**
   - `142,6 ms` (voices) + `162,8 ms` (reverb 24 kHz) = `305,4 ms` por 5 s (~`6,1%` de 1 núcleo em thread separada). Oportunidade secundária: vetorização NEON de mixagem/ADPCM/delay lines sem degradação sonora.

---

## 6. O Que Abandonar vs. O Que Especializar

- **Remover por limpeza/manutenção (pouco impacto direto em FPS, pois `#ifdef` já exclui grande parte no Linux):** `Metal` / macOS (`native_renderer_metal.mm`, `gs_metal_raster.mm`), `vitaGL` / Vita SDK (`__vita__`), shaders Cg.
- **Não utilizar no R36S:** Vulkan com WSI/swapchain tradicional (ausente no `libMali` `-gbm.so` / kernel `4.4.189`), OpenGL desktop / Mesa (`panfrost` indisponível).
- **Manter e especializar para o R36S:** SDL2 KMSDRM + OpenGL ES 3.2 (`libMali.so`) e substituição progressiva de subsistemas e estruturas PS2 por arquitetura nativa ARM64.
