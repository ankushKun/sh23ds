#include "quality.h"

static Quality Q = {
	.fog_sprites = 144,
	.stencil_shadows = true,
	.drop_shadows = true,
	.tile_distance = 7000.0f,
	.frame_cap = 0,
	.fog_fill_3d = 3.0f,
	.supersample = 1,
};

const Quality* quality(void) { return &Q; }
void quality_set(const Quality* q) { Q = *q; }
