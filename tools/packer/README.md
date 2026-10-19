# The packer

The packer reads your `Silent Hill 2` folder (the one with `sh2pc.exe` and `data/`) and converts every
room, texture, character, animation, event, message, picture, map, sound and movie into the formats the
port reads. It writes a `3ds` folder for the SD card and never changes the game folder.

The last file written is `files.txt`, the list of every file in the pack (the inspection extras left out).
If the game data on the card turns out incomplete, sh23ds compares the card against it and lists the
missing files on the touch screen.

There are two ways to run it. Both use the same converters (`tools/sh2conv`, driven by
`tools/make_pack.sh`).

## In the browser

Open the [packer page](https://ankush.one/sh23ds/packer.html), choose the game folder and save the zip it
makes. The page runs the converters on your computer with Pyodide and ffmpeg.wasm, so nothing is
uploaded or installed. It needs a desktop browser and about 1.5 GB of free disk space while it works;
Chrome and Edge can also write straight into a folder such as the SD card. How the page works and how it
is checked against the Docker packer is in [tools/packer-web/README.md](../packer-web/README.md).

## With Docker

The image holds the converters from this repository and nothing from the game.

```sh
# macOS and Linux
mkdir sd
docker run --rm -v "/path/to/Silent Hill 2:/game:ro" -v "$PWD/sd:/out" ghcr.io/ankushkun/sh23ds-packer
```

```powershell
# Windows (PowerShell)
mkdir sd
docker run --rm -v "C:\Games\Silent Hill 2:/game:ro" -v "${PWD}\sd:/out" ghcr.io/ankushkun/sh23ds-packer
```

When it finishes, `sd/3ds/sh23ds/` holds about 1.3 GB. Most of the time goes on textures; plan on an hour
or two, depending on how many CPU cores Docker gets.

- **Linux:** add `--user "$(id -u):$(id -g)"` after `--rm` so the files belong to you.
- **macOS with Colima:** only your home folder is shared with Docker, so keep the game folder and the
  output folder inside it.
- **If it stops partway,** run the same command again. Finished steps are skipped; `-e FRESH=1` starts
  over.
- **Inspection files:** `-e KEEP_EXTRAS=1` keeps PNG previews of every texture and the decoded events as
  JSON, about 0.6 GB more. The game doesn't need them.

To build the image yourself from a clone:

```sh
docker build -f tools/packer/Dockerfile -t sh23ds-packer .
docker run --rm -v "/path/to/Silent Hill 2:/game:ro" -v "$PWD/sd:/out" sh23ds-packer
```

## Without Docker

Install Python 3.11 or newer with `numpy`, `Pillow` and `pefile`, ffmpeg with the Bink decoder (Homebrew's
and most Linux builds have it) and devkitPro's `tex3ds`, then run the script directly. It writes the data
to `./pack`, which you copy to `sdmc:/3ds/sh23ds/`.

```sh
python3 -m venv .venv && .venv/bin/pip install numpy pillow pefile
tools/make_pack.sh "/path/to/Silent Hill 2"
```

## Enhanced Edition packs

If your game folder has the Enhanced Edition's `sh2e` folder (its Essential Files and the Image, FMV and
Audio Enhancement Packs), the packer uses those files in place of the originals and lists them in
`ee_used.txt`. The Enhanced Executable replaces `sh2pc.exe`, and the packer still needs the Director's Cut
exe, so put that back first. `-e SH2EE=0` builds from the original files only. Details are in
[EE_PACKS.md](../sh2conv/EE_PACKS.md).
