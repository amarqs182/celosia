# Reverse-engineering tools: IDA Pro + REA MCP

## Tool priority
1. **IDA Pro installed locally** is the primary binary-analysis engine, accessed through IDA MCP.
2. **REA MCP** is the higher-level analysis layer for structured function, call-graph, cross-reference, and evidence workflows. Configure/use its IDA provider so it analyzes the same IDA database.
3. **Ghidra** is a fallback only when IDA is unavailable, unsupported, or demonstrably produces incorrect results for the target.

REA and IDA are complementary, not competing decompilers. Avoid starting duplicate MCP servers or duplicate databases for the same target unless there is a concrete reason.

## Verify the live environment
Before analysis, discover the tools actually exposed by the active MCP configuration. Verify:
- IDA Pro installation, version, and license availability;
- the IDA MCP implementation and its supported tool catalog;
- REA MCP connection and configured provider;
- database path and input binary SHA-256;
- processor/module configuration, endianness, and image base.

Do not assume IDA or REA is connected merely because the software is installed. Do not invent tool names or call signatures. Do not change MCP configuration or install another server without authorization.

## Compatibility notes
- REA's IDA provider uses an existing IDA MCP server; REA does not install or license IDA Pro.
- The REA provider documentation describes compatibility with the legacy `mrexodia/ida-pro-mcp` 1.4.0 profile. Its attached legacy profile should not be assumed compatible with the renamed 2.x catalog.
- REA headless operation requires the database-supervisor API (including `idb_open`, `idb_list`, and `idb_close`). A version string alone does not prove compatibility.
- The official Hex-Rays IDA MCP is a separate implementation. Its current documented requirements include IDA 9.4+, idalib, and Python 3.11+. Do not assume REA supports it unless REA documentation and the live tool catalog confirm that.
- Use one selected IDA MCP implementation for a given analysis session. Avoid duplicate servers that create ambiguity over which database is active.

## Recommended workflow
1. Obtain a reproducible profile from the R36S, identifying the hot guest PC or native function.
2. Use REA to collect a focused function dossier: identity, callers, callees, xrefs, strings, and relevant data.
3. Open the same function in IDA and inspect the raw disassembly, decompiler output, control flow, and data types.
4. Confirm pseudocode against assembly, especially indirect calls, delay slots, jump tables, and register/stack conventions.
5. Record the binary hash, module, guest address, IDA address, function boundaries, and relevant xrefs.
6. Form one testable optimization hypothesis, implement the smallest change, and validate it on the R36S.

Do not decompile the entire game without a profiling-driven reason.

## PS2 module and address cautions
- Confirm MIPS/R5900 processor mode and endianness for each input.
- Distinguish raw file offsets, linked addresses, relocated module addresses, and runtime guest addresses.
- XFF modules are relocatable; raw XFF offsets are not automatically runtime addresses.
- Use `.cmd` linker maps, relocation metadata, module-loader behavior, and runtime captures to resolve addresses.
- Treat VU1 microcode as a separate execution domain from EE MIPS code.
- Be careful with `jalr`, delay slots, jump tables, function pointers, and indirect dispatch.

## Database and repository hygiene
Keep IDB/I64 databases and analysis caches local and outside Git. Do not commit proprietary game binaries, extracted assets, or databases containing them. Commit only project-authored notes or scripts that are appropriate for the repository.

## References
- REA: https://github.com/morluto/rea
- REA IDA provider: https://github.com/morluto/rea/blob/main/docs/providers/ida-provider.md
- Official Hex-Rays IDA MCP: https://github.com/HexRaysSA/ida-mcp
- Community IDA Pro MCP: https://github.com/mrexodia/ida-pro-mcp
