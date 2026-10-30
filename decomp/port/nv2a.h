/* The NV2A (Xbox GPU) layer shared by port/d3d8.c, port/nv2a.c, port/pushbuffer.c and port/gpu.c. Plain C types
 * only: gpu.c includes the real Windows headers, the others the decompiler's. */
#ifndef NV2A_H
#define NV2A_H

typedef struct { float v[4]; } NvVec4;

/* vertex program state */
#define NV_CONSTANTS 192       /* registers -96..95, stored at index + 96 */
#define NV_PROGRAM_SLOTS 136
extern float nv_constants[NV_CONSTANTS][4];
extern unsigned nv_program[NV_PROGRAM_SLOTS][4];
extern int nv_program_start;

/* vertex program outputs (the NV2A's output register numbers) */
enum { NV_OUT_POS = 0, NV_OUT_D0 = 3, NV_OUT_D1 = 4, NV_OUT_FOG = 5, NV_OUT_PTS = 6, NV_OUT_B0 = 7, NV_OUT_B1 = 8,
       NV_OUT_T0 = 9, NV_OUTPUTS = 13 };

void nv_vsh_run(int start, NvVec4 in[16], NvVec4 out[NV_OUTPUTS]);
void nv_vsh_dump(int start, int max);

/* vertex attributes: where register v0..v15 comes from (Direct3D's vertex shader declaration, or the push buffer's
 * SET_VERTEX_DATA_ARRAY_FORMAT/OFFSET) */
typedef struct {
    const unsigned char *data;  /* first vertex's attribute; 0 = not fed (the default value is used) */
    unsigned stride;
    unsigned type;              /* NV2A array type: 0 UB_D3D, 1 S1, 2 F, 4 UB_OGL, 5 S32K, 6 CMP */
    unsigned count;             /* components */
} NvAttribute;

extern NvAttribute nv_attributes[16];
extern NvVec4 nv_attribute_defaults[16];

/* push buffers and single methods */
void nv_run_pushbuffer(const unsigned *words, unsigned count);
void nv_method(unsigned method, unsigned value);
void nv_draw_arrays(unsigned primitive, unsigned first, unsigned count);
void nv_draw_indexed(unsigned primitive, const unsigned *indices, unsigned count);

/* Direct3D-side state the draws need */
extern int nv_use_program;        /* 1: run the vertex program; 0: fixed function (pretransformed FVF) */
extern int nv_log_frames;         /* frames left to log in detail */
extern unsigned nv_draws;

/* port/gpu.c: the Direct3D 9 side. Vertices arrive pretransformed: x, y in pixels, z in 0..1, rhw. */
typedef struct {
    float x, y, z, rhw;
    unsigned diffuse, specular;
    float t[4][4];
} GpuVertex;
void gpu_draw(int primitive, const GpuVertex *v, unsigned count);  /* NV2A primitive numbering, 1..10 */
void gpu_set_texture(int stage, const void *xbox_texture);
void gpu_render_state(unsigned method, unsigned value);

#endif
