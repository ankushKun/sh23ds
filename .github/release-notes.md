A native 3DS port of Silent Hill 2 (PC Director's Cut). These files contain no game data: you build that from your own copy of the PC game with the packer.

**Install**

1. Build the game data from your `Silent Hill 2` folder (it needs Docker):
   `docker run --rm -v "/path/to/Silent Hill 2:/game:ro" -v "$PWD/sd:/out" ghcr.io/ankushkun/sh23ds-packer`
2. Copy the `3ds` folder from `sd/` to the root of the SD card (about 1.3 GB).
3. Run DSP1 once if `sdmc:/3ds/dspfirm.cdc` doesn't exist yet.
4. Install `sh23ds.cia` with FBI (or put `sh23ds.3dsx` in `sdmc:/3ds/` for the Homebrew Launcher).

Runs on the New 3DS family, and on the original 3DS, 3DS XL and 2DS with a lighter profile that targets 30 fps (tested in Azahar's Old 3DS model, not yet on hardware). The full guide is on the [website](https://ankush.one/sh23ds/) and in the [README](https://github.com/ankushKun/sh23ds#install). Checksums are in `sh23ds.sha256`.
