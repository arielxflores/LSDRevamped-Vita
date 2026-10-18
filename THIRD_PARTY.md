# Third-party code and notices

The port as a whole is distributed under the GNU GPL v3 (see `LICENSE`). It contains or follows the work below.

## Derived from

| Project | Author | License | Used for |
| --- | --- | --- | --- |
| [LSD Revamped](https://github.com/figglewatts/LSDRevamped) | Sam Gibson (Figglewatts) | MIT | Game logic, menus and data conversion follow its Unity code; its Lua scripts are converted by the data tools |
| [libLSD](https://github.com/figglewatts/libLSD) | Sam Gibson | MIT | LBD/TMD/TIX/MOM/SEQ/VAB readers (`tools/lsdfmt.py`, `tools/mom.py`, `src/synth.c`) |
| [EZSynth](https://github.com/figglewatts/EZSynth) | Sam Gibson | MIT | Music sequencer and synthesiser (`src/synth.c`) |

## Included in `src/`

| Library | Author | License |
| --- | --- | --- |
| [pl_mpeg](https://github.com/phoboslab/pl_mpeg) (`pl_mpeg.h`) | Dominic Szablewski | MIT |
| [stb_vorbis, stb_image_write](https://github.com/nothings/stb) | Sean Barrett | MIT or public domain |
| [LZ4](https://github.com/lz4/lz4) (`lz4.c`, `lz4.h`; `tools/c/lz4hc.*`) | Yann Collet | BSD 2-Clause |
| [Lua 5.2.3](https://www.lua.org) (`src/lua/`) | Lua.org, PUC-Rio | MIT |
| [font8x8](https://github.com/dhepper/font8x8) (`font8x8_basic.h`) | Daniel Hepper | Public domain |

The license text of each is kept at the top of its source files.

## Linked (Vita build)

| Library | Author | License |
| --- | --- | --- |
| [vitaGL](https://github.com/Rinnegatamante/vitaGL) | Rinnegatamante | GPL v3 |
| [vitaShaRK](https://github.com/Rinnegatamante/vitaShaRK) | Rinnegatamante | LGPL v3 |
| [SceShaccCgExt](https://github.com/bythos14/SceShaccCgExt) | bythos | GPL v3 |
| [math-neon](https://github.com/andrepuschmann/math-neon) | Lachlan Tychsen-Smith | LGPL v3 |
| [VitaSDK](https://vitasdk.org) newlib, zlib and stubs | VitaSDK contributors | various open source |

## Game content

None is included in this repository or in the VPK. `tools/build_data.py` converts it from the LSD Revamped
repository on the user's computer. That content includes *LSD: Dream Emulator* (OutSide Directors Company /
Asmik Ace, 1998), the Revamped soundtrack (by the artists in the game's credits), and the Roboto (Apache 2.0) and
Fredoka One (SIL OFL 1.1) fonts, which LSD Revamped bundles under `LSDR/Licenses/`.

## MIT license text (LSD Revamped, libLSD, EZSynth)

```
The MIT License (MIT)

Copyright (c) 2016 Sam (LSD Revamped)
Copyright (c) 2017 Sam Gibson (libLSD)
Copyright (c) 2023 Sam Gibson (EZSynth)

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```
