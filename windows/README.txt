LSD Revamped Vita - game data builder for Windows
=================================================

The PS Vita port of LSD Revamped (https://github.com/arielxflores/LSDRevamped-Vita) needs the
game's data, which can't be distributed with it. This tool makes it on your PC from LSD Revamped's
own repository (https://github.com/figglewatts/LSDRevamped).

1. Unzip this folder somewhere with a short path and about 1.5 GB of free space, for example
   C:\LSDR or your Desktop.
2. Double-click "Build LSD data.bat". It downloads about 450 MB and takes 10 to 30 minutes.
   If it stops (for example the connection drops), run it again: it continues where it was.
3. When it says Done, copy the "lsdr" folder it made to ux0:data/ on your Vita with VitaShell
   (USB or FTP), so that you have ux0:data/lsdr/dreams.pak.
4. Install LSDRevamped-Vita.vpk if you haven't yet, and start the game.

Windows may warn about running a .bat file downloaded from the internet: choose "More info",
then "Run anyway". The "work" folder can be deleted afterwards.

Included: Python 3.12 (python.org) with numpy, Pillow and lz4; FFmpeg (LGPL build, see
ffmpeg\SOURCE.txt); the Noto Sans CJK font (SIL Open Font License, see font\OFL.txt); and the
data tools from the LSDRevamped-Vita repository (GPL v3).
