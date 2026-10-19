# sh2-3ds-port-kit

Tools and data for reverse engineering `sh2pc.exe` (Silent Hill 2 PC) toward a New 3DS port. The reference build is the **Director's Cut** (MD5 `f3f5360298b6d0f6f2e09cf24a0925d1`). v1.0 data (MD5 `37caa31bdb473d9019be2491bd8dc914`) is kept alongside. None of this contains game code or assets.

See `../docs/RESEARCH.md` for the full write-up (findings, the original plan, setup and the work log) and `../README.md` for the player and developer guide.

```
tools/
  sh2version.py      identify a build (v1.0 / v1.1 / DC), hashes, SecuROM, known-good
  ee_resolve.py      resolve Silent Hill 2 Enhancements address lookups against an exe
  ee_labels.py       build a label JSON (EE globals + platform labels for v1.0 and DC)
  match_versions.py  map functions between two builds by masked-byte hashing
  dc_imports.py      name the DC's SecuROM-redirected imports from v1.0 call sites
  module_map.py      first-pass module map (libraries, known source files, gaps)
  texture_census.py  texture formats and sizes in a data/ folder
ghidra_scripts/
  ApplyLabels.java   import a label JSON
  Export.java        dump functions.tsv, import_xrefs.tsv, optional decomp.c
  Refs.java          who references an address; dec:<addr> decompiles
  ApplyImportSites.java  apply dc_imports.py output (thunks + call overrides)
  FixOrphans.java    create functions for orphan .text code, drop fragments
  ExportGraph.java   functions, strings, calls, globals, code pointers for module_map.py
  ApplyModuleMap.java  tag functions with their module
  ImportPs2Types.java  PS2 beta structs and enums (headers from tools/dwarf2h.py), one category per file
  ExportProject.java   dump the project's functions, names, tags, labels, comments to data/dc/project
  ImportProject.java   put that dump back into a fresh import (tools/ghidra/rebuild.sh)
  ApplyNames.java      apply checked names from a TSV (addr, name, evidence), never over a hand-made one
  Decompile.java       decompile by address or name with callers and tags (tools/ghidra/dec.sh)
data/
  dc/project/        the saved Ghidra project as text (functions with bodies and tags, labels, typed
                     globals, comments, bookmarks): rebuilt by tools/ghidra/rebuild.sh, refreshed by
                     tools/ghidra/export.sh
  dc/port_names.tsv  PC names the port's comments cite (tools/ghidra/port_names.py)
  dc/view_aligned_names.tsv  camera module names from tools/align_names.py
  dc/                Director's Cut: 82 labels, EE lookups, Ghidra functions (auto + fixed), import xrefs,
                     import names (dc_import_*), module map (module_*), CRI and hand-checked
                     names (cri_labels.json, manual_labels.json), source paths
  v10/               v1.0: same set
  v10_to_dc_functions.tsv   3,535 one-to-one function matches (v1.0 entry -> DC entry)
```

Python needs `pip install pefile capstone`. The everyday entry points are in `../tools/ghidra/` and
`../AGENTS.md` explains when to use which. Ghidra scripts were tested with Ghidra 12.1.4 headless.

Notes:
- In the DC, imports go through SecuROM, so `data/dc/ghidra_import_xrefs.tsv` mostly lists SecuROM code. `data/dc/dc_import_sites.tsv` names the real API per call site (605 of 730 dispatcher sites, all stub calls).
- `gRunOption` was fixed to `gControlType - 6`. `gMemoInventory`/`gGameFlag` share an address on purpose: the memo inventory sits inside the game-flag bitfield.
