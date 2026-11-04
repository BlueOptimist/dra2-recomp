/* Xbox DirectSound on Win32: a small software mixer played through waveOut (winmm, loaded at run time).
 *
 * The game drives two kinds of buffers (decomp/build/obj_src):
 *  - sound effects (FUN_001671c0): a buffer with no memory of its own, pointed at a sample resource with SetBufferData;
 *    the format comes from the resource (PCM or Xbox ADPCM). 2D voices are panned with SetMixBinVolumes (bins 0 and 1
 *    are front left and right), 3D voices get SetVolume plus a listener-relative SetPosition (FUN_00166d80).
 *  - the music and speech stream (FUN_00187320): a 4-second PCM ring buffer played looping, refilled from streams.res
 *    behind the play cursor read with GetCurrentPosition, its volume set with SetMixBinVolumes.
 *
 * Buffer status must be real. The generated stubs (xbox_stubs.c) returned success without filling their out parameters,
 * so the game read uninitialised memory as each buffer's status, every sound seemed to play forever, and anything that
 * waits for a sound to end waited forever: menu panels only finish once their sounds stop (FUN_000c8b90 ->
 * FUN_0011b790). Here a sound played once reports itself playing until its last sample has been mixed; a looping one
 * plays until it is stopped, as on the Xbox: the game's sound objects (FUN_00166f00) take a looping sound that stopped
 * by itself as never keyed off, and releasing it is then a fatal error (FUN_00166810).
 *
 * Play cursors advance in real time even with no audio device (or with DRA2_MUTE set): a timer thread mixes into
 * nothing, so streams still refill and one-shot sounds still end. DRA2_LOGSOUND=1 prints each buffer's format, and
 * DRA2_SOUNDDUMP=<file.wav> records everything mixed.
 * Not done: the amplitude envelopes (SetEG), I3DL2 reverb and the effects image; 3D voices are attenuated by
 * distance and panned left/right only. */
#include "win32_min.h"

void *malloc(unsigned size);
void *realloc(void *p, unsigned size);
void free(void *p);
int printf(const char *format, ...);

typedef struct { void *debug; long lock_count, recursion; HANDLE owner, semaphore; unsigned long spin; } CRITICAL_SECTION;
WINIMPORT void WINAPI InitializeCriticalSection(CRITICAL_SECTION *cs);
WINIMPORT void WINAPI EnterCriticalSection(CRITICAL_SECTION *cs);
WINIMPORT void WINAPI LeaveCriticalSection(CRITICAL_SECTION *cs);
WINIMPORT BOOL WINAPI SetThreadPriority(HANDLE thread, int priority);
WINIMPORT DWORD WINAPI SetFilePointer(HANDLE file, long distance, long *high, DWORD method);

/* waveOut, from winmm.dll */
typedef struct { WORD tag, channels; DWORD rate, bytes_per_second; WORD block_align, bits, extra; } WAVEFORMAT_X;
typedef struct wave_header {
    char *data;
    DWORD length, recorded;
    DWORD user, flags, loops;
    struct wave_header *next;
    DWORD reserved;
} WAVE_HEADER;
#define WHDR_DONE 1
#define CALLBACK_EVENT 0x50000
static unsigned(WINAPI *wave_open)(HANDLE *out, unsigned device, const WAVEFORMAT_X *format, DWORD callback, DWORD user,
                                   DWORD flags);
static unsigned(WINAPI *wave_prepare)(HANDLE out, WAVE_HEADER *header, unsigned size);
static unsigned(WINAPI *wave_write)(HANDLE out, WAVE_HEADER *header, unsigned size);

/* Xbox DSBUFFERDESC: dwSize, dwFlags, dwBufferBytes, lpwfxFormat, lpMixBins, dwInputMixBin */
typedef struct { DWORD size, flags, buffer_bytes; const WAVEFORMAT_X *format; void *mix_bins; DWORD input_mix_bin; }
    XDSBUFFERDESC;
/* DSMIXBINS: a count and that many { mix bin, volume in hundredths of a dB } pairs */
typedef struct { DWORD bin; long volume; } MIXBIN_VOLUME;
typedef struct { DWORD count; const MIXBIN_VOLUME *pairs; } XDSMIXBINS;

#define DSBCAPS_CTRL3D 0x10
#define WAVE_FORMAT_PCM 1
#define WAVE_FORMAT_XBOX_ADPCM 0x69
#define ADPCM_BLOCK 36    /* bytes per channel per block */
#define ADPCM_SAMPLES 64  /* frames per block */

#define OUTPUT_RATE 48000
#define CHUNK_FRAMES 512  /* ~10.7 ms per waveOut block */
#define CHUNKS 6

typedef struct SoundBuffer {
    struct SoundBuffer *prev, *next;  /* every live buffer, for the mixer */
    unsigned char *data;
    DWORD bytes;
    int owned;  /* data is ours (CreateSoundBuffer with a size, or grown by Lock), not the game's (SetBufferData) */
    WORD tag, channels, bits, block_align;
    DWORD rate, frequency;  /* the format's rate, and SetFrequency's (0: the format's) */
    double position;        /* play cursor, in frames */
    int playing, looping, is3d;
    float volume;           /* SetVolume */
    float bin[2];           /* SetMixBinVolumes for front left and right */
    float gain3d[2];        /* distance and pan of a 3D voice */
    int cached_block;       /* the ADPCM block decoded into cache, or -1 */
    short cache[ADPCM_SAMPLES * 2];
} SoundBuffer;

static int sound_device;  /* the IDirectSound object: only its address is used */
static SoundBuffer *buffers;
static CRITICAL_SECTION lock;
static int started, log_formats;

/* ------------------------------------------------------------------------------------------------- maths -- */

static double exp_approx(double x)  /* e^x for |x| up to ~20, to well under a percent */
{
    double y = x / 64.0, r = 1.0 + y * (1.0 + y * (0.5 + y * (1.0 / 6.0 + y / 24.0)));
    for (int i = 0; i < 6; i++)
        r *= r;
    return r;
}

static float centibels(long hundredths_db)  /* DirectSound volume (hundredths of a dB, -10000..0) to a gain */
{
    if (hundredths_db <= -10000)
        return 0.0f;
    if (hundredths_db > 0)
        hundredths_db = 0;
    return (float)exp_approx(hundredths_db * (2.302585093 / 2000.0));
}

static float square_root(float v)
{
    if (v <= 0.0f)
        return 0.0f;
    float r = v > 1.0f ? v : 1.0f;
    for (int i = 0; i < 20; i++)
        r = 0.5f * (r + v / r);
    return r;
}

/* ------------------------------------------------------------------------------------------- the formats -- */

static int adpcm(const SoundBuffer *b) { return b->tag == WAVE_FORMAT_XBOX_ADPCM; }

static DWORD frame_count(const SoundBuffer *b)
{
    if (!b->data || !b->channels)
        return 0;
    if (adpcm(b))
        return b->bytes / (ADPCM_BLOCK * b->channels) * ADPCM_SAMPLES;
    return b->block_align ? b->bytes / b->block_align : 0;
}

static DWORD frames_to_bytes(const SoundBuffer *b, DWORD frames)
{
    if (adpcm(b))
        return frames / ADPCM_SAMPLES * ADPCM_BLOCK * b->channels;
    return frames * b->block_align;
}

static DWORD bytes_to_frames(const SoundBuffer *b, DWORD bytes)
{
    if (adpcm(b))
        return b->channels ? bytes / (ADPCM_BLOCK * b->channels) * ADPCM_SAMPLES : 0;
    return b->block_align ? bytes / b->block_align : 0;
}

static const signed char ima_index[16] = { -1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8 };
static const short ima_step[89] = {
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97, 107,
    118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796, 876, 963,
    1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358, 5894,
    6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794,
    32767 };

/* Xbox ADPCM is IMA ADPCM in 36-byte blocks per channel: a 4-byte header (first sample, step index), then 32 bytes of
 * nibbles, low nibble first, interleaved between channels 4 bytes at a time. The header sample is the block's first
 * frame, the first 63 nibbles the rest. */
static void decode_block(SoundBuffer *b, int block)
{
    int channels = b->channels > 2 ? 2 : b->channels;
    const unsigned char *src = b->data + block * ADPCM_BLOCK * b->channels;
    for (int c = 0; c < channels; c++) {
        const unsigned char *header = src + c * 4;
        int predictor = (short)(header[0] | header[1] << 8), index = header[2];
        if (index > 88)
            index = 88;
        b->cache[c] = (short)predictor;
        for (int n = 1; n < ADPCM_SAMPLES; n++) {
            int k = n - 1;  /* nibble number */
            const unsigned char *byte = src + b->channels * 4 + (k / 8) * 4 * b->channels + c * 4 + (k % 8) / 2;
            int nibble = k & 1 ? *byte >> 4 : *byte & 15;
            int step = ima_step[index], diff = step >> 3;
            if (nibble & 4)
                diff += step;
            if (nibble & 2)
                diff += step >> 1;
            if (nibble & 1)
                diff += step >> 2;
            predictor += nibble & 8 ? -diff : diff;
            if (predictor > 32767)
                predictor = 32767;
            if (predictor < -32768)
                predictor = -32768;
            index += ima_index[nibble];
            if (index < 0)
                index = 0;
            if (index > 88)
                index = 88;
            b->cache[n * channels + c] = (short)predictor;
        }
    }
    b->cached_block = block;
}

/* one frame as left and right, in -1..1 (a mono sound gives the same sample twice) */
static void read_frame(SoundBuffer *b, DWORD frame, float *left, float *right)
{
    float l, r;
    if (adpcm(b)) {
        int block = (int)(frame / ADPCM_SAMPLES), i = (int)(frame % ADPCM_SAMPLES);
        if (b->cached_block != block)
            decode_block(b, block);
        int channels = b->channels > 2 ? 2 : b->channels;
        l = b->cache[i * channels] * (1.0f / 32768.0f);
        r = channels > 1 ? b->cache[i * channels + 1] * (1.0f / 32768.0f) : l;
    } else if (b->bits == 16) {
        const short *s = (const short *)(b->data + frame * b->block_align);
        l = s[0] * (1.0f / 32768.0f);
        r = b->channels > 1 ? s[1] * (1.0f / 32768.0f) : l;
    } else {
        const unsigned char *s = b->data + frame * b->block_align;
        l = (s[0] - 128) * (1.0f / 128.0f);
        r = b->channels > 1 ? (s[1] - 128) * (1.0f / 128.0f) : l;
    }
    *left = l;
    *right = r;
}

/* --------------------------------------------------------------------------------------------- the mixer -- */

static void mix_buffer(SoundBuffer *b, float *out, int frames)
{
    DWORD total = frame_count(b);
    if (!total || (b->tag != WAVE_FORMAT_PCM && !adpcm(b))) {
        b->playing = 0;
        return;
    }
    double step = (double)(b->frequency ? b->frequency : b->rate) / OUTPUT_RATE;
    float gain_l = b->volume * b->bin[0], gain_r = b->volume * b->bin[1];
    if (b->is3d) {
        gain_l = b->volume * b->gain3d[0];
        gain_r = b->volume * b->gain3d[1];
    }
    double pos = b->position;
    for (int i = 0; i < frames; i++) {
        if (pos >= total) {
            if (!b->looping) {
                b->playing = 0;
                pos = 0;
                break;
            }
            pos -= total * (DWORD)(pos / total);
        }
        DWORD f = (DWORD)pos;
        float frac = (float)(pos - f), l0, r0, l1 = 0, r1 = 0;
        read_frame(b, f, &l0, &r0);
        if (f + 1 < total)
            read_frame(b, f + 1, &l1, &r1);
        else if (b->looping)
            read_frame(b, 0, &l1, &r1);
        out[2 * i] += (l0 + (l1 - l0) * frac) * gain_l;
        out[2 * i + 1] += (r0 + (r1 - r0) * frac) * gain_r;
        pos += step;
    }
    b->position = pos;
}

static HANDLE dump;  /* DRA2_SOUNDDUMP: everything mixed, as a 48 kHz stereo WAV */
static DWORD dumped;

static void open_dump(void)
{
    char path[MAX_PATH];
    if (!GetEnvironmentVariableA("DRA2_SOUNDDUMP", path, sizeof path))
        return;
    dump = CreateFileA(path, 0x40000000 /* GENERIC_WRITE */, 1, 0, 2 /* CREATE_ALWAYS */, 0x80, 0);
    if (dump == INVALID_HANDLE_VALUE)
        dump = 0;
}

static void write_dump(const short *samples, int frames)
{
    /* the header is rewritten every time, so a run that is killed still leaves a valid file */
    struct { char riff[4]; DWORD size; char wave[4], fmt[4]; DWORD fmt_size; WAVEFORMAT_X format; char data[4];
             DWORD data_size; } __attribute__((packed)) header = {
        "RIF", 0, "WAV", "fmt", 16, { WAVE_FORMAT_PCM, 2, OUTPUT_RATE, OUTPUT_RATE * 4, 4, 16, 0 }, "dat", 0 };
    DWORD written;
    header.riff[3] = 'F', header.wave[3] = 'E', header.fmt[3] = ' ', header.data[3] = 'a';
    dumped += frames * 4;
    header.data_size = dumped;
    header.size = dumped + 36;
    SetFilePointer(dump, 0, 0, 0);
    WriteFile(dump, &header, 44, &written, 0);
    SetFilePointer(dump, 0, 0, 2 /* FILE_END */);
    WriteFile(dump, samples, frames * 4, &written, 0);
}

static void mix(short *out, int frames)
{
    static float accumulate[CHUNK_FRAMES * 2];
    static short scratch[CHUNK_FRAMES * 2];
    for (int i = 0; i < frames * 2; i++)
        accumulate[i] = 0.0f;
    EnterCriticalSection(&lock);
    for (SoundBuffer *b = buffers; b; b = b->next)
        if (b->playing)
            mix_buffer(b, accumulate, frames);
    LeaveCriticalSection(&lock);
    if (!out && !dump)
        return;
    if (!out)
        out = scratch;
    for (int i = 0; i < frames * 2; i++) {
        float v = accumulate[i] * 0.8f * 32767.0f;
        out[i] = (short)(v > 32767.0f ? 32767 : v < -32768.0f ? -32768 : v);
    }
    if (dump)
        write_dump(out, frames);
}

static DWORD WINAPI mixer_thread(void *param)
{
    (void)param;
    static short memory[CHUNKS][CHUNK_FRAMES * 2];
    static WAVE_HEADER headers[CHUNKS];
    HANDLE device = 0, event = CreateEventA(0, 0, 0, 0);
    char mute[8];
    WAVEFORMAT_X format = { WAVE_FORMAT_PCM, 2, OUTPUT_RATE, OUTPUT_RATE * 4, 4, 16, 0 };
    if (!GetEnvironmentVariableA("DRA2_MUTE", mute, sizeof mute)) {
        HMODULE winmm = LoadLibraryA("winmm.dll");
        if (winmm) {
            wave_open = GetProcAddress(winmm, "waveOutOpen");
            wave_prepare = GetProcAddress(winmm, "waveOutPrepareHeader");
            wave_write = GetProcAddress(winmm, "waveOutWrite");
        }
        if (wave_open && wave_prepare && wave_write &&
            wave_open(&device, (unsigned)-1 /* WAVE_MAPPER */, &format, (DWORD)event, 0, CALLBACK_EVENT) != 0)
            device = 0;
        if (!device)
            printf("[dsound] no audio output device: sound is silent\n");
    }
    if (device) {
        for (int i = 0; i < CHUNKS; i++) {
            headers[i].data = (char *)memory[i];
            headers[i].length = sizeof memory[i];
            wave_prepare(device, &headers[i], sizeof headers[i]);
            mix(memory[i], CHUNK_FRAMES);
            wave_write(device, &headers[i], sizeof headers[i]);
        }
        for (;;) {
            WaitForSingleObject(event, 50);
            for (int i = 0; i < CHUNKS; i++) {
                if (headers[i].flags & WHDR_DONE) {
                    mix(memory[i], CHUNK_FRAMES);
                    wave_write(device, &headers[i], sizeof headers[i]);
                }
            }
        }
    }
    /* no device: keep the play cursors moving in real time */
    LONGLONG frequency, last, now;
    QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&last);
    double owed = 0;
    for (;;) {
        Sleep(10);
        QueryPerformanceCounter(&now);
        owed += (double)(now - last) * OUTPUT_RATE / frequency;
        last = now;
        while (owed >= 1) {
            int n = owed > CHUNK_FRAMES ? CHUNK_FRAMES : (int)owed;
            mix(0, n);
            owed -= n;
        }
    }
    return 0;
}

static void start_mixer(void)
{
    if (started)
        return;
    started = 1;
    char value[8];
    log_formats = GetEnvironmentVariableA("DRA2_LOGSOUND", value, sizeof value) != 0;
    InitializeCriticalSection(&lock);
    open_dump();
    HANDLE thread = CreateThread(0, 0, mixer_thread, 0, 0, 0);
    if (thread)
        SetThreadPriority(thread, 2 /* THREAD_PRIORITY_HIGHEST */);
}

/* ------------------------------------------------------------------------------------------------ the API -- */

static void set_format(SoundBuffer *b, const WAVEFORMAT_X *f)
{
    if (!f)
        return;
    b->tag = f->tag;
    b->channels = f->channels;
    b->rate = f->rate;
    b->bits = f->bits;
    b->block_align = f->block_align ? f->block_align : (WORD)(f->channels * f->bits / 8);
    b->cached_block = -1;
}

static SoundBuffer *new_buffer(const XDSBUFFERDESC *desc)
{
    start_mixer();
    SoundBuffer *b = malloc(sizeof *b);
    if (!b)
        return 0;
    unsigned char *zero = (unsigned char *)b;
    for (unsigned i = 0; i < sizeof *b; i++)
        zero[i] = 0;
    b->volume = b->bin[0] = b->bin[1] = b->gain3d[0] = b->gain3d[1] = 1.0f;
    b->cached_block = -1;
    if (desc) {
        set_format(b, desc->format);
        b->is3d = (desc->flags & DSBCAPS_CTRL3D) != 0;
        b->bytes = desc->buffer_bytes;
        if (b->bytes) {
            b->data = malloc(b->bytes);
            b->owned = 1;
            for (DWORD i = 0; b->data && i < b->bytes; i++)
                b->data[i] = b->bits == 8 ? 0x80 : 0;
        }
        if (log_formats && desc->format)
            printf("[dsound] buffer %p: format %x, %u channels, %u Hz, %u bits, %u bytes%s\n", (void *)b, b->tag,
                   b->channels, (unsigned)b->rate, b->bits, (unsigned)b->bytes, b->is3d ? ", 3D" : "");
    }
    EnterCriticalSection(&lock);
    b->next = buffers;
    if (buffers)
        buffers->prev = b;
    buffers = b;
    LeaveCriticalSection(&lock);
    return b;
}

int __stdcall DSOUND__DirectSoundCreate(void *guid, void **device, void *outer)
{
    (void)guid, (void)outer;
    start_mixer();
    if (device)
        *device = &sound_device;
    return 0;
}

int __stdcall DSOUND__IDirectSound_CreateSoundBuffer(void *device, const XDSBUFFERDESC *desc, void **buffer, void *outer)
{
    (void)device, (void)outer;
    SoundBuffer *b = new_buffer(desc);
    if (buffer)
        *buffer = b;
    return b ? 0 : 0x8007000e;  /* E_OUTOFMEMORY */
}

int __stdcall DSOUND__DirectSoundCreateBuffer(const XDSBUFFERDESC *desc, void **buffer)
{
    return DSOUND__IDirectSound_CreateSoundBuffer(&sound_device, desc, buffer, 0);
}

int __stdcall DSOUND__IDirectSoundBuffer_GetStatus(SoundBuffer *b, DWORD *status)
{
    if (status) {
        EnterCriticalSection(&lock);
        *status = b && b->playing ? 1 | (b->looping ? 4 : 0) : 0;  /* DSBSTATUS_PLAYING | DSBSTATUS_LOOPING */
        LeaveCriticalSection(&lock);
    }
    return 0;
}

int __stdcall DSOUND__IDirectSoundBuffer_GetCurrentPosition(SoundBuffer *b, DWORD *play, DWORD *write)
{
    DWORD p = 0, w = 0;
    if (b) {
        EnterCriticalSection(&lock);
        p = frames_to_bytes(b, (DWORD)b->position);
        /* the write cursor: where the hardware has finished reading, about one mixer block ahead */
        w = b->bytes ? (p + frames_to_bytes(b, (b->rate ? b->rate : OUTPUT_RATE) / 100)) % b->bytes : 0;
        LeaveCriticalSection(&lock);
    }
    if (play)
        *play = p;
    if (write)
        *write = w;
    return 0;
}

int __stdcall DSOUND__IDirectSoundBuffer_SetCurrentPosition(SoundBuffer *b, DWORD position)
{
    if (b) {
        EnterCriticalSection(&lock);
        b->position = bytes_to_frames(b, position);
        LeaveCriticalSection(&lock);
    }
    return 0;
}

/* Lock(offset, bytes): the region may wrap around the end of the buffer into a second part */
int __stdcall DSOUND__IDirectSoundBuffer_Lock(SoundBuffer *b, DWORD offset, DWORD bytes, void **part1, DWORD *bytes1,
                                              void **part2, DWORD *bytes2, DWORD flags)
{
    (void)flags;
    if (part2)
        *part2 = 0;
    if (bytes2)
        *bytes2 = 0;
    if (!b) {
        if (part1)
            *part1 = 0;
        if (bytes1)
            *bytes1 = 0;
        return 0x80070057;  /* E_INVALIDARG */
    }
    EnterCriticalSection(&lock);
    DWORD first = bytes;
    if (b->bytes && offset < b->bytes && offset + bytes > b->bytes && b->owned) {
        first = b->bytes - offset;  /* wraps */
        if (part2)
            *part2 = b->data;
        if (bytes2)
            *bytes2 = bytes - first;
    } else if (offset + bytes > b->bytes) {  /* a buffer created without a size: give it one */
        unsigned char *grown = b->owned ? realloc(b->data, offset + bytes) : malloc(offset + bytes);
        if (!grown) {
            LeaveCriticalSection(&lock);
            return 0x8007000e;
        }
        b->data = grown;
        b->owned = 1;
        b->bytes = offset + bytes;
        b->cached_block = -1;
    }
    if (part1)
        *part1 = b->data + offset;
    if (bytes1)
        *bytes1 = first;
    LeaveCriticalSection(&lock);
    return 0;
}

int __stdcall DSOUND__IDirectSoundBuffer_Unlock(SoundBuffer *b, void *part1, DWORD bytes1, void *part2, DWORD bytes2)
{
    (void)part1, (void)bytes1, (void)part2, (void)bytes2;
    if (b) {
        EnterCriticalSection(&lock);
        b->cached_block = -1;
        LeaveCriticalSection(&lock);
    }
    return 0;
}

int __stdcall DSOUND__IDirectSoundBuffer_Release(SoundBuffer *b)
{
    if (b) {
        EnterCriticalSection(&lock);
        if (b->prev)
            b->prev->next = b->next;
        else
            buffers = b->next;
        if (b->next)
            b->next->prev = b->prev;
        LeaveCriticalSection(&lock);
        if (b->owned)
            free(b->data);
        free(b);
    }
    return 0;
}

int __stdcall DSOUND__IDirectSoundBuffer_Play(SoundBuffer *b, DWORD r1, DWORD r2, DWORD flags)
{
    (void)r1, (void)r2;
    if (b) {
        EnterCriticalSection(&lock);
        b->looping = (flags & 1) != 0;  /* DSBPLAY_LOOPING */
        b->playing = 1;
        LeaveCriticalSection(&lock);
    }
    return 0;
}

int __stdcall DSOUND__IDirectSoundBuffer_Stop(SoundBuffer *b)
{
    if (b) {
        EnterCriticalSection(&lock);
        b->playing = b->looping = 0;
        LeaveCriticalSection(&lock);
    }
    return 0;
}

int __stdcall DSOUND__IDirectSoundBuffer_StopEx(SoundBuffer *b, DWORD t1, DWORD t2, DWORD flags)
{
    (void)t1, (void)t2, (void)flags;
    return DSOUND__IDirectSoundBuffer_Stop(b);
}

/* SetBufferData: play the game's own memory (a sample resource) from now on */
int __stdcall DSOUND__IDirectSoundBuffer_SetBufferData(SoundBuffer *b, void *data, DWORD bytes)
{
    if (b) {
        EnterCriticalSection(&lock);
        if (b->owned)
            free(b->data);
        b->data = data;
        b->bytes = data ? bytes : 0;
        b->owned = 0;
        b->position = 0;
        b->cached_block = -1;
        LeaveCriticalSection(&lock);
    }
    return 0;
}

int __stdcall DSOUND__IDirectSoundBuffer_SetFrequency(SoundBuffer *b, DWORD hz)
{
    if (b)
        b->frequency = hz;
    return 0;
}

int __stdcall DSOUND__IDirectSoundBuffer_SetVolume(SoundBuffer *b, long volume)
{
    if (b)
        b->volume = centibels(volume);
    return 0;
}

int __stdcall DSOUND__IDirectSoundBuffer_SetMixBinVolumes_8(SoundBuffer *b, const XDSMIXBINS *bins)
{
    if (b && bins && bins->pairs)
        for (DWORD i = 0; i < bins->count; i++)
            if (bins->pairs[i].bin < 2)
                b->bin[bins->pairs[i].bin] = centibels(bins->pairs[i].volume);
    return 0;
}

/* a 3D voice's position relative to the listener: the default DirectSound rolloff (minimum distance 1) and a pan */
int __stdcall DSOUND__IDirectSoundBuffer_SetPosition(SoundBuffer *b, DWORD x, DWORD y, DWORD z, DWORD apply)
{
    (void)apply;
    if (!b)
        return 0;
    union { DWORD bits; float f; } fx = { x }, fy = { y }, fz = { z };
    float distance = square_root(fx.f * fx.f + fy.f * fy.f + fz.f * fz.f);
    float gain = distance > 1.0f ? 1.0f / distance : 1.0f;
    float pan = distance > 0.001f ? fx.f / distance : 0.0f;  /* -1 left .. 1 right */
    b->gain3d[0] = gain * (pan > 0 ? 1.0f - 0.7f * pan : 1.0f);
    b->gain3d[1] = gain * (pan < 0 ? 1.0f + 0.7f * pan : 1.0f);
    return 0;
}

/* everything else only changes how a sound would be heard, in ways not reproduced */
int __stdcall DSOUND__IDirectSoundBuffer_Restore(void *b) { (void)b; return 0; }
int __stdcall DSOUND__IDirectSoundBuffer_SetEG(void *b, void *eg) { (void)b, (void)eg; return 0; }
int __stdcall DSOUND__IDirectSoundBuffer_SetMixBins(void *b, void *bins) { (void)b, (void)bins; return 0; }
int __stdcall DSOUND__IDirectSound_CommitDeferredSettings(void *device) { (void)device; return 0; }
int __stdcall DSOUND__IDirectSound_Release(void *device) { (void)device; return 0; }
int __stdcall DSOUND__IDirectSound_SetI3DL2Listener(void *device, void *listener, DWORD apply) { (void)device, (void)listener, (void)apply; return 0; }
int __stdcall DSOUND__IDirectSound_SetOrientation(void *device, DWORD fx, DWORD fy, DWORD fz, DWORD tx, DWORD ty, DWORD tz, DWORD apply) { (void)device, (void)fx, (void)fy, (void)fz, (void)tx, (void)ty, (void)tz, (void)apply; return 0; }
int __stdcall DSOUND__IDirectSound_SetVelocity(void *device, DWORD x, DWORD y, DWORD z, DWORD apply) { (void)device, (void)x, (void)y, (void)z, (void)apply; return 0; }
void __stdcall DSOUND__DirectSoundDoWork(void) {}
void __stdcall DSOUND__DirectSoundUseFullHRTF(void) {}
DWORD __stdcall DSOUND__XAudioDownloadEffectsImage(void *name, void *location, DWORD flags, void **image) { (void)name, (void)location, (void)flags; if (image) *image = 0; return 0; }
