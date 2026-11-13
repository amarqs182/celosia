# R36S worklist

Prioritize by measured impact and confidence. Do not treat historical TODOs as current diagnoses.

## P0 — Explain the moving-camera slowdown
- [x] Capture a repeatable scene with camera stationary.
- [x] Capture the same scene while moving the camera, with the same duration and settings.
- [x] Record frame-time percentiles, per-core/thread CPU, hot guest PCs/functions, draw calls, and available GPU timings.
- [x] Determine whether the cost is in EE recompilation, VU1 emulation/recompilation, renderer submission, GPU execution, synchronization, or presentation.
- [ ] Re-test after each isolated change on the R36S.

## P1 — Validate existing renderer optimizations
- [ ] Verify current `glSort87/glMergeDraws` behavior and actual draw-call counts in the target scene.
- [ ] Verify `meshoptimizer` integration, configuration, and measured benefit; do not reintegrate it.
- [ ] Measure fur-shell cost and confirm the dynamic four-shell cap is active where intended.
- [ ] Check shader variants and GLES state changes for redundant work.

## P2 — Targeted reverse engineering
- [ ] Start from measured hot PCs/functions, not broad decompilation.
- [ ] Use IDA Pro via IDA MCP for direct disassembly/decompiler inspection.
- [ ] Use REA MCP with its IDA provider for structured function/call/xref analysis.
- [ ] Confirm guest address mapping, module relocations, and function boundaries before attributing a hot PC.
- [ ] Validate any proposed optimization against gameplay invariants and the R36S workload.

## P3 — Stability and regression coverage
- [ ] Test scene transitions, camera movement, combat, and sustained play.
- [ ] Check memory use and long-session stability on the 1 GB device.
- [ ] Record build identifier, test scene, settings, and before/after metrics.

## Completion criteria
A performance item is complete only when the change is reproducible, measured on the R36S, and does not introduce visible or gameplay regressions. A short peak or average FPS reading is insufficient evidence for stable 60 FPS.
