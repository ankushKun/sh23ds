# HOME Menu banner and icon

When sh23ds is selected on the HOME Menu, the top screen shows its banner and plays a short sound.
This folder holds that banner as an animated 3D model, like the ones official games have, and
everything it and the icon are made from, as code.

The scene is the sh23ds mark, nine red squares eaten by rust on a plate of black iron, hanging in
grey fog. It sways slowly like a sign on a chain and pulses twice on a heartbeat while ash drifts
down through the scene, and the SILENT HILL 2 wordmark floats in front. The sound is the pocket
radio's static swelling and cutting out, two heartbeats, and a low A minor chord with a distant
bell. The loop is four seconds; the sound is 1.6.

> **Status: third console test pending.** The HOME Menu is the only thing that shows a CIA's
> banner, and Azahar does not draw them, so every change needs a console. Builds so far:
>
> | Build | What was different from the banners that work | Result |
> | --- | --- | --- |
> | v0.2.1-rc.1 | A 551 KB banner (2.9 s jingle, 256 × 256 atlas), textures on the rust and the fog, an unanimated fog sheet, ash with no scale track | Crashed the HOME Menu on selection |
> | v0.2.1-rc.2 | The size fixed (314 KB); the rest unchanged | Froze the HOME Menu |
> | v0.2.1-rc.3 | Nothing: every node follows a pattern of 3dsDL's or 3dsFM's working banner (below) | |
>
> `SH23DS_FLAT_BANNER=1 port/package.sh` packs the flat picture instead.

| File | What it is |
| --- | --- |
| `textures.py` | Draws the 48 × 48 icon (`port/meta/icon.png`, the website's mark in rusted metal). |
| `logo.html` | The wordmark, in the website's Libre Caslon Display, shot as `logo.png` (256 × 128), the model's one texture. |
| `model.py` | Builds the scene as code: shapes, materials and rigid keyframe animation, written out as `banner.glb`. Refuses a model over the limits below or with a pattern the working banners don't use. |
| `jingle.py` | Synthesises `jingle.wav`, an original cue timed to the animation. |
| `build-cgfx.py` | Converts `banner.glb` to `sh23ds.cgfx` with [pycgfx](https://github.com/skyfloogle/pycgfx) and fixes what pycgfx gets wrong (see the rules). |
| `render.html`, `render.sh` | Render the model in headless Chrome with three.js, framed by the banner camera, for the flat banner and for previews. |
| `logo.png`, `banner.glb`, `sh23ds.cgfx`, `jingle.wav` | The generated files, committed so that `port/package.sh` needs none of these tools. |

## Rebuilding

```sh
port/meta/banner/render.sh   # icon, wordmark, model, flat banner, preview frames
port/meta/banner/make.sh     # jingle, model and CGFX (needs Docker)
port/package.sh              # packs them into the CIA
```

`render.sh` needs Google Chrome and the repo's `.venv` (numpy, Pillow). `make.sh` runs pycgfx in a
digest-pinned Python container, with pycgfx's files fetched at a pinned commit and checked against
known hashes; pycgfx has no licence file, so it is never stored here. The `.3dsx` has no banner (the
Homebrew Launcher shows only the icon); its icon, name, description and author come from
`port/Makefile`, which `package.sh` reads for the CIA too.

## Rules the model follows

The pipeline is 3dsDL's (github.com/ankushKun/3dsDL, `resources/banner`). Two banners made with it
work on a console: 3dsDL's final one and 3dsFM's 0.2.3. Seven builds failed: 3dsDL's first three,
3dsFM's 0.2.0 to 0.2.2, and sh23ds's rc.1 and rc.2. Every failed build did something neither
working banner does, but no one feature explains them all, and nothing on the console says which
part was at fault. So this model uses only what those two banners do, and `model.py` checks it.
The check passes both working banners and rejects all six failed models (3dsDL's first two
failures were in the converter, fixed below, not in their models).

- **Every mesh names its node, and every name lookup resolves.** pycgfx leaves mesh node names
  blank, which crashed 3dsDL's first build, and can leave the bones' Patricia tree stale, which
  crashed its second (a null pointer read in `menu`). `build-cgfx.py` fills the names in, rebuilds
  the tree and walks every table's tree the way the console does, refusing a CGFX in which any name
  is lost.
- **Two shapes of hierarchy, both from the working banners.** The mark is 3dsDL's card: an
  unanimated rig on the root holds its placement, one animated bone under it sways, bobs and pulses
  (13° of sway against the card's 19°, a pulse of 0.99 to 1.05 against its squash of 0.90 to 1.08),
  and its parts hang under that with no transform or animation of their own. The fog and the ash
  are 3dsFM's record and 3dsDL's sparkles: one animated bone on the root (Air, which only breathes
  very slightly), the fog sheet under it unanimated, and the flakes under it animated. No bone is
  deeper than 3dsDL's deepest (three below the root).
- **Every animated bone has translation, rotation and scale tracks**, from 0 to 4 s, with no two
  keys closer than 0.01 s; the wordmark alone bobs on translation, as 3dsDL's does. No mesh is
  drawn from a bone that neither moves nor hangs from one that does.
- **Nothing scales below 0.3 and nothing spins.** 3dsDL's third build shrank parts to 0.001 and
  froze; 3dsFM's three frozen builds all spun the record a full turn. The ash falls off the bottom
  of the screen and jumps back to the top between two keys 0.01 s apart, as 3dsDL's arrow does.
- **One texture, on the billboard wordmark only**, as in both working banners: `logo.png`,
  256 × 128, drawn as a Y-axis billboard with a hard-edged cut-out (alpha MASK), never a blend.
  The rust is shaped from geometry in a second colour instead of a texture, and the fog is a plain
  sheet whose normals are bowed so the light fades it toward the lower right.
- **Within 3dsDL's counts**: 14 bones (14), 10 meshes (11), 6 materials (7), 1,647 triangles and
  1,724 vertices (2,000 and 2,048), and the CGFX at 154 KiB against 3dsDL's 151 KiB (512 KiB is
  the documented limit).
- **The sound** is 44.1 kHz 16-bit stereo and 1.6 s, exactly 3dsDL's jingle, and the packed banner
  stays under 330,000 bytes (3dsDL's and 3dsFM's working banners pack to 315 and 324 KB, rc.1's was
  551 KB). `jingle.py` and `port/package.sh` refuse anything bigger.
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
sh23ds and install a flat banner build (`SH23DS_FLAT_BANNER=1 port/package.sh`) or a release that
worked over it. If the HOME
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
