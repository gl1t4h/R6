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

`tools/analyze-mips.py` now performs a conservative scan of a local PS-X EXE.
It reports instruction-class counts, direct in-image `JAL` targets, and likely
`JR $ra` return sites. It deliberately emits metadata rather than generated
game code, and its output should be kept outside Git.

This is an analysis aid, not yet a production disassembler or recompilation
backend. The next milestone is control-flow-aware function discovery with
delay-slot handling and explicit MIPS R3000A instruction decoding.
