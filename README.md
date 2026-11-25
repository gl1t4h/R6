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
| Native translation | Boot and loaded-image slices | `tools/generate-native-stub.py` emits one native function per basic block, and `r6-native` executes them through the dispatcher. The bootstrap runs 1,975 blocks while matching the interpreter's registers, HI/LO and 2 MiB RAM, then stops at `InitHeap`. A 130M-instruction RAM snapshot starts the resident main at `0x80032EE0`; the 4,361-block loaded slice matched 200,000 blocks with GPRs, HI/LO, COP0, hardware registers, GTE, scratchpad, BIOS bookkeeping and all RAM compared after every block. Native DMA2/3/4 transfers complete like the interpreter's. One native bus and one interpreter persist COP0, hardware, GTE/scratchpad and BIOS state across block boundaries, so the run advances to `0x800154CC` instead of resetting into the `0x80013130` wait. The startup trace does not reach the mode dispatcher at `0x80087360`; a targeted mode-3 checkpoint does, reading `0x8002AB60` and executing `0x80087448`. Generated code stays outside the repository. |
| Native player | In progress | `r6-player` is the development interpreter front end; it presents the reference display, not a recompiled build. |

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

To generate and run a local-only native translation slice, first extract the
boot executable from your own image, then generate one native function per
basic block:

```powershell
.\build\Release\r6-run.exe "C:\path\to\your\game.chd" `
  --extract-exe "$env:USERPROFILE\R6-local\game.exe"

python .\tools\generate-native-stub.py `
  "$env:USERPROFILE\R6-local\game.exe" `
  "$env:USERPROFILE\R6-local\generated\game_translation.cpp" `
  --max-functions 64
```

The generated file must stay outside this repository. Configure the native
target with its path and run it:

```powershell
cmake -S . -B build-native `
  -DR6_GENERATED_TRANSLATION="$env:USERPROFILE\R6-local\generated\game_translation.cpp"
cmake --build build-native --config Release --target r6-native
.\build-native\Release\r6-native.exe "C:\path\to\your\game.chd"
```

`r6-native` boots the same executable and executes the generated blocks,
checking GPRs, HI/LO, COP0 and all of RAM against the interpreter after every
block. The bootstrap slice stops at the first unhandled BIOS or hardware seam.

When the boot executable is only a loader, capture a local snapshot once the
main program is resident, then translate the resident entry:

```powershell
.\build\Release\r6-run.exe "C:\path\to\your\game.chd" `
  --steps 130000000 --vblank `
  --dump-snapshot "$env:USERPROFILE\R6-local\game.snap"

python .\tools\generate-native-stub.py `
  --snapshot "$env:USERPROFILE\R6-local\game.snap" `
  --entry 0x80032EE0 `
  --output "$env:USERPROFILE\R6-local\generated\loaded_translation.cpp" `
  --max-functions 512 --max-blocks 16384

.\build-native\Release\r6-native.exe `
  --snapshot "$env:USERPROFILE\R6-local\game.snap" `
  --entry 0x80032EE0 --blocks 200000
```

The snapshot is `R6S1` (GPRs, HI/LO, PC and 2 MiB RAM) and stays outside the
repository. Indirect entries such as BIOS stubs are added with
`--extra-entry ADDRESS`; the Silent Hill loaded slice emits 4,361 blocks and
matched 200,000 executed blocks. The runner keeps one native bus and one
oracle interpreter alive, so COP0, hardware, GTE/scratchpad and BIOS state
persist across block boundaries instead of resetting at each checkpoint. The
run advances to `0x800154CC`; the earlier per-block-reset 3,316-block slice
parked at `0x80013130`. The startup trace still does not reach the resident
mode dispatcher at `0x80087360`; with `[0x800B9FD0]=3` a targeted checkpoint
runs `0x80087360 -> 0x800873A4 -> 0x80087448`, and
`--watch-address 0x8002AB60` reports one read. This is still a verification
slice, not a playable build, and the run stops at the block budget.

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
| Silent Hill (USA) | The interpreter streams 1,402 CD sectors (LBA 102-1402), draws 217,138 primitives and 91,379 VRAM pixels, and stops with the loader paused at LBA 1403 and the display off. The boot executable's startup runs 1,975 generated native blocks to the `InitHeap` seam. A 130M-instruction RAM snapshot then starts the resident main at `0x80032EE0`, which calls the loaded startup at `0x8002E630`; 200,000 generated blocks matched the interpreter on GPRs, HI/LO, COP0, hardware registers, GTE, scratchpad, BIOS bookkeeping and all 2 MiB RAM. One persistent native bus/interpreter keeps COP0, hardware, GTE/scratchpad and BIOS state across blocks, and the run advances to `0x800154CC`; the old reset-per-block slice stopped at `0x80013130`. The startup trace does not reach the six-entry mode dispatcher at `0x8002AB54`; a targeted mode-3 checkpoint executes its `0x8002AB60` entry and `0x80087448` | Add the remaining device seams; native DMA execution and full hardware-register equality are now in place. The interpreter's post-load idle is an oracle question, not the native milestone |
| Air Hockey (USA) | The interpreter opens the retail CHD directly, finds the ISO9660 volume, loads `SLUS_014.67` and runs 40M instructions: 71 interrupts delivered with no pending vectors, `ResetGraph` and `CD_init` printed through `B(0x35)`, and the same correct `GetStat` INT3 handshake; still no guest CD sectors or GPU primitives, and the run ends in a TIM1/GPUSTAT wait loop | Run with a larger step budget to clear the init wait, then follow CD init to its first sector reads and the first display/GPU scene |

## Contributing

Contributions should contain only original code, documentation, scripts, or
metadata that can legally be redistributed. Pull requests containing game
images, extracted assets, proprietary binaries, or generated copyrighted data
will be rejected.
