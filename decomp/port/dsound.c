/* Xbox DirectSound, silent: the objects exist and behave, nothing is heard yet.
 *
 * The generated stubs (xbox_stubs.c) returned success without filling their out parameters, so the game got no
 * buffer pointers and read uninitialised memory as each buffer's status. Its "playing" bit was set, every sound
 * seemed to play forever, and anything that waits for a sound to end waited forever: menu panels only finish once
 * their sounds stop (FUN_000c8b90 -> FUN_0011b790), so the title's panels and the attract mode's challenger screen
 * never completed. Here a buffer is a real object with memory to fill. A sound played once reports itself stopped at
 * once; a looping one plays until it is stopped, as on the Xbox: the game's sound objects (FUN_00166f00) take a looping
 * sound that stopped by itself as never keyed off, and releasing it is then a fatal error (FUN_00166810). */
void *malloc(unsigned size);
void *realloc(void *p, unsigned size);
void free(void *p);

typedef unsigned long DWORD;

/* Xbox DSBUFFERDESC: dwSize, dwFlags, dwBufferBytes, lpwfxFormat, lpMixBins, dwInputMixBin */
typedef struct { DWORD size, flags, buffer_bytes; void *format, *mix_bins; DWORD input_mix_bin; } XDSBUFFERDESC;

typedef struct {
    unsigned char *data;  /* what Lock hands out; the game writes its samples here */
    DWORD bytes;
    DWORD play, write;
    int looping;  /* played with DSBPLAY_LOOPING and not stopped since */
} SoundBuffer;

static int sound_device;  /* the IDirectSound object: only its address is used */

static SoundBuffer *new_buffer(const XDSBUFFERDESC *desc)
{
    SoundBuffer *b = malloc(sizeof *b);
    if (!b)
        return 0;
    b->bytes = desc ? desc->buffer_bytes : 0;
    b->data = b->bytes ? malloc(b->bytes) : 0;
    b->play = b->write = 0;
    b->looping = 0;
    return b;
}

int __stdcall DSOUND__DirectSoundCreate(void *guid, void **device, void *outer)
{
    (void)guid, (void)outer;
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
    if (status)
        *status = b && b->looping ? 1 | 4 : 0;  /* DSBSTATUS_PLAYING | DSBSTATUS_LOOPING, or stopped */
    return 0;
}

int __stdcall DSOUND__IDirectSoundBuffer_GetCurrentPosition(SoundBuffer *b, DWORD *play, DWORD *write)
{
    if (play)
        *play = b ? b->play : 0;
    if (write)
        *write = b ? b->write : 0;
    return 0;
}

int __stdcall DSOUND__IDirectSoundBuffer_SetCurrentPosition(SoundBuffer *b, DWORD position)
{
    if (b)
        b->play = b->write = position;
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
    if (offset + bytes > b->bytes) {  /* a buffer created without a size, or data set later: grow it */
        unsigned char *grown = realloc(b->data, offset + bytes);
        if (!grown)
            return 0x8007000e;
        b->data = grown;
        b->bytes = offset + bytes;
    }
    if (part1)
        *part1 = b->data + offset;
    if (bytes1)
        *bytes1 = bytes;
    return 0;
}

int __stdcall DSOUND__IDirectSoundBuffer_Unlock(SoundBuffer *b, void *part1, DWORD bytes1, void *part2, DWORD bytes2)
{
    (void)b, (void)part1, (void)bytes1, (void)part2, (void)bytes2;
    return 0;
}

int __stdcall DSOUND__IDirectSoundBuffer_Release(SoundBuffer *b)
{
    if (b) {
        free(b->data);
        free(b);
    }
    return 0;
}

int __stdcall DSOUND__IDirectSoundBuffer_Play(SoundBuffer *b, DWORD r1, DWORD r2, DWORD flags)
{
    (void)r1, (void)r2;
    if (b)
        b->looping = (flags & 1) != 0;  /* DSBPLAY_LOOPING */
    return 0;
}

int __stdcall DSOUND__IDirectSoundBuffer_Stop(SoundBuffer *b)
{
    if (b)
        b->looping = 0;
    return 0;
}

int __stdcall DSOUND__IDirectSoundBuffer_StopEx(SoundBuffer *b, DWORD t1, DWORD t2, DWORD flags)
{
    (void)t1, (void)t2, (void)flags;
    if (b)
        b->looping = 0;
    return 0;
}

/* everything else only changes how a sound would be heard */
int __stdcall DSOUND__IDirectSoundBuffer_Restore(void *b) { (void)b; return 0; }
int __stdcall DSOUND__IDirectSoundBuffer_SetBufferData(void *b, void *data, DWORD bytes) { (void)b, (void)data, (void)bytes; return 0; }
int __stdcall DSOUND__IDirectSoundBuffer_SetEG(void *b, void *eg) { (void)b, (void)eg; return 0; }
int __stdcall DSOUND__IDirectSoundBuffer_SetFrequency(void *b, DWORD hz) { (void)b, (void)hz; return 0; }
int __stdcall DSOUND__IDirectSoundBuffer_SetMixBinVolumes_8(void *b, void *volumes) { (void)b, (void)volumes; return 0; }
int __stdcall DSOUND__IDirectSoundBuffer_SetMixBins(void *b, void *bins) { (void)b, (void)bins; return 0; }
int __stdcall DSOUND__IDirectSoundBuffer_SetPosition(void *b, DWORD x, DWORD y, DWORD z, DWORD apply) { (void)b, (void)x, (void)y, (void)z, (void)apply; return 0; }
int __stdcall DSOUND__IDirectSoundBuffer_SetVolume(void *b, DWORD volume) { (void)b, (void)volume; return 0; }
int __stdcall DSOUND__IDirectSound_CommitDeferredSettings(void *device) { (void)device; return 0; }
int __stdcall DSOUND__IDirectSound_Release(void *device) { (void)device; return 0; }
int __stdcall DSOUND__IDirectSound_SetI3DL2Listener(void *device, void *listener, DWORD apply) { (void)device, (void)listener, (void)apply; return 0; }
int __stdcall DSOUND__IDirectSound_SetOrientation(void *device, DWORD fx, DWORD fy, DWORD fz, DWORD tx, DWORD ty, DWORD tz, DWORD apply) { (void)device, (void)fx, (void)fy, (void)fz, (void)tx, (void)ty, (void)tz, (void)apply; return 0; }
int __stdcall DSOUND__IDirectSound_SetVelocity(void *device, DWORD x, DWORD y, DWORD z, DWORD apply) { (void)device, (void)x, (void)y, (void)z, (void)apply; return 0; }
void __stdcall DSOUND__DirectSoundDoWork(void) {}
void __stdcall DSOUND__DirectSoundUseFullHRTF(void) {}
DWORD __stdcall DSOUND__XAudioDownloadEffectsImage(void *name, void *location, DWORD flags, void **image) { (void)name, (void)location, (void)flags; if (image) *image = 0; return 0; }
