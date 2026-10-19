#pragma once
#include <3ds.h>
#include <citro3d.h>
#include <stdbool.h>

// C3D_FrameSync waits for the GPU's frame events. Once the app is being closed from the HOME Menu
// those events no longer come, and the wait never ends (the console froze on HOME, Close). Every
// wait before freeing GPU memory goes through here and is skipped once APT says the app closes (a
// movie stopping on the way out included); main.c also sets app_closing when its loop ends.
extern bool app_closing;

static inline void gpu_sync(void)
{
	if (!app_closing && !aptShouldClose())
		C3D_FrameSync();
}
