# Testing

## Linux build

`Makefile.linux` builds the same game code for Linux, rendering off-screen with EGL and OpenGL ES 2 (Mesa's
llvmpipe is enough). It doesn't open a window: it reads its input from a script and writes a log and
screenshots, which makes it useful for checking changes without a Vita.

```sh
sudo apt install gcc pkg-config libegl-dev libgles-dev zlib1g-dev
make -f Makefile.linux
build/lsdr-linux --data lsdr-data/data --script tests/route.txt --out out
```

`--data` takes the converted data folder made by `tools/build_data.py` (or `--pak` a `dreams.pak`).

## Scripts

A script is a list of lines, one per step:

| Line | Meaning |
| --- | --- |
| `wait N` | N frames with no input |
| `hold N cross up lx=0.5 tx=480 ty=272` | N frames holding buttons, stick values or a touch |
| `cmd TEXT` | a test command (see `game_command` in `src/game.c` and `ds_command` in `src/dreamsys.c`) |
| `shot NAME` | save a screenshot of the next frame |
| `log TEXT` | write a line to the log |

`tests/route.txt` is the route used to test the port on hardware: every dream, special days, videos, the menus,
control schemes and long random walks, with timings logged.

## On the Vita

Copy a script to `ux0:data/lsdr/bench.txt` and start the game: it runs the script once (and deletes it), writing
the log to `ux0:data/lsdr/log.txt` and screenshots to `ux0:data/lsdr/shots/`.
