#pragma once
#include <3ds.h>
#include <citro3d.h>

// The item screen's 3D model of an item (the PC's item menu shows the item's model turning):
// drawn into the screen rectangle (x0, y0)-(x1, y1) of the bottom screen, inside a C3D frame with
// the bottom target bound. itemview_has loads the item's model (false when it has none).
bool itemview_has(int item);
void itemview_draw(int item, float x0, float y0, float x1, float y1, float angle);
void itemview_free(void);
// The same model drawn into a square texture of its own (transparent round it), inside a C3D frame;
// the caller draws on its own target again afterwards. NULL when the item has no model.
C3D_Tex* itemview_render(int item, float angle);
