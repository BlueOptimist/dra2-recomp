/* Digimon Rumble Arena 2 (Xbox) resource formats, as decoded by tools/*.py and verified against the game data.
 * Imported into the Ghidra project by tools/ghidra_scripts/TypeEngine.java; included by ghidra_types.h.
 *
 * rel32 fields are self-relative: target = (char *)&field + value. A value of 1 means "no target". They are not C
 * pointers, so code reads them as  (T *)((char *)&s->field + s->field).
 * All structures are little-endian with natural alignment (no padding is needed). */
#ifndef DRA2_FORMATS_H
#define DRA2_FORMATS_H

#pragma pack(push, 1)

typedef int rel32;
typedef unsigned int fourcc;

/* ---- .res container -------------------------------------------------------------------------------------- */

typedef struct ResTypeInfo {
    fourcc tag;                   /* chunk type, e.g. 'bmsh' */
    unsigned short version;
    unsigned short alignment;
} ResTypeInfo;

typedef struct ResHeader {
    fourcc magic;                 /* "res\n" */
    unsigned char version_major;  /* 7 */
    unsigned char version_minor;  /* 0 */
    unsigned char platform;       /* 0 = Xbox; nonzero = other platforms (GameCube, ...) */
    unsigned char pad07;
    unsigned int section0_offset; /* 0x1000 */
    unsigned int section0_size;
    unsigned int section1_offset; /* bulk data, e.g. streamed audio */
    unsigned int section1_size;
    unsigned int indx_offset;     /* indx chunk, relative to section 0 */
    unsigned int toc_offset;
    unsigned int toc_size;
    unsigned int type_count;
    ResTypeInfo types[1];         /* type_count entries */
} ResHeader;

typedef struct ResTocEntry {
    fourcc tag;
    unsigned int offset;          /* low 2 bits = section */
    unsigned int size;
    unsigned int reloc_count;
    rel32 relocs;                 /* u32 offsets of cross-resource pointers inside this entry */
} ResTocEntry;

typedef struct ResToc {
    unsigned int count;
    unsigned int unk04;
    ResTocEntry entries[1];
} ResToc;

typedef struct ResIndexEntry {
    rel32 name;                   /* string in strg */
    fourcc tag;
    rel32 data;
} ResIndexEntry;

typedef struct ResIndex {         /* 'indx' */
    unsigned int count;
    unsigned int unk04;
    ResIndexEntry entries[1];
} ResIndex;

/* ---- scripts and audio ------------------------------------------------------------------------------------ */

typedef struct LuaChunk {         /* 'lua!' */
    unsigned int length;
    rel32 text;                   /* Lua 5.0 source, `length` bytes */
} LuaChunk;

typedef struct XboxWaveFormat {   /* WAVEFORMATEX + samples per block */
    unsigned short format_tag;    /* 0x69 = Xbox ADPCM */
    unsigned short channels;
    unsigned int samples_per_sec;
    unsigned int avg_bytes_per_sec;
    unsigned short block_align;   /* 36 * channels */
    unsigned short bits_per_sample;
    unsigned short cb_size;
    unsigned short samples_per_block;
} XboxWaveFormat;

typedef struct Wave {             /* 'wave' sound effect */
    unsigned int id;
    unsigned char flags[4];       /* [1] = channel count */
    rel32 format;                 /* -> fmt */
    unsigned int data_size;
    rel32 data;                   /* 1 = empty sound */
    unsigned char unk14[0x0C];
    XboxWaveFormat fmt;
} Wave;

typedef struct Music {            /* 'musc' streamed music header */
    unsigned short format;        /* 1 = PCM */
    unsigned short channels;
    unsigned int sample_rate;
    unsigned int data_size;
    unsigned int data_offset;     /* in section 1 ('mdat') */
} Music;

/* ---- textures and materials ------------------------------------------------------------------------------ */

typedef struct Surface {          /* 'surf' texture */
    unsigned int unk00;
    unsigned int id;
    unsigned char format;         /* 0x0A = DXT1, 0x0B = DXT3 */
    unsigned char frame_count;
    unsigned char unk0a;
    unsigned char mip_count;
    unsigned short width;
    unsigned short height;
    unsigned int unk10;
    rel32 levels[1];              /* frame_count * mip_count, frame-major */
} Surface;

typedef struct ShaderLayer {
    rel32 texture;                /* -> Surface */
    unsigned char unk04[0x74];
} ShaderLayer;

typedef struct Shader {           /* 'gshd' material */
    unsigned char unk00[0x58];
    unsigned int layer_count;     /* 2 = base + reflection layer */
    unsigned int unk5c;
    ShaderLayer layers[1];
} Shader;

/* ---- meshes ---------------------------------------------------------------------------------------------- */

typedef struct MeshDescriptor {   /* shared by bmsh submeshes and rdms */
    unsigned char unk00[0x18];
    unsigned int pushbuffer_size;
    unsigned int unk1c;
    unsigned int unk20;
    rel32 pushbuffer;             /* NV2A commands: SET_BEGIN_END + ARRAY_ELEMENT16/32 */
    rel32 vertices;
} MeshDescriptor;

typedef struct SkinnedVertex {    /* 60 bytes */
    float position[3];
    float weights[4];
    float normal[3];
    float uv[2];
    unsigned int pad30[2];
    unsigned char slots[4];       /* banks of 13: palette index = slot - 1 - slot / 13 */
} SkinnedVertex;

typedef struct StaticVertex {     /* 44 bytes */
    float position[3];
    float normal[3];
    unsigned int color;           /* ARGB */
    float uv[2];
    unsigned int pad24[2];
} StaticVertex;

typedef struct SkinnedSubmesh {
    unsigned int vertex_count;
    rel32 descriptor;             /* -> MeshDescriptor */
    float radius;
    unsigned int unk0c;
    rel32 material;               /* -> Shader */
} SkinnedSubmesh;

typedef struct SkinnedGroup {
    unsigned int unk00;           /* 1 */
    unsigned int bone_count;
    rel32 palette;                /* u32 bone indices */
    unsigned int unk0c;
    unsigned int submesh_count;
    SkinnedSubmesh submeshes[1];
} SkinnedGroup;

typedef struct SkinnedMesh {      /* 'bmsh' */
    unsigned int group_count;
    unsigned int unk04;           /* 8 */
    float unk08;                  /* 1/4096 */
    SkinnedGroup groups[1];       /* variable size: 20 + 20 * submesh_count each */
} SkinnedMesh;

typedef struct StaticMesh {       /* 'rdms' */
    unsigned int submesh_count;   /* 1 */
    rel32 material;               /* -> Shader */
    unsigned int unk08;
    rel32 descriptor;             /* -> MeshDescriptor */
    unsigned int vertex_count;
    unsigned int unk14;
    unsigned int unk18;
    float radius;
} StaticMesh;

/* ---- skeletons and animation ----------------------------------------------------------------------------- */

typedef struct Bone {             /* 0xA0 bytes */
    rel32 name;
    unsigned int unk04;
    short parent;                 /* -1 = root */
    unsigned char unk0a;
    unsigned char flags;
    float unk0c;
    float rotate[4];              /* quaternion x y z w */
    float joint_orient[4];
    float rotate_axis[4];
    float scale[3];
    float unk4c;
    float translate[3];
    float radius;
    float inverse_bind[16];       /* row-vector layout, translation in the last row */
} Bone;

typedef struct Body {             /* 'body' skeleton; local = T * jointOrient * rotate * rotateAxis * S */
    rel32 unk00;
    rel32 bones;                  /* -> Bone[] */
    unsigned int lod_count;
    rel32 lods;                   /* rel32 -> SkinnedMesh, one per LOD */
    unsigned int attachment_count;
    rel32 attachments;            /* 0x34-byte entries, also the end of the bone array */
    rel32 bones_end;
} Body;

typedef struct AnimTrack {
    rel32 bone_name;
    unsigned short key_count;     /* n; followed by 3n+1 Bezier control points */
    unsigned short flags;         /* bits 0-3 component mask, 4-7 stored, 8-11 omitted, 12-15 channel (0 rot, 8 pos, 4 scale) */
    unsigned short times[1];      /* 1/60 s ticks, segment end times */
} AnimTrack;

typedef struct Animation {        /* 'banm' */
    unsigned int track_count;
    rel32 tracks;                 /* rel32 -> AnimTrack, one per track */
    unsigned int event_count;
    rel32 events;
    float duration;               /* seconds */
} Animation;

/* ---- levels and scene graph ------------------------------------------------------------------------------ */

typedef struct Level {            /* 'levl' */
    rel32 collision;              /* -> 'tern' */
    rel32 root_node;              /* -> Node */
} Level;

typedef struct Node {             /* 'node' Maya scene */
    rel32 curves;
    rel32 objects;                /* rel32 -> NodeObject, one per object */
} Node;

typedef struct NodeObject {
    rel32 type_name;              /* "transform", "platform", "effect", "sequencer", ... */
    rel32 name;
    rel32 attributes;             /* per-type attribute block */
    unsigned int unk0c;
    unsigned int unk10;
    unsigned int attribute_count;
    rel32 unk18;                  /* 1 or animation input */
    int parent;                   /* object index, -1 = root */
} NodeObject;

typedef struct TransformAttributes {
    float visibility;
    float translate[3];
    float rotate[3];              /* radians, xyz order */
    float scale[3];
} TransformAttributes;

typedef struct NodeTransform {
    NodeObject base;
    float scale_pivot[3];
    float scale_pivot_translate[3];
    float rotate_pivot[3];
    float rotate_pivot_translate[3];
} NodeTransform;

typedef struct NodePlatform {
    NodeObject base;
    unsigned int unk20;
    rel32 geometry;               /* record with bbox and, at +0x5C/+0x60, rel32 -> StaticMesh */
    rel32 collision;              /* -> 'tern' */
} NodePlatform;

typedef struct NodeEffect {
    NodeObject base;
    unsigned int unk20;
    unsigned int unk24;
    rel32 node_name;              /* name of the effect's own Node resource */
} NodeEffect;

#pragma pack(pop)

#endif
