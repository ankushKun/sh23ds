#pragma once
#include <3ds.h>

// pack/bg/<area>/area.idx (tools/sh2conv/areaindex.py): every room's bounds and camera roads in
// one file, so entering an area opens one file instead of two per room (port/PERFORMANCE.md).
// Callers fall back to the single files for names the index does not have.

// Reads the area's index unless it is the one already held; false when there is none.
bool areaidx_load(const char* pack_dir, const char* area);
bool areaidx_has(const char* name);
bool areaidx_bounds(const char* name, float bb[6]);
// The .cam contents for a name (NULL when it has none); valid until the next areaidx_load.
const void* areaidx_cam(const char* name, u32* size);
