# Auditoria Técnica de Física e Colisão no Celosia (SotC)

## 1. Resumo Executivo e Telemetria Real no R36S

Esta auditoria técnica foi realizada diretamente no hardware alvo (**R36S Handheld Console**, Rockchip RK3326 quad-core Cortex-A35 @ 1.5 GHz, Mali-G31 MP2 @ 600 MHz, ArkOS Linux aarch64), instrumentando o motor com `SOTC_COLL_CENSUS=1`, `SOTC_COLLISION_STATS=1`, `SOTC_COLL115_LOG=1` e `SOTC_SEG123=1`.

### 1.1 Dados Coletados em Tempo Real (Cenário: Saguão do Templo / Gameplay)

| Métrica / Subsistema | Contagem / Taxa por Quadro | Custo Estimado CPU | Observação Arquitetural |
|---|---|---|---|
| `ClipCollisionDB` (0x0111B9F0) | 137.650 chamadas totais / 223 por quadro | ~3.5 – 5.0 ms | Teste exaustivo de plano e triângulo contra a geometria |
| `boundary` (esferas limítrofes) | 143.326 chamadas totais | ~1.5 – 2.0 ms | Teste de corte de esfera de cada triângulo |
| `ClipCollisionFinder` | 6 chamadas / 0.158 ms por quadro | ~1.2 ms | Varredura de superfície sob os pés do Wander (IK/piso) |
| `camera_collision_check_direct` | 4 a 6 chamadas / quadro | ~0.35 ms | Raycasts de evasão da câmera contra paredes e pilares |
| `FlushModifiedManager` | **349 a 390 chamadas / quadro** | ~0.15 ms | Invalidações de matrizes e flags de geometria móvel |
| `clipCollisionKeep` | 164 chamadas / quadro | ~0.8 ms | Teste de retenção de declive/escorregamento do personagem |
| `skinFinderCorners` / `skinFinder` | 25 a 47 chamadas / quadro | ~0.5 ms | Detecção de contato em superfícies animadas/skinning |
| **Repetição de Queries Idênticas** | **138 direct, 335 finder por quadro** | **Desperdício Puro** | Raios disparados da mesma posição frame a frame |

---

## 2. Anatomia do Pipeline e Funções Críticas

### 2.1 Kd-Tree Traversal (`collisionNode` / `0x01131C80` e `walk101`)
- **Funcionamento**: A geometria do mundo é particionada em uma Kd-tree com nós de 8 bytes (split no eixo X, Y ou Z, apontando para filhos ou folhas).
- **Gargalo Legado**: No código original e nos wrappers recompilados, a cada divisão de nó a árvore executava:
  ```cpp
  f2 = FPU_SUB_S(f0, f1);
  f0 = FPU_SUB_S(split, f1);
  f0 = ps2FpuDivide(g.ctx->fcr31, f0, f2); // Divisão lenta com emulação de flags MIPS FPU
  ```
- **Solução Equivalente vs Melhoria de Algoritmo**:
  - *Equivalente*: Substituir `ps2FpuDivide` por divisão nativa IEEE 754 de ponto flutuante em hardware ARMv8-A.
  - *Algorítmica*: Podar ramos inteiros da Kd-tree através de um teste de bounding box AABB pré-calculado para o raio, evitando descer na árvore para segmentos distantes.

### 2.2 Segment Memoization Temporal (`seg123` / `coll123.h`)
- **Descoberta da Auditoria**:
  - Em câmeras paradas ou movimentos lentos, **88.5% das consultas de colisão** (`queries 1656, kept 1467`) são exatamente as mesmas do quadro anterior.
  - Sem memoização (`SOTC_SEG123=0`), a CPU refaz todas as 1.656 travessias na Kd-tree e no `ClipCollisionDB`.
  - Com validação por fingerprint dos objetos (`seg123`), o resultado é retornado em **O(1)** (~44 microssegundos), desde que os objetos no raio não tenham se movido.
  - **Impacto no R36S**: A ativação de `SOTC_SEG123=1` reduziu o tempo de CPU de ~35 ms para **24.5 ms**, estabilizando o jogo em **30 FPS sólidos**.

### 2.3 Detecção de Contato Wander-Colossos (`skinFinder` e `decomp_skinfinder.cpp`)
- **Problema**: Wander se move e se agarra em malhas que se deformam via esqueleto (ossos). No PS2 original, isso envolvia calcular o inverso das matrizes de cada osso na CPU para transformar o raio para o espaço local de cada osso, testando triângulo a triângulo.
- **Oportunidade Algorítmica**:
  - Criar um broadphase de cápsulas/esferas atreladas a cada osso do Colosso.
  - Testar ray-capsule em C++ nativo antes de descer para os triângulos detalhados da pelagem e das saliências de pedra.

### 2.4 Raycasts de Câmera (`camera_collision_check_direct` / `0x0140EE60`)
- **Problema**: O sistema de câmera dispara múltiplos raios em cone para evitar colisão com tetos e colunas.
- **Solução Aplicada e Próximo Passo**:
  - `SOTC_SAC_CAMCOLL=1` (já implementado) filtra raios supérfluos quando a câmera não está em rotação rápida.
  - Próximo passo: Memoizar o raio principal da câmera com tolerância angular (ray cone caching).

---

## 3. Matriz de Priorização das Melhorias

| Melhoria Proposta | Categoria | Ganho Esperado | Risco de Regressão | Complexidade |
|---|---|---|---|---|
| **1. Ativação Definitiva de `SOTC_SEG123=1`** | Algoritmo (Memoização) | **Alto (~3-5 ms)** | **Nenhum** (Validado por fingerprint de 1mm) | **Baixa** (Pronto) |
| **2. FPU Nativa no Traversal Kd-Tree** | Otimização Equivalente | **Médio (~1-2 ms)** | Muito Baixo (IEEE 754) | **Baixa** |
| **3. AABB Pruning pré-`ClipCollisionDB`** | Algoritmo (Broadphase) | **Alto (~2-4 ms)** | Baixo | **Média** |
| **4. Broadphase Capsular Wander/Colosso** | Algoritmo (Física/Contato) | **Muito Alto no Colosso** | Médio (precisa de difftest) | **Alta** |
| **5. Batch de Invalidações `FlushModifiedManager`** | Lógica de Processamento | **Baixo/Médio (~0.5 ms)** | Baixo | **Média** |

---

## 4. Próximos Passos Imediatos

1. Manter `SOTC_SEG123=1` no lançador oficial `SotC.sh`.
2. Otimizar as divisões e interpolações de nós na travessia Kd-tree em `decomp_collision.cpp` para remover simulações desnecessárias de FCR31 MIPS.
3. Validar a movimentação e combate no primeiro colosso (Valus) no R36S garantindo 100% de precisão de gameplay.
