# GTE (COP2) scope

The interpreter used to count COP2 commands without computing anything, so every
vertex a game submitted arrived degenerate. The coordinate path and the
single-vector lighting path now compute. This records what runs, what still does
not, and in which order the rest makes sense.

Sources of truth are psx-spx, "Geometry Transformation Engine (GTE)" for
register layout, saturation and command semantics. No game data is used to test
any of it.

## Implemented

| Command | Opcode | What it gives a renderer |
| --- | --- | --- |
| RTPS | `01h` | Rotate, translate, project one vertex into the SXY/SZ FIFOs |
| RTPT | `30h` | The same for V0, V1 and V2, which is what a polygon uses |
| NCLIP | `06h` | Winding order, so back faces can be dropped |
| AVSZ3 | `2Dh` | Triangle average Z for the ordering table |
| AVSZ4 | `2Eh` | Quad average Z |
| MVMVA | `12h` | General matrix x vector, used for lighting and custom maths |
| SQR | `28h` | Squared vector length pieces |
| NCS | `1Eh` | Light + colour matrix into IR, leaving the colour FIFO alone |
| NCCS | `1Bh` | NCS plus the primary-colour multiply and a colour-FIFO entry |
| NCDS | `13h` | NCCS plus depth-cue interpolation towards the far colour |
| NCT | `20h` | NCS for V0, V1 and V2; IR only, no colour-FIFO entry |
| NCCT | `3Fh` | NCCS per vertex, so three colour-FIFO entries |
| NCDT | `16h` | NCDS per vertex, with the depth cue applied to each |

Supporting behaviour, all covered by tests:

- The full data (`cop2r0-31`) and control (`cop2r32-63`) register files, with
  the documented sign extension on reads, the `SXYP` mirror with move-on-write,
  the `IRGB`/`ORGB` 5:5:5 conversion, and `LZCR` leading-bit counts.
- `sf` (shift fraction) and `lm` (IR saturation range) decoding per command.
- `FLAG` semantics: cleared at the start of every command, saturation bits set
  by the documented helpers, and bit 31 derived from bits 30-23 and 18-13. Note
  that the summary deliberately excludes bit 22 (IR3) and the colour bits.
- COP2 dispatch: `mfc2`/`cfc2`/`mtc2`/`ctc2`, `lwc2`/`swc2`, and `BC2F`/`BC2T`
  (whose condition is always false, so `BC2F` never branches and `BC2T` always
  does).

## Not implemented

- **Colour from existing values**: CC, CDP, DCPL, DPCS, DPCT, INTPL. They take
  IR or the colour FIFO as input rather than a normal vector, which is why they
  are separate from the lighting path above.

  All of these are accepted and counted per opcode so a run continues and can
  say what is missing: `r6-run`'s GTE line lists the four busiest unsupported
  opcodes as opcode, command name and count, then how many further opcodes were
  seen, for example `GTE: 1546 commands, 1546 unsupported (0x14 CDP x1200, 0x10
  DPCS x300, 0x1c CC x40, 0x11 INTPL x5, +1 more)`. Only totals per command are
  kept; there is no per-call trace.
- **OP, GPF, GPL**: cross product and general interpolation. Small, and only
  needed by code that does its own vector maths.
- **MVMVA `mx=3`**: hardware returns a garbage matrix for that selector; the
  rotation matrix is used instead until something depends on it.
- **Exact division**: RTPS/RTPT use the documented `((H*20000h/SZ3)+1)/2`, not
  the hardware's unsigned Newton-Raphson approximation, so results can differ by
  a unit in rare cases near the saturating limit.
- **GTE load-delay slots** (1 instruction for `mfc2`/`cfc2`, 2 for `lwc2`) and
  the `LZCS` store delay. Titles known to care are rare (Tekken 2 is the usual
  example); this needs verifying against a real run before it is modelled.

## Verification

`tests/test_gte.cpp`, registered with CTest through `r6-tests`:

- identity RTPS puts the vertex on the screen offset and reports SZ3;
- the rotation matrix doubles as a 3.12 scale factor;
- matrix overflow saturates IR1 and sets the IR1 and summary flags;
- a vertex at or behind the camera saturates SZ3 to zero and raises the divide
  overflow flag, and the projected X clamps to `+3FFh`;
- RTPT leaves the SXY and SZ FIFOs in vertex order;
- NCLIP's sign follows winding and is zero for collinear vertices;
- AVSZ3/AVSZ4 scale correctly and saturate OTZ;
- MVMVA adds the translation vector, or the background colour when `cv=1`;
- SQR shifts or saturates, with and without `sf`;
- register reads sign-extend, `SXYP` mirrors, `IRGB` expands, `LZCR` counts;
- `FLAG` resets per command, and an unknown opcode is reported rather than
  silently accepted, with per-opcode totals and a named summary;
- NCS turns a normal into IR and leaves the colour FIFO untouched, while the
  background colour is added in the second matrix pass;
- NCCS at neutral primary colour and neutral light is the identity, and its FIFO
  entries keep the RGBC code byte;
- the colour FIFO shifts in order and saturates per channel with the colour
  flags;
- NCDS with IR0 = 0 leaves the lit colour alone, and with IR0 = 1.0 replaces it
  with the far colour, which is the check that the cue runs on the pre-shift
  MAC;
- the triple forms really iterate: NCT leaves IR on the last vertex and the
  FIFO untouched, NCCT pushes one entry per vertex in V0/V1/V2 order, and NCDT
  applies the cue to each;
- the interpreter itself dispatches `rtps`, then reads `SXY2` through `mfc2`.

Run them with `ctest --test-dir build --output-on-failure`.

## Order for the rest

1. `CC`, `CDP`: colour from the IR values a preceding NCS or NCT produced.
2. `DPCS`, `DPCT`, `INTPL`, `DCPL`: the remaining colour interpolation forms.
3. `OP`, `GPF`, `GPL`: cross product and general interpolation.
4. UNR division and load delays only if a real run shows artifacts.
