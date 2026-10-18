# Building the VPK

## Requirements

- [VitaSDK](https://vitasdk.org)
- [vitaGL](https://github.com/Rinnegatamante/vitaGL), [vitaShaRK](https://github.com/Rinnegatamante/vitaShaRK),
  [SceShaccCgExt](https://github.com/bythos14/SceShaccCgExt) and
  [math-neon](https://github.com/andrepuschmann/math-neon), installed into VitaSDK
- Python 3 with Pillow (for the LiveArea images)

Release builds use vitaGL built like this (commit `2bdbe89`, with `patches/vitaGL-log-hook.patch`, which sends
vitaGL's error messages to the game's log):

```sh
git clone https://github.com/Rinnegatamante/vitaGL.git
cd vitaGL
git checkout 2bdbe89
git apply ../LSDRevamped-Vita/patches/vitaGL-log-hook.patch
make LOG_ERRORS=1 NO_SPLASHSCREEN=1 HAVE_SHARK_LOG=1 install
```

A vitaGL built without `LOG_ERRORS` works too; the game just won't log vitaGL's errors. The other libraries were
built at vitaShaRK `df24065`, SceShaccCgExt `1021923` and math-neon `c0ae64c`.

## Building

```sh
make
```

The VPK is written to `build/vita/LSDRevamped-Vita.vpk`. The LiveArea images are generated from `art/src/` by
`tools/livearea.py`.
