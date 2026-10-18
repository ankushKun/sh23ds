#include "room.h"
#include "sdsim.h"
#include "sdio.h"
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <tex3ds.h>
#include "profile.h"

#define TEXCACHE_MAX 512
// bytes of texture kept from rooms visited earlier: 12 MB, 6 MB with less memory (profile.c)
#define TEXCACHE_BUDGET profile_texcache_budget()

typedef struct {
	u32 id;                 // 0: free slot
	u32 last_use;           // room load generation that last used it
	u32 bytes;
	C3D_Tex tex;
	bool ok;
} TexEntry;

static TexEntry s_cache[TEXCACHE_MAX];
static u32 s_cached;        // slots in use (free slots inside this range are reused)
static u32 s_generation;
static char s_cache_area[16];

void texcache_clear(void)
{
	for (u32 i = 0; i < s_cached; i++)
		if (s_cache[i].ok)
			C3D_TexDelete(&s_cache[i].tex);
	s_cached = 0;
	s_cache_area[0] = 0;
}

u32 texcache_count(void)
{
	u32 n = 0;
	for (u32 i = 0; i < s_cached; i++)
		n += s_cache[i].id != 0;
	return n;
}

// Frees textures that only earlier rooms used, oldest first, until the cache fits the budget. The
// current room's textures are never touched, and slots are not moved, so its material pointers stay
// valid. The caller has synced the GPU before freeing the previous room.
static void texcache_trim(void)
{
	for (;;) {
		u32 total = 0, oldest = 0xFFFFFFFF;
		TexEntry* victim = NULL;
		for (u32 i = 0; i < s_cached; i++) {
			TexEntry* e = &s_cache[i];
			if (!e->id)
				continue;
			total += e->bytes;
			if (e->last_use < s_generation && e->last_use < oldest) {
				oldest = e->last_use;
				victim = e;
			}
		}
		if (total <= TEXCACHE_BUDGET || !victim)
			return;
		if (victim->ok)
			C3D_TexDelete(&victim->tex);
		memset(victim, 0, sizeof *victim);
	}
}

// Marks every texture the given rooms use as in use, then trims the rest to the budget. Several rooms
// can be resident at once (neighbouring town tiles), so the caller lists all of them.
void texcache_keep(const Room* const* rooms, int n)
{
	s_generation++;
	for (int r = 0; r < n; r++)
		for (u32 i = 0; i < rooms[r]->n_materials; i++) {
			const C3D_Tex* t = rooms[r]->materials[i].tex;
			if (!t)
				continue;
			TexEntry* e = (TexEntry*)((u8*)t - offsetof(TexEntry, tex));
			if (e >= s_cache && e < s_cache + TEXCACHE_MAX)
				e->last_use = s_generation;
		}
	texcache_trim();
}

int texcache_ids(const char* area, u32* out, int max)
{
	int n = 0;
	if (strcmp(area, s_cache_area) != 0)
		return 0;
	for (u32 i = 0; i < s_cached && n < max; i++)
		if (s_cache[i].id)
			out[n++] = s_cache[i].id;
	return n;
}

// With little memory (profile_tex_half) room textures are kept from their second mip level: the
// .t3x files carry the whole chain, so the copy of levels 1.. is the same texture at half the width
// and height in a quarter of the memory, with no second texture pack. Textures under 16 pixels stay
// as they are.
static void tex_halve(C3D_Tex* t)
{
	if (t->maxLevel < 1 || t->width < 16 || t->height < 16)
		return;
	C3D_Tex h;
	C3D_TexInitParams p = { t->width / 2, t->height / 2, t->maxLevel - 1, t->fmt, GPU_TEX_2D, false };
	if (!C3D_TexInitWithParams(&h, NULL, p))
		return;
	u32 size = 0;
	const void* level1 = C3D_TexGetImagePtr(t, t->data, 1, &size);
	memcpy(h.data, level1, h.size);       // levels 1.. are contiguous and as long as h's whole chain
	C3D_TexFlush(&h);
	C3D_TexDelete(t);
	*t = h;
}

static C3D_Tex* texcache_get(const char* pack, const char* area, u32 id, const RoomTexBlob* tex, int ntex)
{
	if (strcmp(area, s_cache_area) != 0) {
		texcache_clear();
		snprintf(s_cache_area, sizeof s_cache_area, "%s", area);
	}
	TexEntry* e = NULL;
	for (u32 i = 0; i < s_cached; i++) {
		if (s_cache[i].id == id) {
			s_cache[i].last_use = s_generation;
			return s_cache[i].ok ? &s_cache[i].tex : NULL;
		}
		if (!s_cache[i].id && !e)
			e = &s_cache[i];
	}
	if (!e) {
		if (s_cached >= TEXCACHE_MAX)
			return NULL;
		e = &s_cache[s_cached++];
	}
	memset(e, 0, sizeof *e);
	e->id = id;
	e->last_use = s_generation;
	Tex3DS_Texture t3x = NULL;
	for (int i = 0; i < ntex; i++)
		if (tex[i].id == id && tex[i].data) {
			t3x = Tex3DS_TextureImport(tex[i].data, tex[i].size, &e->tex, NULL, false);
			break;
		}
	if (!t3x) {
		char path[256];
		snprintf(path, sizeof path, "%s/bg/%s/tex/%08lx.t3x", pack, area, (unsigned long)id);
		FILE* f = fopen(path, "rb");
		if (!f)
			return NULL;
		sdsim_open();
		t3x = Tex3DS_TextureImportStdio(f, &e->tex, NULL, false);
		sdsim_read((u32)ftell(f));
		fclose(f);
	}
	if (!t3x)
		return NULL;
	Tex3DS_TextureFree(t3x);
	if (profile_tex_half())
		tex_halve(&e->tex);
	e->bytes = e->tex.size * 4 / 3;      // level 0 plus the mipmap chain
	C3D_TexSetFilter(&e->tex, GPU_LINEAR, GPU_LINEAR);
	C3D_TexSetFilterMipmap(&e->tex, GPU_LINEAR);
	C3D_TexSetWrap(&e->tex, GPU_REPEAT, GPU_REPEAT);
	e->ok = true;
	return &e->tex;
}

typedef struct {
	char magic[4];
	u32 version;
	float bbox_min[3], bbox_max[3];
	u32 n_materials, n_batches, n_vertices, n_indices;
	u32 ofs_materials, ofs_batches, ofs_vertices, ofs_indices;
} S3MHeader;

bool room_load(Room* room, const char* pack, const char* area, const char* name, char* err, size_t errlen)
{
	memset(room, 0, sizeof *room);
	char path[256];
	snprintf(path, sizeof path, "%s/bg/%s/%s.s3m", pack, area, name);
	FILE* f = fopen(path, "rb");
	if (!f) {
		snprintf(err, errlen, "cannot open %s", path);
		return false;
	}
	fseek(f, 0, SEEK_END);
	long size = ftell(f);
	fseek(f, 0, SEEK_SET);
	u8* data = malloc(size);
	if (!data || sd_fread(data, size, f) != (size_t)size) {
		fclose(f);
		free(data);
		snprintf(err, errlen, "read failed (%ld bytes)", size);
		return false;
	}
	fclose(f);
	return room_load_mem(room, pack, area, name, data, size, NULL, 0, err, errlen);
}

#define S3M_REBASED 0x80000000u    // version bit set by room_prepare (in memory only)

void room_prepare(u8* data, long size)
{
	S3MHeader* h = (S3MHeader*)data;
	if (size < (long)sizeof *h || memcmp(h->magic, "S3M1", 4) != 0 || h->version != 2)
		return;
	if (h->ofs_batches + h->n_batches * sizeof(RoomBatchFile) > (u32)size ||
	    h->ofs_indices + h->n_indices * sizeof(u16) > (u32)size)
		return;
	const RoomBatchFile* fb = (const RoomBatchFile*)(data + h->ofs_batches);
	u16* idx = (u16*)(data + h->ofs_indices);
	for (u32 i = 0; i < h->n_batches; i++) {
		if (fb[i].first_index + fb[i].index_count > h->n_indices)
			return;                      // left to room_load_mem, which does the same
	}
	for (u32 i = 0; i < h->n_batches; i++)
		for (u32 k = 0; k < fb[i].index_count; k++)
			idx[fb[i].first_index + k] = (u16)(idx[fb[i].first_index + k] + fb[i].first_vertex);
	h->version |= S3M_REBASED;
}

int room_texture_ids(const u8* data, long size, u32* out, int max)
{
	const S3MHeader* h = (const S3MHeader*)data;
	if (size < (long)sizeof *h || memcmp(h->magic, "S3M1", 4) != 0 ||
	    h->ofs_materials + (long)h->n_materials * 16 > size)
		return 0;
	const u32* m = (const u32*)(data + h->ofs_materials);
	int n = 0;
	for (u32 i = 0; i < h->n_materials && n < max; i++, m += 4) {
		bool seen = !m[0];
		for (int k = 0; k < n && !seen; k++)
			seen = out[k] == m[0];
		if (!seen)
			out[n++] = m[0];
	}
	return n;
}

// ---------------------------------------------------------------- map part switches

// The loaded maps' batches (Room structs are copied by value; their batch arrays stay put).
#define PART_MAPS 24
static struct {
	RoomBatch* batches;
	u32 n, map_id;
} s_parts_maps[PART_MAPS];
// The switches applied since the glb last changed, for maps that load after them.
#define PART_KEEP 48
static u32 s_keep_key[PART_KEEP], s_keep_mask[PART_KEEP], s_keep_glb;
static int s_nkeep;

static bool open_glb(int glb) { return glb == 0x11 || (glb >= 1 && glb <= 4); }   // 0x494d80

// 0x5b49b0 on one map: the variants of group g on by mask
static void parts_switch(RoomBatch* b, u32 n, int g, u32 mask)
{
	for (u32 i = 0; i < n; i++) {
		int v = b[i].part & 0xff;
		if ((b[i].part >> 8) == g)
			b[i].hidden = !(v == 0 || (mask & 0xffff & (1u << ((v - 1) & 31))));
	}
}

// 0x47d5d0
static void parts_key(u32 key, u32 mask, int only)
{
	for (int k = 0; k < PART_MAPS; k++)
		if (s_parts_maps[k].batches && s_parts_maps[k].map_id == key) {
			if (only < 0 || only == k)
				parts_switch(s_parts_maps[k].batches, s_parts_maps[k].n, 0, mask);
			return;
		}
	if (open_glb((int)(key >> 16)))
		return;
	for (int k = 0; k < PART_MAPS; k++) {
		u32 id = s_parts_maps[k].map_id;
		if (s_parts_maps[k].batches && id && key >= id && key - id < 256 && (only < 0 || only == k))
			parts_switch(s_parts_maps[k].batches, s_parts_maps[k].n, (int)(key - id), mask);
	}
}

void room_parts_add(RoomPartList* l, int glb, int id, int bit)
{
	u32 key = (u32)glb << 16 | (u32)id;
	int i = 0;
	while (i < l->n && l->key[i] != key)
		i++;
	if (i == l->n) {
		if (l->n >= 16)
			return;
		l->key[l->n] = key, l->mask[l->n] = 0, l->n++;
	}
	if (bit >= 0)
		l->mask[i] |= 1u << (bit & 31);
}

void room_parts_apply(const RoomPartList* l, int glb)
{
	if ((u32)glb != s_keep_glb)
		s_nkeep = 0, s_keep_glb = (u32)glb;
	if (open_glb(glb))
		s_nkeep = 0;                    // each apply starts from variant 0 there
	if (open_glb(glb))
		for (int k = 0; k < PART_MAPS; k++)
			if (s_parts_maps[k].batches)
				parts_switch(s_parts_maps[k].batches, s_parts_maps[k].n, 0, 0);
	for (int i = 0; i < l->n; i++) {
		parts_key(l->key[i], l->mask[i], -1);
		int j = 0;
		while (j < s_nkeep && s_keep_key[j] != l->key[i])
			j++;
		if (j == s_nkeep && s_nkeep < PART_KEEP)
			s_keep_key[s_nkeep++] = l->key[i];
		if (j < PART_KEEP)
			s_keep_mask[j] = l->mask[i];
	}
}

static u32 map_id_of(const char* area, const char* name)
{
	static const char* const areas[] = { "ca", "cb", "cc", "cd", "ob", "er", "bw", "th", "ap", "hp", "ps", "rr",
	                                     "ru", "qp", "qt", "ma" };
	for (int i = 0; i < 16; i++)
		if (!strncmp(area, areas[i], 2) && !strncmp(name, areas[i], 2) && name[2] >= '0' && name[2] <= '9')
			return (u32)(i + 1) << 16 | (u32)atoi(name + 2);
	return 0;
}

static void parts_register(Room* room, const char* area, const char* name)
{
	room->map_id = map_id_of(area, name);
	for (int k = 0; k < PART_MAPS; k++)
		if (!s_parts_maps[k].batches) {
			s_parts_maps[k].batches = room->batches;
			s_parts_maps[k].n = room->n_batches;
			s_parts_maps[k].map_id = room->map_id;
			// the switches already applied in this glb reach a map that loads later
			if (room->map_id >> 16 == s_keep_glb)
				for (int i = 0; i < s_nkeep; i++)
					parts_key(s_keep_key[i], s_keep_mask[i], k);
			return;
		}
}

static void parts_unregister(const RoomBatch* batches)
{
	for (int k = 0; k < PART_MAPS; k++)
		if (batches && s_parts_maps[k].batches == batches)
			s_parts_maps[k].batches = NULL;
}

bool room_tex_warm(const char* pack, const char* area, const RoomTexBlob* tex, int ntex, u64 deadline)
{
	bool first = true;
	for (int i = 0; i < ntex; i++) {
		if (!tex[i].data)
			continue;
		bool have = false;
		if (!strcmp(area, s_cache_area))
			for (u32 k = 0; k < s_cached && !have; k++)
				have = s_cache[k].id == tex[i].id;
		if (have)
			continue;
		if (!first && svcGetSystemTick() >= deadline)
			return false;
		first = false;
		texcache_get(pack, area, tex[i].id, tex, ntex);
	}
	return true;
}

bool room_load_mem(Room* room, const char* pack, const char* area, const char* name, u8* data, long size,
                   const RoomTexBlob* tex, int ntex, char* err, size_t errlen)
{
	memset(room, 0, sizeof *room);
	S3MHeader* h = (S3MHeader*)data;
	bool rebased = size >= (long)sizeof *h && (h->version & S3M_REBASED);
	if (size < (long)sizeof *h || memcmp(h->magic, "S3M1", 4) != 0 || (h->version & ~S3M_REBASED) != 2) {
		char path[256];
		snprintf(path, sizeof path, "%s/bg/%s/%s.s3m", pack, area, name);
		free(data);
		snprintf(err, errlen, "%s: not S3M v2 (re-run the converter)", path);
		return false;
	}
	memcpy(room->bbox_min, h->bbox_min, sizeof room->bbox_min);
	memcpy(room->bbox_max, h->bbox_max, sizeof room->bbox_max);
	room->n_materials = h->n_materials;
	room->n_batches = h->n_batches;
	room->n_vertices = h->n_vertices;
	room->n_indices = h->n_indices;

	room->materials = calloc(h->n_materials ? h->n_materials : 1, sizeof(RoomMaterial));
	room->batches = calloc(h->n_batches ? h->n_batches : 1, sizeof(RoomBatch));
	if (h->n_vertices > 65536) {
		unsigned long verts = h->n_vertices;
		free(data);
		snprintf(err, errlen, "%lu vertices: room needs splitting", verts);
		return false;
	}
	room->vertices = linearAlloc(h->n_vertices * ROOM_VERTEX_SIZE);
	if (!room->materials || !room->batches || !room->vertices) {
		unsigned long verts = h->n_vertices;
		free(data);
		room_free(room);
		snprintf(err, errlen, "out of memory (%lu verts)", verts);
		return false;
	}
	const u32* m = (const u32*)(data + h->ofs_materials);
	s_generation++;      // textures this room touches count as in use (see texcache_keep)
	for (u32 i = 0; i < h->n_materials; i++, m += 4) {
		RoomMaterial* rm = &room->materials[i];
		rm->texture_id = m[0];
		rm->mode = m[1];
		rm->color = m[2];
		rm->flags = m[3];
		if (rm->texture_id) {
			rm->tex = texcache_get(pack, area, rm->texture_id, tex, ntex);
			if (!rm->tex)
				room->missing_textures++;
		}
	}
	const RoomBatchFile* fb = (const RoomBatchFile*)(data + h->ofs_batches);
	const u16* fidx = (const u16*)(data + h->ofs_indices);
	for (u32 i = 0; i < h->n_batches; i++) {
		RoomBatch* b = &room->batches[i];
		b->group = fb[i].group & 0xff;
		b->part = (u16)(fb[i].group >> 8);       // the geometry id (0 in packs made before it was kept)
		b->hidden = (b->part & 0xff) != 0;       // 0x5b4690: only variant 0 shows until switched
		b->material = fb[i].material;
		b->first_vertex = fb[i].first_vertex;
		b->vertex_count = fb[i].vertex_count;
		b->index_count = fb[i].index_count;
		b->indices = linearAlloc(b->index_count * sizeof(u16));
		if (!b->indices) {
			free(data);
			room_free(room);
			snprintf(err, errlen, "out of memory (indices)");
			return false;
		}
		// Rebase to room-global vertex indices: every batch then draws from one vertex buffer
		// binding, and no two batches share index values (a post-transform vertex cache keyed by
		// index would otherwise hand later batches stale vertices from earlier ones).
		if (rebased)
			memcpy(b->indices, fidx + fb[i].first_index, b->index_count * sizeof(u16));
		else
			for (u32 k = 0; k < b->index_count; k++)
				b->indices[k] = (u16)(fidx[fb[i].first_index + k] + b->first_vertex);
	}
	memcpy(room->vertices, data + h->ofs_vertices, h->n_vertices * ROOM_VERTEX_SIZE);
	free(data);
	parts_register(room, area, name);
	return true;
}

void room_free(Room* room)
{
	parts_unregister(room->batches);
	free(room->materials);
	if (room->batches)
		for (u32 i = 0; i < room->n_batches; i++)
			if (room->batches[i].indices)
				linearFree(room->batches[i].indices);
	free(room->batches);
	if (room->vertices)
		linearFree(room->vertices);
	memset(room, 0, sizeof *room);
}
