# Roteiro de Otimizações de Alta Performance para R36S (Inspirado no Remaster PS4)

Este documento estabelece a arquitetura, prioridades e estratégias de implementação para adaptar as melhorias técnicas do remake/remaster moderno de *Shadow of the Colossus* para o port nativo **Celosia** no console portátil **R36S** (Rockchip RK3326, 4x Cortex-A35 @ 1.5 GHz, GPU Mali-G31 MP2 @ 600 MHz, 1 GB RAM, ArkOS Linux aarch64, OpenGL ES 3.2).

---

## 1. Matriz de Prioridade e Impacto no Hardware

O hardware do R36S possui duas restrições fundamentais:
1. **GPU Mali-G31 MP2**: Arquitetura *Tile-Based Deferred Rendering* (TBDR) com 2 núcleos de shader e largura de banda de memória limitada (~4.5 GB/s compartilhada). Transparências sobrepostas e quebra de *Early-Z* estrangulam a GPU.
2. **CPU Quad Cortex-A35**: Processador in-order ultra-eficiente de baixo consumo, com IPC modesto. O despache excessivo de chamadas de desenho (*draw calls*) e transformações matemáticas por vértice na CPU causam gargalos de thread.

| Posição | Subsistema | Impacto no R36S | Alvo Principal | Meta de Ganho |
| :---: | :--- | :---: | :--- | :---: |
| **1º** | **Pelagem (Fur Shell Clamping & LOD)** | **CRÍTICO** | Eliminar overdraw 16x de transparências na Mali-G31 | **+10 a +15 FPS** nas batalhas |
| **2º** | **Renderização (Batching & State Sorting)** | **MUITO ALTO** | Reduzir overhead de chamadas e trocas de tiles | **+5 a +8 FPS** em áreas densas |
| **3º** | **Animações (GPU Skinning via UBO)** | **ALTO** | Descarregar transformações de ossos da CPU A35 | **+3 a +5 FPS** de alívio na CPU |
| **4º** | **Iluminação (Remoção de Passes Aditivos)** | **ALTO** | Economizar fillrate e banda de memória | **+3 a +4 FPS** no Saguão/Sol |
| **5º** | **Shaders (FP16 `mediump` sem `discard`)** | **MÉDIO-ALTO** | Dobro de taxa aritmética e Early-Z ativo | **+2 a +4 FPS** contínuos |
| **6º** | **Colisão (Cache de Raycast de Câmera)** | **MÉDIO** | Reduzir testes de interseção repetidos por frame | **+1 a +2 FPS** (sem micro-stutter) |
| **7º** | **Física (Vetorização SIMD NEON)** | **MÉDIO** | Acelerar integração de partículas de tecido | **+1 FPS** (30 FPS estável) |
| **8º** | **Inteligência Artificial (Tick Decoupling)** | **BAIXO** | Intercalar nós distantes de navegação | Estabilidade marginal |
| **9º** | **Lógica de Gameplay (Deadzone & Câmera)** | **QoL** | Conforto dos analógicos e jogabilidade | **0 FPS** (Qualidade de Vida) |

---

## 2. Fase 1: Otimização da Pelagem dos Colossos (Fur Rendering)

### 2.1 Problema no PS2 / Celosia
No PS2 original, os Colossos e o rabo da égua Agro utilizam técnica de **Multi-Layer Fur Shells**:
* Uma malha base é duplicada concentricamente para fora ao longo de suas normais em **8 a 16 cascas (*shells*)**.
* Cada casca recebe uma textura de pelos pontilhados com *Alpha Testing*.
* Na GPU Mali-G31, 16 camadas translúcidas sobrepostas geram um *overdraw* de 16x na memória de tiles de 16 KB.
* O teste alfa tradicional desativa o *Hidden Surface Removal* (HSR / Early-Z), forçando a GPU a executar o fragment shader em todos os 16 fragmentos para cada pixel da tela.

### 2.2 Estratégia de Engenharia
1. **Dynamic Shell Clamping**:
   * Detectar as sub-malhas pertencentes à pelagem (`d.state.blend == Blend::Normal` com teste alfa sequencial e incremento de offset de normais).
   * Limitar o número de cascas ativas de acordo com a distância da câmera:
     * **Distância Curta (< 5 metros / Wander agarrado)**: 6 cascas.
     * **Distância Média (5 a 25 metros)**: 3 cascas.
     * **Distância Longa (> 25 metros)**: 1 casca (malha sólida texturizada).
2. **Dithered Alpha Stippling**:
   * Substituir o descarte pesado de fragmentos por dithering de matriz 4x4 ordenado, preservando a coerência do Early-Z da Mali.

---

## 3. Fase 2: Batching e Ordenação de Estado (State Sorting)

### 3.1 Problema Atual
No saguão do templo (*Shrine of Worship*), o altar e os pilares submetem mais de 2.000 draw calls individuais, a maioria com poucas dezenas de vértices:
* A Mali-G31 precisa alocar e sincronizar listas de comandos e descritores de textura para cada draw.
* Trocas aleatórias de programas de shader e texturas fragmentam os blocos do rasterizador.

### 3.2 Estratégia de Engenharia
1. **Chave de Ordenação de 64-bit**:
   ```
   [63..48: Render Target/FBO] | [47..32: Program ID] | [31..16: Texture ID] | [15..8: Blend Mode] | [7..0: Depth State]
   ```
2. **Fusão de Malhas Estáticas no Mesh Pool**:
   * Concatenar os vértices de sub-malhas consecutivas com o mesmo material em buffers de vértices compartilhados (`kPoolVertices = 0xFFFFu`), emitindo uma única chamada `glDrawElements` em vez de centenas de pequenos draws.

---

## 4. Fase 3: GPU Skinning via Uniform Buffer Objects (UBO)

### 4.1 Problema Atual
Atualmente, as deformações de vértices de personagens (Wander, Agro e partes móveis dos Colossos) são computadas no thread da CPU (`sotc_worker.cpp` / `decomp_char104.cpp`), consumindo preciosos ciclos dos núcleos Cortex-A35.

### 4.2 Estratégia de Engenharia
1. Alocar um Uniform Buffer Object no OpenGL ES 3.2 contendo a matriz de ossos (`mat4 uBones[64]`).
2. Atualizar o vertex shader nativo (`native_renderer_gl.cpp`) para ler os índices de ossos e pesos presentes nos atributos de vértice:
   ```glsl
   #version 300 es
   precision mediump float;
   layout(location = 0) in vec3 aPos;
   layout(location = 1) in vec3 aNormal;
   layout(location = 2) in vec4 aBoneIndices;
   layout(location = 3) in vec4 aBoneWeights;
   layout(std140) uniform BoneBlock {
       mat4 uBones[64];
   };
   ```
3. A GPU Mali-G31 MP2 realiza a multiplicação vetorial em paralelo com a rasterização, liberando a CPU para manter 30 FPS estáveis na lógica de jogo e VU1 recompiler.

---

## 5. Fase 4: Shaders e Precisão Half-Float (FP16)

### 5.1 Otimização na Arquitetura Bifrost
A GPU ARM Mali-G31 possui o dobro de unidades aritméticas para dados de 16-bit (`mediump float`) do que para 32-bit (`highp float`).
* Declarar explicitamente `precision mediump float;` em todos os shaders de fragmento e variáveis intermediárias de cor, UV e vetores normalizados.
* Reduz a ocupação de registradores por thread pela metade, eliminando *register spills* para a memória RAM lenta.

---

## 6. Histórico de Implementação

* **v1.0.0 (Novembro 2026)**:
  * Diagnóstico do gargalo de reflexão e refração (1.400 draws removidos do templo).
  * Reversão do shader fotográfico para passthrough limpo 1:1 (`raylib_linux.cpp`).
  * Ativação dos perfis de alívio de geometria do altar (`SOTC_SAC_LOD=1`, `SOTC_SAC_DRAWDIST=1`).
  * Documentação e roadmap consolidados.
