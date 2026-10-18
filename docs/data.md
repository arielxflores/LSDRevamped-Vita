# Building the game data

The Vita port reads its data from `ux0:data/lsdr/`:

| File | Contents |
| --- | --- |
| `dreams.pak`, `dreams2.pak`, `dreams3.pak` | Dreams, entities, scripts, sound banks, special days, menus |
| `movies1.pak` ... `movies6.pak` | In-game videos |
| `intro1.pak` ... `intro4.pak` | Intro videos |
| `music1.pak` ... `music5.pak` | The Revamped soundtrack |

All of it is made from the [LSD Revamped repository](https://github.com/figglewatts/LSDRevamped) by
`tools/build_data.py`, which downloads that repository at the commit the port was made against
(`8238ab7`), fetches the Git LFS files it needs straight from GitHub, converts them and packs them. That is
about 450 MB of downloads and 1.5 GB of disk space; the result is about 330 MB.

## On Windows (WSL)

1. Install WSL with Ubuntu if you don't have it: open PowerShell as administrator, run `wsl --install`,
   restart, and finish setting up Ubuntu.
2. In Ubuntu:

   ```sh
   sudo apt update
   sudo apt install git python3-numpy python3-pil ffmpeg gcc fonts-noto-cjk
   git clone https://github.com/arielxflores/LSDRevamped-Vita.git
   cd LSDRevamped-Vita
   python3 tools/build_data.py
   ```

3. When it says `done`, the files are in `lsdr-data/lsdr/`. From Windows the folder is at
   `\\wsl$\Ubuntu\home\<your user>\LSDRevamped-Vita\lsdr-data\lsdr` (or copy it out with
   `cp -r lsdr-data/lsdr /mnt/c/Users/<you>/Desktop/`).
4. Copy the `lsdr` folder to `ux0:data/` on the Vita with VitaShell (USB or FTP).

## On Linux

Install git, Python 3.8 or newer with numpy and Pillow, ffmpeg, gcc and the Noto Sans CJK font, then run
`python3 tools/build_data.py` in the repository.

## Options

```
python3 tools/build_data.py [--lsdr DIR] [--out DIR] [--jobs N]
```

- `--lsdr`: where LSD Revamped is downloaded to (default `LSDRevamped/`). An existing clone is reused and moved
  to the right commit.
- `--out`: work folder and result (default `lsdr-data/`). The downloaded LFS files are kept in
  `lsdr-data/lfs-cache/`, so running it again is much faster.
- `--jobs`: how many dreams to convert at once.

You can delete `LSDRevamped/` and `lsdr-data/` (except `lsdr-data/lsdr/`) afterwards.

## Troubleshooting

- `missing: ...`: install what it names (see the `apt install` line above).
- A download stops halfway: run the same command again; finished downloads are kept.
- The game says it could not load the dreams: check that the files are directly in `ux0:data/lsdr/`
  (not in `ux0:data/lsdr/lsdr/`), and look at `ux0:data/lsdr/log.txt`.
