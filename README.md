# Digimon Rumble Arena 2 (USA): decomp / Windows port workspace

## This repository

It contains only original work: the extraction and analysis tools, the Ghidra scripts, the Windows platform layer (`decomp/port/`), headers, notes and the hand-made naming tables.
**It contains no game code, data or assets.** The disc image, extracted files, decompiled code and everything generated from them stay on your machine (see `.gitignore`, a whitelist).

To work on it you need your own copy of the game:
1. Extract the disc (`tools/xdvdfs_extract.py`).
2. Import `default.xbe` into Ghidra with the `ghidra-xbe` loader.
3. Run the scripts and tools as described below and in `decomp/README.md`.

GitHub Actions (`.github/workflows/ci.yml`) checks what the repository can check without the game:
- the Python tools compile;
- the shipped headers compile and the format struct sizes hold;
- the platform layer builds as a 32-bit library (`dra2_port.lib`, uploaded as an artifact).

## What the disc is

This is an **original Xbox** disc (XGD1, redump layout, game partition at `0x18300000`), not an Xbox 360 one.
That matters: the executable is 32-bit **x86** (Pentium III), the same instruction set Windows runs.
No PowerPC translation is needed (that is what Xbox 360 recompilers like XenonRecomp do).

| | |
|---|---|
| Executable | `extracted/default.xbe` (3.0 MB) |
| Title ID | `42410001` (Bandai) |
| Base / entry | `0x00010000` / `0x00127020` (retail XOR key) |
| Kernel thunk table | `0x00292980` (start of `.rdata`) |
| XDK | 5788 (XAPILIB, XBOXKRNL, LIBCMT, D3D8, D3DX8, XGRAPHC, DSOUND) |
| Compiler (expected for XDK 5788) | MSVC 7.1 (Visual Studio .NET 2003) |

The XBE sections `D3D`, `D3DX`, `XGRPH`, `DSOUND`, `XPP` and `DOLBY` are statically linked Microsoft XDK code, not game code.
Game code is in `.text` (about 2 MB), and that is the part to decompile.

## Layout

```
Digimon Rumble Arena 2 (USA).iso   original image (left untouched)
extracted/                         2,863 files, 1.58 GB, extracted from the game partition
  default.xbe                      game executable
  *.res, characters/, levels/ ...  asset containers ("res\n" magic, chunk tags: lua!, strg, indx, minf ...)
decomp/                            raw C decompilation of all 7,929 game functions + tracker (see decomp/README.md)
unpacked/                          every .res unpacked (86k files, 1.57 GB); each folder has a _manifest.json
analysis/
  lua_keys.tsv                     Lua config keys that also appear as strings in the XBE (510)
  lua_key_functions.tsv            functions that read those keys (122), with the keys each one reads
tools/
  xdvdfs_extract.py                XDVDFS extractor:   python -I tools/xdvdfs_extract.py <iso> <outdir>
  xbe_info.py                      XBE header dumper:  python -I tools/xbe_info.py extracted/default.xbe
  res_unpack.py                    .res unpacker:      python -I tools/res_unpack.py extracted unpacked
  surf_to_png.py / wave_to_wav.py  texture / sound converters
  model_export.py                  every mesh -> OBJ:  python -I tools/model_export.py extracted models
  level_export.py                  assembled levels:   python -I tools/level_export.py extracted/levels models
  anim_export.py                   rigged + animated characters (glTF): python -I tools/anim_export.py extracted/characters models/animated
  lua_bindings.py                  Lua key -> XBE string matcher (writes analysis/lua_keys.tsv)
  ghidra_scripts/FunctionStats.java  function counts per section
  ghidra_scripts/LabelLuaKeys.java   labels key strings, comments and renames the functions that read them
```

## .res container format

The full layout is documented at the top of `tools/res_unpack.py`. In short:
- A 0x1000-byte header holds the offsets and sizes of two data sections, the table of contents (TOC) and the `indx` chunk, plus a list of chunk types.
- Each TOC entry is 20 bytes: tag, offset (the low 2 bits pick the section), size, and relocation info.
- `indx` maps names stored in `strg` to entries, through pointers relative to each pointer's own address.
- All 2,861 files unpack: 83,377 entries, 40,275 of them named.
  The unnamed ones are sub-resources (debug data, render meshes, textures) that `indx` doesn't list.

| tag | count | guess (not yet verified) |
|---|---|---|
| `surf` | 15,863 | **texture, decoded**: DXT1/DXT3, converted to PNG in `textures/` |
| `wave` | 12,351 | **sound effect/voice, decoded**: Xbox ADPCM, converted to 16-bit `.wav` in `sounds/` |
| `rdms` | 15,222 | **static mesh, decoded**: props, effects and level geometry, exported to OBJ in `models/` |
| `banm` | 14,298 | bone animation (named by move, e.g. `omnimon/block.banm`) |
| `bmsh` | 1,028 | **skinned character mesh, decoded**: exported to OBJ in `models/` |
| `node` / `ndbg` | 6,784 each | scene node and its debug data |
| `gshd` | 8,191 | shader |
| `tern` | 2,283 | terrain/collision |
| `body` | 370 | character physics body |
| `lua!` | 140 | Lua 4 script, written out as plain `.lua` |
| `musc` + `mdat` | 16 | streamed music header and 16-bit PCM data, written out as playable `.wav` |

### surf (textures)

Convert with `python -I tools/surf_to_png.py unpacked textures`. The converter is pure Python and needs no extra packages.
- Header: format byte at 0x08 (`0x0A` = DXT1, `0x0B` = DXT3; no other values occur), frame count at 0x09, mip count at 0x0B, width/height at 0x0C.
- Then frames × mips pointers, stored frame by frame, each relative to its own address.
- The data is plain S3TC, not swizzled (Xbox swizzles only uncompressed textures).
- Only mip 0 is exported. Animated textures (menu animations, effects) are written as `name_f00.png`, `name_f01.png`, ...

### wave (sound effects and voices)

Convert with `python -I tools/wave_to_wav.py unpacked sounds`.
- Header: a pointer to a WAVEFORMATEX at 0x20, the data size at 0x0C, and a data pointer at 0x10.
  All of these sounds are Xbox ADPCM (format tag `0x69`) at about 44 kHz: 12,337 mono and 14 stereo.
- Each 36-byte block per channel holds a header (s16 sample, u8 step index) plus 64 four-bit IMA codes.
  The header sample is a real sample, so each block decodes to 65 samples. Without it, every block boundary shows a double-sized step.
- Stereo interleaves the codes per channel in 4-byte words.
- 18 entries (`loc/*/endgame/characters/can/*`) have size 0. They are written as empty WAVs.

### bmsh / rdms (models)

Export with `python -I tools/model_export.py extracted models`. Run `surf_to_png.py` first, because the `.mtl` files link to `textures/`.
- The exporter reads the `.res` files directly, because materials and textures are reached through pointers across resources.
- Both mesh types share one submesh descriptor:
  - +0x18: push-buffer size
  - +0x24: relative pointer to the push buffer
  - +0x28: relative pointer to the vertex data
- Triangles come from the raw **NV2A push buffer**: `SET_BEGIN_END` (0x17FC) plus `ARRAY_ELEMENT16/32`. Almost all of it is triangle strips with degenerate joins.
- `bmsh`: groups → submeshes, each with a bone palette.
  - Vertices are 60 bytes: position, 4 bone weights, normal, UV, 8 zero bytes, 4 palette indices.
  - A character has 3 LODs (Agumon: 2,917 / 1,619 / 626 triangles). The last one is untextured.
- `rdms`: one submesh. Vertices are 44 bytes: position, normal, ARGB color, UV, 8 zero bytes.
- `gshd` material: texture count at +0x58, texture pointers at +0x60 + 0x78·i.
- **Axes:** the game data is **right-handed, Z-up** (it comes from Maya).
  - The OBJ files are converted to Y-up with `(x, y, z) → (x, z, −y)`, keeping the stored winding. V is flipped.
  - This is confirmed by rendering: Agumon stands upright, and the shop signs in Old West Town read left to right.
  - An earlier export mirrored Z on the assumption of D3D left-handedness. That was wrong, and it's why characters appeared to lie down.
- Result: 16,250 OBJs (1,028 `bmsh`, 15,222 `rdms`). All texture links resolve.

Rigged and animated characters are exported separately, as glTF (see below).

### body + banm (skeletons and animation) → glTF

Export with `python -I tools/anim_export.py extracted/characters models/animated`. It needs `textures/`.
It writes one `.gltf` + `.bin` per character (the English variant): LOD0 skinned mesh, skeleton, and every animation, resampled to 30 fps.
- **`body` (skeleton):** bone array at +0x04, 0xA0 bytes per bone.
  - Name, parent (s16 at +0x08), quaternions rotate (+0x10) / jointOrient (+0x20) / rotateAxis (+0x30), scale, translate (+0x50), inverse bind matrix (+0x60).
  - Local = **T × jointOrient × rotate × rotateAxis × S**. This reproduces all stored inverse bind matrices exactly.
  - It also lists the 3 LOD meshes.
- **Skinning:** each vertex has 4 weights and 4 u8 slots. Slots come in **banks of 13** (one reserved slot + 12 bones), so palette index = `slot − 1 − slot // 13`.
  - Verified by mesh stretch in an animated pose: Agumon's mean edge stretch is 3.5% with this mapping and 23% with the naive one.
- **`banm` (animation):** a list of tracks and a duration in seconds.
  - Each track: bone name, key count *n*, a flags word, *n* u16 key times (1/60 s ticks; the first segment starts at 0), then **3n+1 cubic Bézier control points**.
  - Flags bits 0–3: which of x/y/z/w are stored. Bits 12–15: channel.
    - 0x0 = rotate quaternion (/32000, renormalized; a missing w is rebuilt from unit length).
    - 0x8 = translate (/1024; checked against bind translations, 2,578 exact matches against 231 for /1000).
    - 0x4 = scale (/256).
  - Animated rotations replace `rotate` inside the joint formula above.
- Checked by skinning poses and rendering them: Agumon idle, fast attack and run, and Agunimon's idle fighting stance.

Not exported:
- Animation events (the per-animation event list, probably sound/FX triggers).
- LOD1/LOD2 meshes in the glTF; they're still available as OBJ.
- The material reflection layer. A DXT3 base texture is only alpha-masked when the material has no second layer, because otherwise its alpha is a reflection mask.

### Assembled levels (levl + node)

Assemble with `python -I tools/level_export.py extracted/levels models` (needs `textures/`). It writes one OBJ per level to `models/levels/`.
- **`levl`:**
  - +0x00 points to the collision data (`tern`).
  - +0x04 points to the root scene `node`.
  - Its other relocated pointers are static meshes that are already in world space.
- **`node`:** a Maya scene. +0x04 points to an object list. Each object has:
  - a type string, a name string, and an attribute block;
  - its **parent index at +0x1C** (−1 = root).
- **`transform`:**
  - Attribute block: visibility, translate, rotate (radians, XYZ order), scale.
  - Inline at +0x20: scale pivot, scale pivot translate, rotate pivot, rotate pivot translate.
  - Matrices follow Maya's formula. Visibility 0 hides the subtree.
- **`platform`:** +0x24 points to a geometry record, whose `rdms` pointer is at +0x5C or +0x60. Platforms that point to `tern` are collision only and are skipped.
- **`effect`:** +0x28 names another node in the same `.res`. That node is instanced recursively under the effect's transform.
- The relocation list of each TOC entry gives the offsets of its cross-resource pointers. Name and type strings are not relocated.

All 11 arenas assemble. Ice Palace, Old West Town and Wharf were checked by rendering them; they're complete and coherent.

Limitations:
- **Static initial state only.** Moving parts are placed at their rest pose. Animation curves, sequencers, timers and expressions are not evaluated. Toy Town in particular is mostly animated props.
- **Blend modes are lost.** OBJ/MTL can't express additive blending, so glow, energy and snow cards show as solid. Examples: the falling-snow panes in Ice Palace and the energy columns in the final boss arena.
- **Distant scenery is included.** Sky and backdrop meshes extend hundreds to thousands of units out.

The Lua scripts are almost all **data tables** (`characterConfig`, `menu`, `sounds`, `anglecam`, `boss_level` ...).
The C++ engine reads them by key name, which is how `LabelLuaKeys.java` finds the reader functions.
For example, `FUN_000fe720` reads 82 attack/knockback keys and `FUN_001053d0` reads the AI keys.

Useful find: `lua.res` and `characterlua.res` contain **plain-text Lua source** (character logic and a C++/Lua binding layer, `CPPToLua`).
A large part of the gameplay logic is therefore readable without decompiling it.

## Plan

1. **Static analysis.** Load `default.xbe` in Ghidra with the `ghidra-xbe` loader.
   The loader maps the sections, resolves kernel imports and runs XDK library signatures, so the D3D8, DSOUND and LIBCMT functions get named automatically.
2. **Asset tooling.** Reverse the `.res` chunk format (header + `indx`/`strg` tables) and write an unpacker, starting with the Lua containers.
3. **Decompilation.** Choose one of two routes:
   - *Matching decomp:* rebuild functions in C++ with MSVC 7.1 and check them byte for byte with `objdiff`. This is the most accurate route, but also the slowest.
   - *Functional decomp:* clean up the Ghidra output per function until it compiles and runs. This is faster, but there is no guarantee it behaves exactly like the original.
4. **Windows port.** Replace the platform layer:
   - Kernel/XAPI calls become Win32.
   - D3D8 (including the Xbox-only NV2A extensions and push-buffer usage) goes to D3D9/D3D11, or to SDL plus a modern renderer.
   - DSOUND goes to XAudio2 or SDL audio.
   - XInput: the Xbox gamepad API maps closely to the PC one.
   - Build with a modern MSVC or clang-cl once the code no longer has to match the original.

Shortcut for just *playing* on PC: xemu (low-level emulation) or Cxbx-Reloaded (high-level emulation) can run the extracted files as they are.

## Installed tools (`C:\Users\Veo\Tools`)

- Ghidra 12.1.4 (`ghidra_12.1.4_PUBLIC`), SHA-256 verified. It uses the JDK below through `JAVA_HOME_OVERRIDE` in `support/launch.properties`.
- Eclipse Temurin JDK 21.0.12.1 (`jdk-21.0.12.1+1`), SHA-256 verified.
- `ghidra-xbe` loader build 202610161023 for Ghidra 12.1.4, installed in `Ghidra/Extensions`.

Ghidra project: `ghidra/DigimonRumbleArena2.gpr`. Open it with `ghidraRun.bat`.
It is already imported and auto-analyzed, which found 9,865 functions:

| block | functions | named by XDK signatures |
|---|---|---|
| .text | 7,929 | 341 |
| D3D / D3DX | 389 | 183 |
| DSOUND | 614 | 197 |
| XGRPH / XPP | 933 | 32 |

Gotcha: Ghidra's `.bat` launchers break on the `(USA)` in this folder's path.
For headless runs, use the short path `C:\Users\Veo\DOWNLO~1\DIGIMO~2`, for example:

```
analyzeHeadless.bat C:\Users\Veo\DOWNLO~1\DIGIMO~2\ghidra DigimonRumbleArena2 -process default.xbe -noanalysis -scriptPath C:\Users\Veo\DOWNLO~1\DIGIMO~2\tools\ghidra_scripts -postScript FunctionStats.java
```

- LLVM/clang 23.1.4 (`clang+llvm-23.1.4-x86_64-pc-windows-msvc`), portable. Its SHA-256 matched the release attestation. Used by `tools/compile_check.py` (see decomp/README.md).

- Git for Windows 2.56.0.2, portable (`PortableGit`), SHA-256 and signature verified. No admin rights needed.

- llvm-mingw 20261006 (UCRT, x86_64 host), portable. Its SHA-256 was checked against the release metadata. It links the `.exe` without Visual Studio: clang, lld and the MinGW-w64 headers, import libraries and C runtime.

## Tools not needed after all

- Visual Studio Build Tools: llvm-mingw covers linking. One install attempt was cancelled at the admin prompt, and that's fine.
- Optional: MSVC 7.1 toolchain and objdiff, needed only for a matching decomp
