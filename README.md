# R6 — PlayStation 1 Native Recompilation

R6 is an experimental toolchain and research project for producing a **native
Windows build** from a user's own legally obtained PlayStation 1 game copy.

This is **not an emulator** and it is not intended to ship a PlayStation
emulation layer. The long-term goal is to translate/recompile the original
game code into a maintainable native Windows program while replacing
platform-specific services with native implementations.

The project is currently in the **initial investigation and tooling phase**.
There is not yet a playable native recompilation.

## Current game status

| Game | Region/version | Status | Progress |
| --- | --- | --- | --- |
| Spider-Man | USA PlayStation 1 release | Windows I/O backend | 85% - keyboard input and software audio/framebuffer sinks |

No games are currently completed.

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
│   └── recomp-findings.md
├── include/
│   └── r6/
├── src/
├── tools/
│   ├── analyze-mips.py
│   ├── generate-native-stub.py
│   ├── mips_decode.py
│   ├── inspect-ps1-disc.py
│   ├── report-indirects.py
│   └── verify-disc.ps1
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

## Completed games

None.

## Games in progress

| Game | Status | Next milestone |
| --- | --- | --- |
| Spider-Man (USA) | Windows I/O backend | Implement GPU command decoding and BIOS service behavior with tests |

## Contributing

Contributions should contain only original code, documentation, scripts, or
metadata that can legally be redistributed. Pull requests containing game
images, extracted assets, proprietary binaries, or generated copyrighted data
will be rejected.
