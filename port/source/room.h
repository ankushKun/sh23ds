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
	u16 part;           // the map geometry's id: variant (low byte) and part group (high byte)
	u8 hidden;          // a part the stage code has not switched on (see room_parts_apply)
} RoomBatch;

typedef struct {
	float bbox_min[3], bbox_max[3];
	u32 n_materials, n_batches, n_vertices, n_indices;
	RoomMaterial* materials;
	RoomBatch* batches;
	void* vertices;     // linear memory, 28-byte vertices (pos, uv, color, normal)
	u32 missing_textures;
	u32 map_id;         // the PC's map file id, glb_crd << 16 | the map's number (ap22: 0x90016)
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
// Puts a tile's read-ahead textures into the cache until svcGetSystemTick() passes deadline (at
// least one), so that room_load_mem finds them there and a tile's uploads spread over a few frames.
// True when every blob is in the cache. Main thread only.
bool room_tex_warm(const char* pack, const char* area, const RoomTexBlob* tex, int ntex, u64 deadline);
// Rebases an .s3m's indices to room-global vertex numbers in place, as room_load_mem would, and
// marks it done (safe on any thread: the reader thread does it, so the join does not).
void room_prepare(u8* data, long size);
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

// The PC's map part switches. A map geometry's id holds a part group (high byte) and a variant
// (low byte); 0x5b4690 shows variant 0 and hides the others when the map loads, and the stage code
// switches variants on by group: per frame or on entering a room it lists (glb_crd << 16 | id,
// mask) pairs (0x4a2a80) and applies them (0x4a2ad0, then 0x47d5d0 and 0x5b49b0). A switch whose
// key is a resident map's id sets that map's group 0; otherwise, outside the town and forest, every
// resident map with an id below the key gets the group key - its id. The variants on are those
// whose bit (variant - 1) is in the mask. In the town and forest the apply first turns every
// group 0 back to variant 0 only (0x47d4f0).
typedef struct {
	u32 key[16], mask[16];
	int n;
} RoomPartList;
// 0x4a2a80: add id (glb_crd << 16 | id) to the list, with bit set in its mask unless bit < 0.
void room_parts_add(RoomPartList* l, int glb, int id, int bit);
// 0x4a2ad0: apply the list to the resident maps (and to maps that load later in the same glb).
void room_parts_apply(const RoomPartList* l, int glb);
