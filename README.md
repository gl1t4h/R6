# R6 — PlayStation 1 Native Recompilation

R6 is an experimental toolchain and research project for producing a **native
Windows build** from a user's own legally obtained PlayStation 1 game copy.

This is **not an emulator** and it is not intended to ship a PlayStation
emulation layer. The long-term goal is to translate/recompile the original
game code into a maintainable native Windows program while replacing
platform-specific services with native implementations.

The project is currently in the **initial investigation and tooling phase**.
There is not yet a playable native recompilation.

## Current status

R6 is a general PlayStation 1 bring-up/recompilation research project. It is not
tied to any single game title. Bring-up is validated against user-supplied disc
images that are never included in this repository.

| Area | Status | Notes |
| --- | --- | --- |
| Disc boot | In progress | Loads a PS-X EXE from a user-supplied BIN/CUE, executes it on the interpreter, and services kernel/IRQ paths used during early boot. |
| CD-ROM / DMA | In progress | DMA3 disc reads, CD command responses, and sector delivery are implemented enough for early file streaming; timing-sensitive loader behavior is still being debugged. |
| GPU / display | In progress | Software GPU handles common GP0/GP1 commands and presents a VRAM display window; guest scene submission depends on further CPU/GTE/timing coverage. |
| GTE / COP2 | In progress | RTPS/RTPT, NCLIP, AVSZ3/4, MVMVA and SQR compute with the documented saturation and FLAG behaviour, as do the lighting commands NCS, NCCS, NCDS and their triple forms NCT, NCCT and NCDT; the colour commands that work from existing IR (CC, CDP, DPCS, DPCT, INTPL, DCPL) are counted per opcode and named in the `r6-run` report. See `docs/gte-scope.md`. |
| Native player | In progress | `r6-player` opens a Win32 window and presents the translated display from a user-supplied local disc image. |

Progress is measured by bring-up milestones, not playable game content. On-screen
counters report interpreter/player state; a black or incomplete display usually
means the guest has not submitted a renderable scene yet.
## What is and is not included

This repository contains only original project code, scripts, documentation,
build files, and metadata needed to reproduce the work.

It does **not** contain:

- PlayStation disc images or ROMs
- Original game executables
- Extracted game data
- Textures, models, audio, video, fonts, or other game assets
- BIOS files
- User save files
- Copyrighted files copied from a game disc

Each user must provide their own legally obtained copy and must follow the law
where they live. The build and extraction tools are designed to read local
input files without copying those files into this repository.

## Native recompilation model

The intended pipeline is:

1. The user supplies a verified local copy of the supported game.
2. Local tooling identifies and analyzes the original executable and data
   formats.
3. The recompilation pipeline generates native C/C++ code or equivalent
   translation artifacts.
4. Native Windows platform services are implemented separately from the
   original PlayStation hardware interfaces.
5. The user builds the native executable locally.
6. Runtime game data is loaded from the user's own local copy or from a
   locally generated, user-owned build workspace.

The project will not distribute a prebuilt game executable or copyrighted
game data.

## Requirements

The initial development environment targets Windows:

- Git
- CMake 3.20 or newer
- A C++20-capable compiler, such as Visual Studio 2022 or LLVM/Clang
- PowerShell 5.1 or newer
- A legally obtained copy of the target game

The exact reverse-engineering and code-generation dependencies will be added
as the pipeline becomes concrete. We will document versions here rather than
silently relying on emulator software.

## Getting started

Clone the repository:

```powershell
git clone https://github.com/gl1t4h/R6.git
cd R6
```

Verify a local BIN/CUE input without copying it into the repository:

```powershell
.\tools\verify-disc.ps1 `
  -CuePath "C:\path\to\your\game.cue"
```

Write a local-only verification manifest outside the repository:

```powershell
.\tools\verify-disc.ps1 `
  -CuePath "C:\path\to\your\game.cue" `
  -ManifestPath "$env:USERPROFILE\R6-local\game-manifest.json"
```

The manifest contains hashes and structural metadata only. It does not copy or
embed disc contents.

The native build scaffold can be configured with:

```powershell
cmake -S . -B build
cmake --build build --config Release
```

The current scaffold is a development diagnostic, not a playable game.

Run your own local copy on the development runner:

```powershell
.\build\r6-run.exe "C:\path\to\your\game.cue" --steps 2000000
```

Use `build\Release\r6-run.exe` for a Visual Studio build. The runner loads the
boot executable out of your BIN/CUE, executes it on the native R3000A
interpreter, and reports the instructions, BIOS calls and GPU traffic it saw. It
is **not** a playable build: the GTE colour commands that work from existing IR
are still missing, and the loader has not submitted a scene.

Run the native checks (GPU decoder and BIOS services) and the analysis tests:

```powershell
ctest --test-dir build --output-on-failure
python -m unittest discover -s tests -v
```

To generate a local-only native translation prototype from an extracted PS-X
EXE:

```powershell
python .\tools\generate-native-stub.py `
  "$env:USERPROFILE\R6-local\game.exe" `
  "$env:USERPROFILE\R6-local\generated\game_translation.cpp"
```

The generated file must stay outside this repository. It supports only a small
instruction subset and deliberately stops on unsupported control flow; it is
not yet a playable recompilation.

## Disc formats

`r6-run` and `r6-player` read raw BIN/CUE images and CHD version 5 CD images. A
`.cue` sheet points at its `.bin` track and `resolve_bin()` follows that
pointer, so either front end accepts a `.cue`, a `.bin` or a `.chd` directly.

The CHD reader covers the containers `chdman` writes:

- uncompressed hunks and the 4-byte map produced by `-c none`
- `cdzl` (DEFLATE) and `cdlz` (LZMA) CD hunks
- both the plain and the Huffman-compressed hunk map
- back-references (SELF hunks) and 2- or 3-byte compressed lengths

`cdfl` hunks only hold CD audio frames, which the emulated CD controller never
requests, so a data track with audio tracks after it is fine. These are
rejected with a specific message instead of silent garbage:

- Zstandard hunks (`cdzs`)
- delta CHDs that need a parent image
- CHD versions 1 to 4

MAME 0.289 `chdman` output is the tested case. The fixtures under
`tests/fixtures` are built by `tests/make_chd_fixtures.py` from repeated ASCII
text (no game data), and every sector they expose is compared against the
original synthetic image. No retail disc has been run through this path yet.

If a CHD opens but a sector read fails, `r6-run --chd-info file.chd` prints the
header, the codecs and the first eight and last four map entries, with a count
of the hunks in between, without booting anything; the error itself names the
failing hunk. That output is enough to tell a map the reader misparsed from a
container it does not support.

If a file still cannot be read, expand it once with `chdman` (shipped with
MAME) and point the front ends at the `.cue` instead:

```powershell
chdman extractcd -i "$env:USERPROFILE\Downloads\yourgame.chd" `
  -o "$env:USERPROFILE\R6-local\game.cue" `
  -ob "$env:USERPROFILE\R6-local\game.bin"
```

Write the output outside the repository, as those paths do. The expanded image
is disc data: `.gitignore` already excludes `*.chd`, `*.cue` and `*.bin`, so it
cannot be committed by accident, but keeping it out of the tree avoids tools
picking it up. The only committed `.chd` files are the synthetic test fixtures
under `tests/fixtures`; the ignore rule carries an explicit exception for them.

## Development rules

- Never add a game image or extracted game files to Git.
- Never use a personal absolute path in committed source or documentation.
- Keep local input, generated output, and research notes under ignored paths.
- Validate `git status --short` before every commit.
- Do not claim a game is playable until a clean-machine build and runtime test
  have passed.
- Document any discovered game-version assumptions.

## Project layout

```text
.
├── docs/
│   ├── asset-policy.md
│   ├── game-status.md
│   ├── gte-scope.md
│   ├── irq-bringup.md
│   └── recomp-findings.md
├── include/
│   └── r6/
├── src/
├── tools/
│   ├── analyze-mips.py
│   ├── generate-native-stub.py
│   ├── inspect-ps1-disc.py
│   ├── mips_cfg.py
│   ├── mips_decode.py
│   ├── report-indirects.py
│   └── verify-disc.ps1
├── tests/
├── CMakeLists.txt
└── README.md
```

## Legal notice

R6 is an independent research and software project. It is not affiliated with
Sony, Activision, Neversoft, Marvel, or any other rights holder.

Game names, trademarks, executable code, audiovisual content, and other
copyrighted materials remain the property of their respective owners. This
repository does not grant permission to copy, distribute, or share those
materials.

Users are responsible for obtaining their own lawful copy, complying with
copyright and anti-circumvention laws applicable to them, and ensuring that
their use of the tools is lawful. This notice is informational and is not
legal advice.

## References and credits

Hardware behaviour here was checked against public documentation and two
independent emulators. Neither emulator is required to build or run R6, and
this tree contains no code from either project.

- **psx-spx** (<https://psx-spx.consoledev.net/>) is the primary reference
  for register maps, the root-counter clock table, and the kernel's
  exception frame.
- **DuckStation**, by stenzek and contributors
  (<https://github.com/stenzek/duckstation>), was used as a behavioural
  reference for root-counter clock sources, exception entry, and restoring
  the register file on `ReturnFromException`. DuckStation is licensed under
  Creative Commons BY-NC-ND 4.0, which forbids derivative works, so its
  source is consulted only to confirm documented hardware behaviour; it is
  never copied, translated, or vendored into this repository.
- **psxe**, by Allkern (Lisandro Alarcón)
  (<https://github.com/allkern/psxe>), was used as an independent second
  opinion on the same timer and exception behaviour. psxe is MIT licensed,
  copyright (c) 2024 Allkern (Lisandro Alarcón); its code may be reused
  with that notice, but none is included here.

The root-counter fixes and the register-file restore on `B(0x17)` were
written from those documented facts and from the failures observed in the
runner, not from either emulator's source, and no third-party source files
are compiled into R6.

## Completed games

None.

## Games in progress

| Game | Status | Next milestone |
| --- | --- | --- |
| Spider-Man (USA) | Native interpreter runs the guest, its vblank dispatcher, VSync callbacks and the CD loader (whole files stream); emulated-cycle timers, DMA interrupt/DMA4 completion and SPU transfer stubs are committed | Confirm the emulated-cycle timers remove the remaining `CdSync`/loader stalls on disc, then chase the first display/GPU scene and GTE geometry |
| Silent Hill (USA) | The interpreter opens the retail CHD directly, finds the ISO9660 volume, loads `SLUS_007.07` and runs 40M instructions of early init, printing the guest's own `CD_init` line; no guest CD sectors or GPU primitives yet, and the interrupt-delivery fix made after that run is not yet confirmed on disc | Rerun on disc to confirm `HookEntryInt` interrupt delivery and CD init reaching its first sector reads, then capture the first display/GPU scene |
| Air Hockey (USA) | The interpreter opens the retail CHD directly, finds the ISO9660 volume, loads `SLUS_014.67` and runs 40M instructions of early init, including four `B(0x35)` stdout `FileWrite` calls now serviced as TTY text; that run read no guest CD sectors, submitted no GPU primitives and delivered no interrupts (before the `HookEntryInt` fix) | Rerun on disc to confirm interrupt delivery, then follow CD init to its first sector reads and the first display/GPU scene |

## Contributing

Contributions should contain only original code, documentation, scripts, or
metadata that can legally be redistributed. Pull requests containing game
images, extracted assets, proprietary binaries, or generated copyrighted data
will be rejected.
