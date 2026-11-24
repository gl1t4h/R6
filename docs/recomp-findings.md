# Recompilation findings

This document records reproducible technical findings that do not contain game
assets or copyrighted binaries.

## 2026-11-24 — Spider-Man (USA) disc layout

The user's local BIN/CUE was inspected without copying it into this repository.

- The BIN is a raw **Mode2/2352** track.
- An ISO9660 primary volume descriptor is present at logical sector 16.
- The root directory contains:
  - `SLUS_008.75;1`
  - `SYSTEM.CNF;1`
  - `CD.HED;1`
  - `CD.WAD;1`
  - `CINEMAS.XA;1`
  - `CINEMAS` directory
- The main executable has a `PS-X EXE` header.
- The executable's reported initial program counter is `0x8008739C`.
- Its reported load address is `0x80010000`.
- Its reported payload size is `0x000B6800` bytes.

These values are metadata for identifying the user's input and are not a
replacement for the executable or any other game data.

## What this means for the native recompilation

The first implementation milestone should be an input-analysis pipeline, not
an emulator:

1. Parse the local ISO9660 filesystem from the raw BIN.
2. Locate and validate the PS-X EXE header.
3. Make a local-only analysis copy of the executable.
4. Decode MIPS R3000A instructions and build a symbol/function database.
5. Generate native C/C++ translation units from the analyzed code.
6. Implement native Windows replacements for PlayStation platform services.
7. Keep game data local and load it only from the user's own workspace.

The repository currently implements steps 1 and 2 in
`tools/inspect-ps1-disc.py`. Steps 3–7 are not complete.

## First-pass MIPS analysis

`tools/analyze-mips.py` now performs a conservative control-flow walk from the
entry point and statically discovered in-image `JAL` targets. It records basic
blocks, branch edges, delay-slot instructions, direct calls, indirect calls,
and return sites. It deliberately emits metadata rather than generated game
code, and its output should be kept outside Git.

The walk is intentionally bounded (`32` function candidates and `5,000`
instructions per function by default) so a first pass cannot spend excessive
time treating data as code. These limits are configurable for later targeted
analysis.

This is an analysis aid, not yet a production disassembler or recompilation
backend. The next milestone is explicit MIPS R3000A instruction decoding and
validation against synthetic control-flow fixtures.

The first decoder and synthetic tests are now in `tools/mips_decode.py` and
`tests/test_mips_decode.py`. They cover common arithmetic, memory, branch,
jump, call, return, and delay-slot forms. The decoder is intentionally
dependency-free so it can become part of the future translation pipeline.

## Native translation prototype

`tools/generate-native-stub.py` now consumes a local PS-X EXE and emits a
local-only C++ translation prototype outside the repository. It currently
translates a small safe subset (`nop`, selected arithmetic/immediate
instructions, `lui`, `ori`, and returns) and fails loudly at unsupported
instructions. This proves the decoder-to-C++ pipeline without pretending that
the game is already playable.

The generated prototype now accepts `r6::Memory&` and lowers common
little-endian `lb`, `lh`, `lw`, `lbu`, `lhu`, `sb`, `sh`, and `sw` operations.
The memory object is deliberately bounds-checked so invalid address mappings
fail during development instead of silently corrupting the host process.

Control-flow lowering now models the MIPS delay slot before returning the next
program counter for direct jumps, calls, branches, `jr`, and `jalr`. The
generated prototype uses a `PlatformServices` boundary so controller, audio,
and video replacements can be implemented without embedding PlayStation
hardware code into the translator.

The runtime now has a `FunctionDispatcher` that maps translated PlayStation
addresses to native C++ function pointers. Generated translation units expose a
registration function and return the next PC after each translated block.
Execution is bounded by a step budget while the function graph is incomplete,
preventing accidental infinite loops during development.

The local generator now uses `tools/mips_cfg.py` to walk from the entry point,
follow direct calls and control-flow edges, emit a bounded native function
candidate for each discovered function, and register them with the dispatcher.
Indirect calls and jump tables are left unresolved rather than treating
arbitrary data as code.

The CFG walker now tracks a small constant register state through `lui`,
`ori`, `addiu`, `addu`, and `subu`. If that proves the target of an indirect
`jalr` or `jr` lies inside the executable image, the target is followed;
otherwise it remains unresolved. This deliberately favors incomplete analysis
over false code discovery.
