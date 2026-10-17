#pragma once
#include <3ds.h>
#include <citro3d.h>

// One converted room (S3M v2, see tools/sh2conv/convert.py).

enum { GROUP_OPAQUE = 0, GROUP_TRANSPARENT = 1, GROUP_DECAL = 2 };

typedef struct {
	u32 texture_id;
	u32 mode;
	u32 color;
	u32 flags;          // bit0: texture has alpha
	C3D_Tex* tex;       // NULL if untextured or missing
} RoomMaterial;

typedef struct {
	u32 group, material, first_vertex, vertex_count, first_index, index_count;
} RoomBatchFile;

typedef struct {
	u32 group, material, first_vertex, vertex_count, index_count;
	u16* indices;       // own linear allocation: the PICA index buffer address must be aligned
} RoomBatch;

typedef struct {
	float bbox_min[3], bbox_max[3];
	u32 n_materials, n_batches, n_vertices, n_indices;
	RoomMaterial* materials;
	RoomBatch* batches;
	void* vertices;     // linear memory, 28-byte vertices (pos, uv, color, normal)
	u32 missing_textures;
} Room;

#define ROOM_VERTEX_SIZE 28

// Loads <pack>/bg/<area>/<name>.s3m and its textures. Returns false with a message in err.
bool room_load(Room* room, const char* pack, const char* area, const char* name, char* err, size_t errlen);

// A texture file read ahead of time (stream.c reads tiles on a worker thread).
typedef struct {
	u32 id;
	void* data;
	size_t size;
} RoomTexBlob;

// The same from an .s3m already in memory (takes ownership of data). Textures come from the blobs
// when given, otherwise from the cache or the SD card. Main thread only (linear memory, C3D).
bool room_load_mem(Room* room, const char* pack, const char* area, const char* name, u8* data, long size,
                   const RoomTexBlob* tex, int ntex, char* err, size_t errlen);
// The texture ids an .s3m uses (no allocation; safe on any thread).
int room_texture_ids(const u8* data, long size, u32* out, int max);
// Texture ids the cache holds for an area (a snapshot for the reader thread).
int texcache_ids(const char* area, u32* out, int max);
void room_free(Room* room);

// Texture cache shared by rooms of one area (textures are keyed by SH2 texture id).
void texcache_clear(void);
// Keeps the textures of the resident rooms and trims the cache (call after loading rooms).
void texcache_keep(const Room* const* rooms, int n);
u32 texcache_count(void);
