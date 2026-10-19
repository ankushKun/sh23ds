# HOME Menu banner and icon

When sh23ds is selected on the HOME Menu, the top screen shows its banner and plays a short sound.
This folder holds that banner as an animated 3D model, like the ones official games have, and
everything it and the icon are made from, as code.

The scene is the sh23ds mark, nine rusted red squares on a plate of black iron, hanging in fog. It
sways slowly like a sign on a chain and pulses twice on a heartbeat while ash drifts down through
the scene, and the SILENT HILL 2 wordmark floats in front. The sound is the pocket radio's static
swelling and cutting out, two heartbeats, and a low A minor chord with a distant bell. The loop is
four seconds; the sound is 1.6.

> **Status: second console test pending.** The first build (v0.2.1-rc.1) crashed the HOME Menu
> the moment sh23ds was selected, before anything was drawn. Its model passed every check below;
> what it did not share with 3dsDL's banner was size: a 2.9 s jingle (bannertool stores the sound
> uncompressed, 512 KB) and a 256 × 256 atlas made a 551 KB banner against 3dsDL's 315 KB. The
> second build has 3dsDL's sound length and texture size and packs to 314 KB. The HOME Menu is the
> only thing that shows a CIA's banner, and Azahar does not draw them, so check every change on a
> console; `SH23DS_FLAT_BANNER=1 port/package.sh` packs the flat picture instead.

| File | What it is |
| --- | --- |
| `textures.py` | Draws the rust, the fog and the 48 × 48 icon (`port/meta/icon.png`, the website's mark in rusted metal). |
| `logo.html` | The wordmark, in the website's Libre Caslon Display. |
| `model.py` | Builds the scene as code: shapes, materials and rigid keyframe animation, written out as `banner.glb`. Refuses to write a model over the limits below. |
| `jingle.py` | Synthesises `jingle.wav`, an original cue timed to the animation. |
| `build-cgfx.py` | Converts `banner.glb` to `sh23ds.cgfx` with [pycgfx](https://github.com/skyfloogle/pycgfx) and fixes what pycgfx gets wrong (see the rules). |
| `render.html`, `render.sh` | Render the model in headless Chrome with three.js, framed by the banner camera, for the flat banner and for previews. |
| `atlas.png`, `banner.glb`, `sh23ds.cgfx`, `jingle.wav` | The generated files, committed so that `port/package.sh` needs none of these tools. |

## Rebuilding

```sh
port/meta/banner/render.sh   # textures, icon, wordmark, atlas, model, flat banner, preview frames
port/meta/banner/make.sh     # jingle, model and CGFX (needs Docker)
port/package.sh              # packs them into the CIA
```

`render.sh` needs Google Chrome and the repo's `.venv` (numpy, Pillow). `make.sh` runs pycgfx in a
digest-pinned Python container, with pycgfx's files fetched at a pinned commit and checked against
known hashes; pycgfx has no licence file, so it is never stored here. The `.3dsx` has no banner (the
Homebrew Launcher shows only the icon); its icon, name, description and author come from
`port/Makefile`, which `package.sh` reads for the CIA too.

## Rules the model follows

The pipeline is 3dsDL's (github.com/ankushKun/3dsDL, `resources/banner`). Its banner took five test
builds on a console: the first two crashed the HOME Menu as soon as the title was selected, the
third drew one frame and froze it, and the fourth worked. 3dsFM's banner then froze the HOME Menu
in three builds even after copying that banner's structure, so it was cut back to exactly what
3dsDL's banner does (3dsFM's swap builds showed the fault was in its model, not the sound or the
packaging). This model adds nothing that 3dsDL's banner hasn't shown to work:

- **Every mesh names its node.** pycgfx leaves the name blank, which crashed 3dsDL's first build.
  `build-cgfx.py` fills the names in.
- **Every name lookup resolves.** The HOME Menu finds bones, materials, textures and animations by
  name through a Patricia tree in each table; pycgfx can leave a tree stale, which crashed 3dsDL's
  second build (a null pointer read in `menu`). `build-cgfx.py` rebuilds the bone tree and walks
  every table's tree the way the console does, refusing to write a CGFX in which any name is lost.
- **Rigid animation only**, the way 3dsDL's banner moves: one unanimated rig node holding the
  placement, one animated bone under it (the mark, with 3dsDL's card motion, smaller: 13° of sway
  against 19°, a pulse of 0.99 to 1.05 against a squash of 0.90 to 1.08), and parts under it with no
  transform of their own. No skinning, no spin, no part rotating more than 3dsDL's do.
- **Nothing scales to (nearly) nothing.** 3dsDL's third build shrank parts to 0.001 and froze. The
  ash falls off the bottom of the screen and jumps back to the top between two keys 0.01 s apart,
  as 3dsDL's arrow does.
- **The wordmark is a Y-axis billboard with a hard-edged cut-out (alpha MASK)**, never a blend:
  blending makes pycgfx reorder bones, and a flat plane seen from behind vanishes.
- **One texture**, as 3dsDL's banner has, and the same size: `atlas.png`, 256 × 128, with the
  wordmark on the left and the rust and the fog down the right. Each region is inset by a texel so filtering never reaches its neighbour.
- **Within 3dsDL's counts**: 12 bones (14), 9 meshes (11), 5 materials (7), 1,384 triangles and
  1,448 vertices (2,000 and 2,048), and the CGFX at 145 KiB against 3dsDL's 151 KiB (512 KiB is
  the documented limit). `model.py` and `build-cgfx.py` check all of these.
- **The sound** is 44.1 kHz 16-bit stereo and 1.6 s, exactly 3dsDL's jingle. The documented limit
  is 3 seconds, but a 2.9 s sound is the likely cause of the first build's crash, so `jingle.py`
  refuses anything longer than 3dsDL's.
- **The packed banner stays under 330,000 bytes.** 3dsDL's and 3dsFM's working banners pack to
  315 and 324 KB; `port/package.sh` stops if `build/banner.bnr` is bigger.
- **The camera is fixed** at (0, 1, 44.786) looking down -z with a 30° field of view and a near plane
  at 26.5, so the plane z = 0 shows x from -20 to 20 and y from -11 to 13, one unit is ten pixels,
  and nothing may come closer than z = 18. The fog sits at z = -9, the mark at -1, the wordmark at
  2.4 and the nearest ash at 4.2.

## Testing on a console

1. Install the CIA with FBI and move the HOME Menu cursor to sh23ds. The jingle plays once, and the
   banner shows the mark swaying in fog with ash falling and the wordmark in front; it loops
   without a jump, and nothing is black or flickers.
2. With the 3D slider up, the wordmark and some ash stand in front of the screen and the fog sits
   far behind.
3. Move the cursor on and off sh23ds several times, and open and close the game. The HOME Menu
   never crashes or freezes.

If it crashes or freezes: hold POWER to switch off, start the console, open FBI without highlighting
sh23ds and install a flat banner build (`SH23DS_FLAT_BANNER=1 port/package.sh`) over it. If the HOME
Menu starts with sh23ds highlighted and crashes again, start once without the SD card so the cursor
moves elsewhere. To tell an animation problem from anything else, pack a still of the scene:

```sh
BANNER_STILL=0.8 port/meta/banner/make.sh                          # also writes still.cgfx
SH23DS_BANNER_CGFX=meta/banner/still.cgfx port/package.sh
```

If the still works and the animated banner doesn't, the fault is in a track; if both fail, it is in
the meshes, materials or texture.

## Credits

[pycgfx](https://github.com/skyfloogle/pycgfx) by skyfloogle converts the model, and
[CBC for YANBF](https://github.com/daaaaavia/CBC-for-YANBF)'s notes describe the fixes it needs.
