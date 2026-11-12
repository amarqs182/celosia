# Celosia: Plano de Modernização e Evolução Algorítmica

Este documento estabelece as diretrizes para superar as limitações herdadas da arquitetura do PlayStation 2 (Emotion Engine, VU0/VU1, Graphics Synthesizer), transitando de uma **otimização equivalente** (recompilação direta) para uma **melhoria algorítmica** (reengenharia de estruturas de dados e algoritmos modernos).

---

## 1. Princípio Fundamental: Otimização Equivalente vs. Melhoria Algorítmica

| Abordagem | Definição | Exemplo no Celosia | Ganho Típico |
|---|---|---|:---:|
| **Otimização Equivalente** | Executar a mesma lógica original de forma mais eficiente no hardware atual. | Vetorizar com ARM NEON a rotina de relaxamento de tecidos do VU0 sem alterar as fórmulas. | 1.5x – 3x |
| **Melhoria Algorítmica** | Substituir o algoritmo/estrutura legado por uma solução moderna superior. | Trocar a varredura $O(N)$ de colisores por uma árvore BVH/Octree hierárquica com *dirty flags*. | 5x – 20x |

A recompilação estática livra o jogo do emulador de CPU, mas ainda mantém algoritmos projetados para a memória minúscula (32 MB RDRAM) e para a coprocessador VU0 do PS2. A evolução real do motor consiste em redesenhar esses pontos críticos.

---

## 2. Os 4 Pilares de Engenharia

### Pilar A: Física e Colisão
* **Broadphase Espacial Acelerado**:
  * Implementação de AABB Tree dinâmica ou Grid Espacial com hashing para descartar pares de colisão distantes em $O(1)$, antes do *narrowphase* poligonal.
* **Memoização e Caching Temporal de Raycasts**:
  * Raios de câmera e de verificação de sustentação de pés (*foot IK*) com cache temporal: se nem o Wander nem a geometria se moveram além de um limiar $\epsilon$, reutilizar o hit do frame anterior.
* **Resolução Contínua de Contato com Colossos**:
  * Substituir o modelo de *point-ray polling* discreto do PS2 por um tratamento de contato local baseado em coordenadas de superfície (UV / barycentric space no colosso), eliminando quedas acidentais do Wander quando a malha do colosso deforma bruscamente.

---

### Pilar B: Lógica e Processamento
* **Desacoplamento de Taxa de Atualização (Tick Throttling)**:
  * Entidades decorativas (aves, lagartos, peixes) e lógicas secundárias não precisam atualizar a 60 Hz quando fora da tela. Executar em cadência adaptativa (15–30 Hz) com interpolação linear de posição.
* **Paralelização Assíncrona Segura**:
  * Isolar tarefas que possuem dependência de leitura pura (ex: raycasts de visibilidade e cálculo de tecidos) em *job tasks* distribuídos nos núcleos ociosos do SoC, minimizando barreiras de sincronização e *spin-waits*.

---

### Pilar C: Shaders e Renderização Nativa GLSL
* **Desacoplamento Completo dos Limites do GS do PS2**:
  * Não tentar reproduzir as deficiências do framebuffer de 4 MB do PS2 (que forçava multipass com blending cumulativo).
  * Criação de shaders próprios em GLSL ES 3.2 que calculam iluminação direta, ambiente e atenuação em **Single-Pass**.
* **Tratamento de Iluminação e Tonemapping Consciente**:
  * Iluminação calculada no espaço linear com compressão suave de altas luzes (evitando estouramento do sol e chão lavado do saguão) sem o peso de pós-processamento multi-render-target.
* **Minimização de Trocas de Estado (State Sorting)**:
  * Manter a pipeline de renderização ordenada por Textura e Shader (`glSort87`), maximizando a união de draws (`glMergeDraws`).

---

### Pilar D: Animações e Esqueletos
* **Hierarchical Dirty Flagging**:
  * Marcar ramos da hierarquia de ossos que não sofreram rotação/translação neste frame para pular a computação da matriz global correspondente.
* **GPU Skinning via UBOs**:
  * Enviar as matrizes dos ossos uma única vez para a GPU e permitir que a Mali-G31 transforme os vértices diretamente no Vertex Shader, liberando 100% da CPU dos cálculos de deformação de vértices.

---

## 3. Metodologia de Execução em 5 Etapas

1. **Profiling e Medição de Hotspots**: Identificar via `sotc_prof.cpp` e contadores de hardware os gargalos exatos da CPU.
2. **Reimplementação em C++ Moderno**: Escrever a lógica em C++20 limpo, com tipagem forte e alinhamento de memória adequado.
3. **Evolução Algorítmica**: Aplicar as estruturas superiores (BVH, Dirty Flags, Hashing).
4. **Validação Diferencial (*Difftest*)**: Comparar as saídas com a rotina original do jogo para assegurar que não haja desvios de gameplay ou física.
5. **Integração e Benchmarking no Dispositivo Alvo (R36S)**: Medir frame-times reais, verificar estabilidade e comitar com governança estrita.
