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
| Native translation | Boot and loaded-image slices | `tools/generate-native-stub.py` emits one native function per basic block, and `r6-native` executes them through the dispatcher. The bootstrap runs 1,975 blocks while matching the interpreter's registers, HI/LO and 2 MiB RAM, then stops at `InitHeap`. A 130M-instruction RAM snapshot starts the resident main at `0x80032EE0`; the 4,384-block event-aware loaded slice matched 200,000 blocks with GPRs, HI/LO, COP0, hardware registers, GTE, scratchpad, BIOS bookkeeping, an ordered VBlank/pad/DMA/interrupt event trace (326,004 events in the 200k VBlank run) and all RAM compared after every block. Native VBlank, pad and CD/DMA delivery share the interpreter's COP0 exception path, including HookEntryInt and ReturnFromException; DMA2/3/4 transfers complete like the interpreter's. One native bus and one interpreter persist COP0, hardware, GTE/scratchpad and BIOS state across block boundaries, so the run advances to `0x800154CC` instead of resetting into the `0x80013130` wait. The startup trace does not reach the mode dispatcher at `0x80087360`; a targeted mode-3 checkpoint does, reading `0x8002AB60` and executing `0x80087448`. Generated code stays outside the repository. |
| Native port | In progress | `r6-port` runs the generated blocks continuously and presents the display through an OpenGL 3.3 shader, with a live FPS counter in the title bar and an on-screen overlay. On Spider-Man it boots the retail `.chd` and runs its frame loop without a recompilation miss, but the guest never reaches its renderer: the loader now parses all eight directory records and streams 128 sectors, then parks in its stage consumer (`0x8002AD30`) waiting for a chunk that is never requested, so the only draws issued are three black fills and nine gouraud-shaded triangles clipped to a one-pixel drawing area and VRAM stays black. The directory extent bug that stopped the stream at six sectors was a translator `lwl` alignment error, fixed in `a8de1f9`. The translated code matches the interpreter for 3,000,000 blocks and the port now takes interrupts per instruction through resume entries inside blocks; the second directory read now seeks to sector 22 and the file search walks all eight records; the next blocker is the chunk request that the loader never issues after the 128-sector stream. | Find what should request the next chunk after LBA 128342, then capture the first scene |
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
  "$env:USERPROFILE\R6-local\generated\game_translation.cpp"
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
  --entry 0x80032EE0 --blocks 200000 --vblank
```

The snapshot is `R6S1` (GPRs, HI/LO, PC and 2 MiB RAM) and stays outside the
repository. Generated blocks poll VBlank, pad and interrupt events before every
instruction, and the oracle uses the matching event-step mode so the ordered
traces must agree. Indirect entries such as BIOS stubs are added with
`--extra-entry ADDRESS`; the Silent Hill loaded slice emits 4,384 event-aware blocks and
matched 200,000 executed blocks, including a 326,004-event ordered trace and its full-stream fingerprint. The runner
keeps one native bus and one oracle interpreter alive, so COP0, hardware, GTE/scratchpad and BIOS state
persist across block boundaries instead of resetting at each checkpoint. The
run advances to `0x800154CC`; the earlier per-block-reset 3,316-block slice
parked at `0x80013130`. The startup trace still does not reach the resident
mode dispatcher at `0x80087360`; with `[0x800B9FD0]=3` a targeted checkpoint
runs `0x80087360 -> 0x800873A4 -> 0x80087448`, and
`--watch-address 0x8002AB60` reports one read. This is still a verification
slice, not a playable build, and the run stops at the block budget.

### Full-image native build from your own .chd

`tools/generate-native-stub.py` emits hand-sized translation slices for
differential work. For a complete image, `tools/r6-recomp` reads the user's own
`.chd`/`.cue`/`.bin` directly and emits the same block-threaded unit, covering
every reachable block it can prove plus the code pointers and function
prologues that runtime-installed callbacks use.

With CMake:

```powershell
.\build\Release\r6-recomp.exe "C:\path\to\your\game.chd" `
  "$env:USERPROFILE\R6-local\games\translation.cpp"

cmake -S . -B build-native `
  -DR6_GENERATED_TRANSLATION="$env:USERPROFILE\R6-local\games\translation.cpp"
cmake --build build-native --config Release --target r6-native
cmake --build build-native --config Release --target r6-port
```

On Windows without CMake, `tools/build-port-msvc.cmd` does the same work with
the Visual Studio toolchain and needs nothing else installed:

```powershell
.\tools\build-port-msvc.cmd "C:\path\to\your\game.chd" "C:\R6-local\build"
C:\R6-local\build\r6-port.exe --disc "C:\path\to\your\game.chd"
```

It locates Visual Studio through `vswhere`, builds `r6-recomp`, translates the
disc into `translation.cpp`, compiles and links `r6-port.exe`, and copies
`shaders\` beside it. Every generated file stays in the output directory.

`r6-native` verifies the generated blocks against the interpreter, block by
block. `r6-port` is the playable front end: it boots the same executable, runs
the generated blocks continuously with real platform services, and presents the
GPU display through the OpenGL 3.3 shaders in `shaders\`, with a live frames
per second counter in the title bar and the top-left corner.

The port has no interpreter fallback. An address with no translated block
stops the run and is printed; add it to an `--extra-leaders` file and re-run
`r6-recomp` to translate it. The generated `.cpp` is derived from game code, so
it must stay outside the repository, as must any build directory that contains
it.

`r6-recomp` also accepts an extracted PS-X EXE. With `--no-seed-pointers` it
skips the table and prologue discovery and translates only the statically
reachable call graph.

## Disc formats

`r6-run` and `r6-player` read raw BIN/CUE images and CHD version 5 CD images. A
`.cue` sheet points at its `.bin` track and `resolve_bin()` follows that
pointer, so either front end accepts a `.cue`, a `.bin` or a `.chd` directly.
The player validates the ISO9660 descriptor before accepting an auto-discovered
image; a texture or other non-disc `.bin` is skipped instead of being opened as
a CD.

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
├── shaders/                 OpenGL 3.3 vertex and fragment shaders for r6-port
├── tools/
│   ├── analyze-mips.py
│   ├── generate-native-stub.py
│   ├── r6-recomp.cpp
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
| Spider-Man (USA) | The native port boots the retail `.chd`, executes 24,214 translated blocks (146,599 instructions) for 3,600 fields without a recompilation miss, and enables the display; the guest parks in its CD wait loop at `0x80084D80` with 2 GP0 commands and no VRAM pixels. `r6-native` matched 4,000 blocks against the interpreter (registers, HI/LO, COP0, hardware, GTE, scratchpad, BIOS, event trace, 2 MiB RAM). | Clear the CD streaming wait so the loader submits its first scene, then capture the first textured polygon |
| Silent Hill (USA) | The interpreter boots `SLUS_007.07` from a local `.cue`/`.bin` pair, streams 682 disc sectors, submits 217,138 primitives (91,379 VRAM pixels) and turns the display on, but every dumped frame is still black. The guest spins at `0x800C9AB0`, looping while the sound tick `0x80033548` returns 0. The tick returns 0 because its stream control block at `0x800B97D0` is cleared once by the BSS clear at `0x80010B0C` and never written again: the voice table pointer `0x800B97E4`, the active-stream byte `0x800B97E0` and the countdown `0x800B97DC` all stay zero, and all six store sites that could arm the countdown live inside the tick behind an active voice. The only voice-start call (`0x8002E94C`, from `0x800C9B8C`) sits in substate 3 of the very state machine that spins in substate 2, and the payload buffers it waits on are already filled by the CD driver copy loop at `0x80015C88`. A persistent native bus also matches the interpreter for 200,000 blocks on a RAM snapshot taken at 130M instructions. | Arm the stream path so the tick reports work and the state machine leaves substate 2, then capture the first non-black frame |
| Air Hockey (USA) | The interpreter opens the retail CHD directly, finds the ISO9660 volume, loads `SLUS_014.67` and runs 40M instructions: 71 interrupts delivered with no pending vectors, `ResetGraph` and `CD_init` printed through `B(0x35)`, and the same correct `GetStat` INT3 handshake; still no guest CD sectors or GPU primitives, and the run ends in a TIM1/GPUSTAT wait loop | Run with a larger step budget to clear the init wait, then follow CD init to its first sector reads and the first display/GPU scene |

## Contributing

Contributions should contain only original code, documentation, scripts, or
metadata that can legally be redistributed. Pull requests containing game
images, extracted assets, proprietary binaries, or generated copyrighted data
will be rejected.
