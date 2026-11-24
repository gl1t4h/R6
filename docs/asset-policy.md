# Asset and input policy

The repository is source-only.

## Allowed in Git

- Original R6 source code
- Build scripts and tooling
- Documentation
- Format specifications written from independent analysis
- Hashes and non-sensitive structural metadata
- Tests using synthetic fixtures

## Not allowed in Git

- `.bin`, `.cue`, `.iso`, `.img`, `.chd`, `.pbp`, or other disc images
- Original PlayStation executables or BIOS files
- Extracted files from a game disc
- Game textures, models, sound, music, video, fonts, or scripts
- Save files or memory-card images
- Prebuilt native game binaries containing copyrighted game code or data

The local verification script reads the user's input in place and does not
copy it into the repository.
