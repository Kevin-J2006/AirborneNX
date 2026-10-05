#include "opensles.h"
#include "pthr.h"
#include "../utils/logger.h"
#include <switch.h>
#include <malloc.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

// ============================================================================
// OpenSL ES Interface UUIDs (Khronos Standard & Android Extensions)
// ============================================================================
const struct SLInterfaceID_ SL_IID_ENGINE_val                   = { 0x8d2e6040, 0x9e99, 0x11df, 0x9f5e, { 0x00, 0x02, 0xa5, 0xd5, 0xc5, 0x1b } };
const struct SLInterfaceID_ SL_IID_PLAY_val                     = { 0xef064d60, 0x9e99, 0x11df, 0xa4a7, { 0x00, 0x02, 0xa5, 0xd5, 0xc5, 0x1b } };
const struct SLInterfaceID_ SL_IID_BUFFERQUEUE_val              = { 0x3d0269e0, 0x9e9a, 0x11df, 0x829e, { 0x00, 0x02, 0xa5, 0xd5, 0xc5, 0x1b } };
const struct SLInterfaceID_ SL_IID_ANDROIDSIMPLEBUFFERQUEUE_val = { 0x198e1a4a, 0x1fed, 0x4e0a, 0x9452, { 0x48, 0x82, 0x37, 0x3d, 0x6e, 0x8e } };
const struct SLInterfaceID_ SL_IID_ANDROIDCONFIGURATION_val     = { 0x89791480, 0xa87c, 0x11df, 0x8b67, { 0x00, 0x02, 0xa5, 0xd5, 0xc5, 0x1b } };
const struct SLInterfaceID_ SL_IID_VOLUME_val                   = { 0x09e8eda0, 0x9e99, 0x11df, 0xbc35, { 0x00, 0x02, 0xa5, 0xd5, 0xc5, 0x1b } };
const struct SLInterfaceID_ SL_IID_SEEK_val                     = { 0x4632db20, 0x9e99, 0x11df, 0x97d6, { 0x00, 0x02, 0xa5, 0xd5, 0xc5, 0x1b } };
const struct SLInterfaceID_ SL_IID_RECORD_val                   = { 0xc437a380, 0x9e99, 0x11df, 0x9078, { 0x00, 0x02, 0xa5, 0xd5, 0xc5, 0x1b } };
const struct SLInterfaceID_ SL_IID_PREFETCHSTATUS_val           = { 0x66ff8f80, 0x9e99, 0x11df, 0x9212, { 0x00, 0x02, 0xa5, 0xd5, 0xc5, 0x1b } };
const struct SLInterfaceID_ SL_IID_METADATAEXTRACTION_val       = { 0x4e082720, 0x9e99, 0x11df, 0x945d, { 0x00, 0x02, 0xa5, 0xd5, 0xc5, 0x1b } };
const struct SLInterfaceID_ SL_IID_PITCH_val                    = { 0x80806480, 0x9e99, 0x11df, 0xa664, { 0x00, 0x02, 0xa5, 0xd5, 0xc5, 0x1b } };

// The exported SL_IID_* symbols are variables of type SLInterfaceID (a pointer).
SLInterfaceID SL_IID_ENGINE_ptr                   = &SL_IID_ENGINE_val;
SLInterfaceID SL_IID_PLAY_ptr                     = &SL_IID_PLAY_val;
SLInterfaceID SL_IID_BUFFERQUEUE_ptr              = &SL_IID_BUFFERQUEUE_val;
SLInterfaceID SL_IID_ANDROIDSIMPLEBUFFERQUEUE_ptr = &SL_IID_ANDROIDSIMPLEBUFFERQUEUE_val;
SLInterfaceID SL_IID_ANDROIDCONFIGURATION_ptr     = &SL_IID_ANDROIDCONFIGURATION_val;
SLInterfaceID SL_IID_VOLUME_ptr                   = &SL_IID_VOLUME_val;
SLInterfaceID SL_IID_SEEK_ptr                     = &SL_IID_SEEK_val;
SLInterfaceID SL_IID_RECORD_ptr                   = &SL_IID_RECORD_val;
SLInterfaceID SL_IID_PREFETCHSTATUS_ptr           = &SL_IID_PREFETCHSTATUS_val;
SLInterfaceID SL_IID_METADATAEXTRACTION_ptr       = &SL_IID_METADATAEXTRACTION_val;
SLInterfaceID SL_IID_PITCH_ptr                    = &SL_IID_PITCH_val;

#define OUTPUT_RATE     48000
#define OUTPUT_CHANNELS 2
#define MAX_PLAYERS     16
#define MAX_QUEUED      32

// Output: periods of 1024 frames (21 ms), three of them queued in audout.
#define OUTPUT_FRAMES   1024
#define OUTPUT_BUFFERS  3
#define OUTPUT_BYTES    (OUTPUT_FRAMES * OUTPUT_CHANNELS * 2)
#define OUTPUT_STACK    0x40000
#define OUTPUT_WAIT_NS  100000000ULL  // how long to wait for a period to finish
#define OUTPUT_STALLED  30            // waits in a row with nothing played: restart

static bool s_audio_inited = false;
static Thread s_output_thread;
static AudioOutBuffer s_output[OUTPUT_BUFFERS];

// ============================================================================
// Standard Khronos OpenSL ES 1.0.1 VTable Definitions (Strict ABI Compliance)
// ============================================================================

struct SLObjectItf_ {
    uint32_t (*Realize)(void *self, uint32_t async);
    uint32_t (*Resume)(void *self, uint32_t async);
    uint32_t (*GetState)(void *self, uint32_t *pState);
    uint32_t (*GetInterface)(void *self, const SLInterfaceID iid, void *pInterface);
    uint32_t (*RegisterCallback)(void *self, void *callback, void *pContext);
    void     (*AbortAsyncOperation)(void *self);
    void     (*Destroy)(void *self);
    uint32_t (*SetPriority)(void *self, int32_t priority, uint32_t preemptable);
    uint32_t (*GetPriority)(void *self, int32_t *pPriority, uint32_t *pPreemptable);
    uint32_t (*SetLossOfControlInterfaces)(void *self, uint32_t numInterfaces, const SLInterfaceID *pInterfaceIDs, uint32_t enabled);
};

struct SLEngineItf_ {
    uint32_t (*CreateLEDDevice)(void *self, void **pDevice, uint32_t deviceID, uint32_t numInterfaces, const SLInterfaceID *pInterfaceIds, const uint32_t *pInterfaceRequired);
    uint32_t (*CreateVibraDevice)(void *self, void **pDevice, uint32_t deviceID, uint32_t numInterfaces, const SLInterfaceID *pInterfaceIds, const uint32_t *pInterfaceRequired);
    uint32_t (*CreateAudioPlayer)(void *self, void **pPlayer, void *pAudioSrc, void *pAudioSnk, uint32_t numInterfaces, const SLInterfaceID *pInterfaceIds, const uint32_t *pInterfaceRequired);
    uint32_t (*CreateAudioRecorder)(void *self, void **pRecorder, void *pAudioSrc, void *pAudioSnk, uint32_t numInterfaces, const SLInterfaceID *pInterfaceIds, const uint32_t *pInterfaceRequired);
    uint32_t (*CreateMidiPlayer)(void *self, void **pPlayer, void *pAudioSrc, void *pBankSrc, void *pAudioSnk, void *pMidiSnk, void *pVibra, uint32_t numInterfaces, const SLInterfaceID *pInterfaceIds, const uint32_t *pInterfaceRequired);
    uint32_t (*CreateListener)(void *self, void **pListener, uint32_t numInterfaces, const SLInterfaceID *pInterfaceIds, const uint32_t *pInterfaceRequired);
    uint32_t (*Create3DGroup)(void *self, void **pGroup, uint32_t numInterfaces, const SLInterfaceID *pInterfaceIds, const uint32_t *pInterfaceRequired);
    uint32_t (*CreateOutputMix)(void *self, void **pMix, uint32_t numInterfaces, const SLInterfaceID *pInterfaceIds, const uint32_t *pInterfaceRequired);
    uint32_t (*CreateMetadataExtractor)(void *self, void **pMetadataExtractor, void *pDataSource, uint32_t numInterfaces, const SLInterfaceID *pInterfaceIds, const uint32_t *pInterfaceRequired);
    uint32_t (*CreateExtensionObject)(void *self, void **pObject, void *pParameters, uint32_t objectID, uint32_t numInterfaces, const SLInterfaceID *pInterfaceIds, const uint32_t *pInterfaceRequired);
    uint32_t (*QueryNumSupportedEngineInterfaces)(void *self, uint32_t *pNumSupportedInterfaces);
    uint32_t (*QuerySupportedEngineInterface)(void *self, uint32_t index, SLInterfaceID *pInterfaceId);
};

struct SLPlayItf_ {
    uint32_t (*SetPlayState)(void *self, uint32_t state);
    uint32_t (*GetPlayState)(void *self, uint32_t *pState);
    uint32_t (*GetDuration)(void *self, uint32_t *pMsec);
    uint32_t (*GetPosition)(void *self, uint32_t *pMsec);
    uint32_t (*RegisterCallback)(void *self, void *callback, void *pContext);
    uint32_t (*SetCallbackEventsMask)(void *self, uint32_t eventFlags);
    uint32_t (*GetCallbackEventsMask)(void *self, uint32_t *pEventFlags);
    uint32_t (*SetMarkerPosition)(void *self, uint32_t mSec);
    uint32_t (*ClearMarkerPosition)(void *self);
    uint32_t (*GetMarkerPosition)(void *self, uint32_t *pMsec);
    uint32_t (*SetPositionUpdatePeriod)(void *self, uint32_t mSec);
    uint32_t (*GetPositionUpdatePeriod)(void *self, uint32_t *pMsec);
};

struct SLBufferQueueItf_ {
    uint32_t (*Enqueue)(void *self, const void *pBuffer, uint32_t size);
    uint32_t (*Clear)(void *self);
    uint32_t (*GetState)(void *self, void *pState);
    uint32_t (*RegisterCallback)(void *self, void *callback, void *pContext);
};

struct SLVolumeItf_ {
    uint32_t (*SetVolumeLevel)(void *self, int16_t level);
    uint32_t (*GetVolumeLevel)(void *self, int16_t *pLevel);
    uint32_t (*GetMaxVolumeLevel)(void *self, int16_t *pMaxLevel);
    uint32_t (*SetMute)(void *self, uint32_t mute);
    uint32_t (*GetMute)(void *self, uint32_t *pMute);
    uint32_t (*EnableStereoPosition)(void *self, uint32_t enable);
    uint32_t (*IsEnabledStereoPosition)(void *self, uint32_t *pEnable);
    uint32_t (*SetStereoPosition)(void *self, int32_t position);
    uint32_t (*GetStereoPosition)(void *self, int32_t *pPosition);
};

struct SLSeekItf_ {
    uint32_t (*SetPosition)(void *self, uint32_t mSec, uint32_t seekMode);
    uint32_t (*SetLoop)(void *self, uint32_t loopEnable, uint32_t startPos, uint32_t endPos);
    uint32_t (*GetLoop)(void *self, uint32_t *pLoopEnable, uint32_t *pStartPos, uint32_t *pEndPos);
};

struct SLAndroidConfigurationItf_ {
    uint32_t (*SetConfiguration)(void *self, const char *configKey, const void *pConfigValue, uint32_t valueSize);
    uint32_t (*GetConfiguration)(void *self, const char *configKey, uint32_t *pValueSize, void *pConfigValue);
};

// Data source description passed to CreateAudioPlayer
typedef struct {
    void *pLocator;
    void *pFormat;
} SLDataSource;

typedef struct {
    uint32_t formatType;    // 2 = SL_DATAFORMAT_PCM
    uint32_t numChannels;
    uint32_t samplesPerSec; // in milliHertz
    uint32_t bitsPerSample;
    uint32_t containerSize;
    uint32_t channelMask;
    uint32_t endianness;
} SLDataFormat_PCM;

// ============================================================================
// Instance Wrapper Structs (Double-Pointer COM Layout)
// ============================================================================

typedef struct {
    const struct SLEngineItf_ *vtable;
} EngineItfInstance;

typedef struct {
    const struct SLObjectItf_ *vtable;
    EngineItfInstance engineItf;
} EngineObjectInstance;

typedef struct {
    const struct SLObjectItf_ *vtable;
} OutputMixObjectInstance;

typedef struct AudioPlayerObjectInstance AudioPlayerObjectInstance;

typedef struct {
    const struct SLPlayItf_ *vtable;
    AudioPlayerObjectInstance *player;
    uint32_t state;
} PlayItfInstance;

typedef struct {
    const void *data;
    uint32_t size;
} QueuedBuffer;

typedef struct {
    const struct SLBufferQueueItf_ *vtable;
    AudioPlayerObjectInstance *player;
    void (*callback)(void *caller, void *pContext);
    void *context;
    QueuedBuffer queue[MAX_QUEUED];
    uint32_t head;       // index of the buffer currently playing
    uint32_t count;      // buffers queued, including the playing one
    uint32_t offset;     // byte offset into the playing buffer
    uint32_t frac;       // resampling phase, 16.16 fixed point
    uint32_t play_index; // buffers fully played so far
} BufferQueueItfInstance;

typedef struct {
    const struct SLVolumeItf_ *vtable;
    AudioPlayerObjectInstance *player;
    int16_t volume;
    uint32_t mute;
} VolumeItfInstance;

typedef struct {
    const struct SLSeekItf_ *vtable;
    AudioPlayerObjectInstance *player;
    uint32_t loop;
} SeekItfInstance;

typedef struct {
    const struct SLAndroidConfigurationItf_ *vtable;
    AudioPlayerObjectInstance *player;
} AndroidConfigItfInstance;

struct AudioPlayerObjectInstance {
    const struct SLObjectItf_ *vtable;
    PlayItfInstance playItf;
    BufferQueueItfInstance bqItf;
    VolumeItfInstance volItf;
    SeekItfInstance seekItf;
    AndroidConfigItfInstance configItf;
    uint32_t channels;
    uint32_t rate;
    bool in_callback;   // the mixer is inside the guest's buffer callback
    bool destroyed;     // Destroy came in meanwhile: the mixer frees it
};

// All players, mixed together by the output thread; the lock guards the table
// and each player's queue and state.
//
// It is never held across the guest's buffer callback. The game's sound
// driver takes its own mutex in that callback, and takes the same mutex
// around its calls to SetPlayState when it suspends and resumes audio (every
// loading screen and pause). Calling back with this lock held is a lock-order
// inversion: the mixer waits for the driver's mutex while the thread holding
// it waits for this lock, and the sound never comes back.
static AudioPlayerObjectInstance *s_players[MAX_PLAYERS];
static RMutex s_audio_lock;

// Generic Safe Fallback Stub
static uint32_t OpenSL_StubSuccess(void) { return SL_RESULT_SUCCESS; }
static void OpenSL_StubVoid(void) {}

// ============================================================================
// Mixer
// ============================================================================
static inline int16_t clamp16(int32_t v) {
    if (v > 32767) return 32767;
    if (v < -32768) return -32768;
    return (int16_t)v;
}

// Mixes one player into `out`. Called with the lock held; drops it around the
// guest's callback. False if the player was destroyed meanwhile and is gone.
static bool mix_player(AudioPlayerObjectInstance *p, int16_t *out, int frames) {
    BufferQueueItfInstance *bq = &p->bqItf;
    if (p->playItf.state != SL_PLAYSTATE_PLAYING) return true;

    const uint32_t frame_bytes = p->channels * 2;
    const uint32_t step = (uint32_t)(((uint64_t)p->rate << 16) / OUTPUT_RATE);
    const bool muted = p->volItf.mute || p->volItf.volume <= -9600;
    int produced = 0;
    int guard = MAX_QUEUED * 4;

    while (produced < frames && bq->count > 0 && guard-- > 0) {
        QueuedBuffer *buf = &bq->queue[bq->head];
        const int16_t *src = (const int16_t *)buf->data;
        uint32_t src_frames = buf->size / frame_bytes;
        uint32_t pos = bq->offset / frame_bytes;

        while (produced < frames && pos < src_frames) {
            if (!muted) {
                int32_t l = src[pos * p->channels];
                int32_t r = (p->channels > 1) ? src[pos * p->channels + 1] : l;
                out[produced * 2]     = clamp16(out[produced * 2] + l);
                out[produced * 2 + 1] = clamp16(out[produced * 2 + 1] + r);
            }
            produced++;
            bq->frac += step;
            pos += bq->frac >> 16;
            bq->frac &= 0xFFFF;
        }
        bq->offset = pos * frame_bytes;

        if (pos >= src_frames) {
            bq->head = (bq->head + 1) % MAX_QUEUED;
            bq->count--;
            bq->offset = 0;
            bq->play_index++;
            // Buffer finished: this is the moment OpenSL ES notifies the app,
            // which normally responds by enqueueing the next buffer.
            void (*callback)(void *, void *) = bq->callback;
            void *context = bq->context;
            if (callback) {
                p->in_callback = true;
                rmutexUnlock(&s_audio_lock);
                callback((void *)bq, context);
                rmutexLock(&s_audio_lock);
                p->in_callback = false;
                if (p->destroyed) {
                    free(p);
                    return false;
                }
                // Paused or stopped while the callback ran: nothing more to
                // play from this player in this period.
                if (p->playItf.state != SL_PLAYSTATE_PLAYING) break;
            }
        }
    }
    return true;
}

// Fills one output period with whatever the players have queued.
static void mix_period(int16_t *out) {
    memset(out, 0, OUTPUT_BYTES);
    rmutexLock(&s_audio_lock);
    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (s_players[i]) mix_player(s_players[i], out, OUTPUT_FRAMES);
    }
    rmutexUnlock(&s_audio_lock);
}

// The output thread: keeps every buffer that audout has finished refilled and
// queued again.
//
// It holds no state that a late wake-up can invalidate. SDL's audio driver
// for the Switch, used here before, waits for the buffer it has just queued
// to reach the "playing" state; a thread that is kept off the CPU for a few
// tens of milliseconds (a loading screen is enough) finds it already played,
// waits for that state forever, and the game goes on without sound. Here a
// late thread only costs the periods that were missed.
static void output_main(void *arg) {
    (void)arg;
    pthr_enter_host_thread();

    bool queued[OUTPUT_BUFFERS] = { false };
    int stalled = 0;
    for (;;) {
        for (int i = 0; i < OUTPUT_BUFFERS; i++) {
            if (queued[i]) continue;
            mix_period((int16_t *)s_output[i].buffer);
            queued[i] = R_SUCCEEDED(audoutAppendAudioOutBuffer(&s_output[i]));
        }

        AudioOutBuffer *done = NULL;
        u32 count = 0;
        bool released = false;
        Result rc = audoutWaitPlayFinish(&done, &count, OUTPUT_WAIT_NS);
        // One buffer comes back per call, and the event behind the wait may
        // stand for several: collect them all, also after a timeout.
        for (;;) {
            if (R_SUCCEEDED(rc) && count > 0) {
                for (int i = 0; i < OUTPUT_BUFFERS; i++) {
                    if (done == &s_output[i]) queued[i] = false;
                }
                released = true;
            }
            done = NULL;
            count = 0;
            rc = audoutGetReleasedAudioOutBuffer(&done, &count);
            if (R_FAILED(rc) || count == 0) break;
        }

        // Nothing played for three seconds: the output itself has stopped.
        // Start it again and queue everything anew.
        stalled = released ? 0 : stalled + 1;
        if (stalled >= OUTPUT_STALLED) {
            l_warn("[OpenSL] audio output stalled, restarting it");
            audoutStopAudioOut();
            audoutStartAudioOut();
            memset(queued, 0, sizeof(queued));
            stalled = 0;
        }
    }
}

// ============================================================================
// Engine Object Implementation
// ============================================================================
static uint32_t EngineObj_Realize(void *self, uint32_t async) {
    (void)self; (void)async;
    l_debug("[OpenSL] EngineObj_Realize");
    return SL_RESULT_SUCCESS;
}

static uint32_t EngineObj_Resume(void *self, uint32_t async) {
    (void)self; (void)async;
    return SL_RESULT_SUCCESS;
}

static uint32_t EngineObj_GetState(void *self, uint32_t *pState) {
    (void)self;
    if (pState) *pState = 2; // SL_OBJECT_STATE_REALIZED
    return SL_RESULT_SUCCESS;
}

static uint32_t EngineObj_GetInterface(void *self, const SLInterfaceID iid, void *pInterface);
static void EngineObj_Destroy(void *self) { (void)self; }

static const struct SLObjectItf_ s_EngineObj_Vtbl = {
    EngineObj_Realize,
    EngineObj_Resume,
    EngineObj_GetState,
    EngineObj_GetInterface,
    (void *)OpenSL_StubSuccess, // RegisterCallback
    (void *)OpenSL_StubVoid,    // AbortAsyncOperation
    EngineObj_Destroy,
    (void *)OpenSL_StubSuccess, // SetPriority
    (void *)OpenSL_StubSuccess, // GetPriority
    (void *)OpenSL_StubSuccess  // SetLossOfControlInterfaces
};

// ============================================================================
// OutputMix Object Implementation
// ============================================================================
static uint32_t OutputMix_Realize(void *self, uint32_t async) {
    (void)self; (void)async;
    l_debug("[OpenSL] OutputMix_Realize");
    return SL_RESULT_SUCCESS;
}

static uint32_t OutputMix_GetInterface(void *self, const SLInterfaceID iid, void *pInterface) {
    (void)self; (void)iid;
    if (pInterface) *(void **)pInterface = NULL;
    return SL_RESULT_PARAMETER_INVALID;
}

static void OutputMix_Destroy(void *self) { (void)self; }

static const struct SLObjectItf_ s_OutputMixObj_Vtbl = {
    OutputMix_Realize,
    (void *)OpenSL_StubSuccess, // Resume
    EngineObj_GetState,
    OutputMix_GetInterface,
    (void *)OpenSL_StubSuccess, // RegisterCallback
    (void *)OpenSL_StubVoid,    // AbortAsyncOperation
    OutputMix_Destroy,
    (void *)OpenSL_StubSuccess, // SetPriority
    (void *)OpenSL_StubSuccess, // GetPriority
    (void *)OpenSL_StubSuccess  // SetLossOfControlInterfaces
};

static OutputMixObjectInstance s_globalOutputMix = {
    &s_OutputMixObj_Vtbl
};

// ============================================================================
// AudioPlayer Methods
// ============================================================================
static uint32_t PlayerObj_Realize(void *self, uint32_t async) {
    (void)self; (void)async;
    l_debug("[OpenSL] AudioPlayer realized");
    return SL_RESULT_SUCCESS;
}

static uint32_t PlayerObj_GetInterface(void *self, const SLInterfaceID iid, void *pInterface);

static void PlayerObj_Destroy(void *self) {
    l_debug("[OpenSL] AudioPlayer destroyed");
    AudioPlayerObjectInstance *player = (AudioPlayerObjectInstance *)self;
    rmutexLock(&s_audio_lock);
    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (s_players[i] == player) s_players[i] = NULL;
    }
    // The mixer may be inside this player's callback, with the lock released:
    // it frees the player when the callback returns.
    bool busy = player->in_callback;
    if (busy) player->destroyed = true;
    rmutexUnlock(&s_audio_lock);
    if (!busy) free(player);
}

static const struct SLObjectItf_ s_PlayerObj_Vtbl = {
    PlayerObj_Realize,
    (void *)OpenSL_StubSuccess, // Resume
    EngineObj_GetState,
    PlayerObj_GetInterface,
    (void *)OpenSL_StubSuccess, // RegisterCallback
    (void *)OpenSL_StubVoid,    // AbortAsyncOperation
    PlayerObj_Destroy,
    (void *)OpenSL_StubSuccess, // SetPriority
    (void *)OpenSL_StubSuccess, // GetPriority
    (void *)OpenSL_StubSuccess  // SetLossOfControlInterfaces
};

// 1. Play Interface
static uint32_t Play_SetPlayState(void *self, uint32_t state) {
    PlayItfInstance *play = (PlayItfInstance *)self;
    l_debug("[OpenSL] Play_SetPlayState: %u", state);
    rmutexLock(&s_audio_lock);
    play->state = state;
    if (state == SL_PLAYSTATE_STOPPED) {
        BufferQueueItfInstance *bq = &play->player->bqItf;
        bq->offset = 0;
        bq->frac = 0;
    }
    rmutexUnlock(&s_audio_lock);
    return SL_RESULT_SUCCESS;
}

static uint32_t Play_GetPlayState(void *self, uint32_t *pState) {
    PlayItfInstance *play = (PlayItfInstance *)self;
    if (pState) *pState = play->state;
    return SL_RESULT_SUCCESS;
}

static uint32_t Play_GetDuration(void *self, uint32_t *pMsec) {
    (void)self;
    if (pMsec) *pMsec = 0xFFFFFFFF; // SL_TIME_UNKNOWN
    return SL_RESULT_SUCCESS;
}

static uint32_t Play_GetPosition(void *self, uint32_t *pMsec) {
    (void)self;
    if (pMsec) *pMsec = 0;
    return SL_RESULT_SUCCESS;
}

static const struct SLPlayItf_ s_PlayItf_Vtbl = {
    Play_SetPlayState,
    Play_GetPlayState,
    Play_GetDuration,
    Play_GetPosition,
    (void *)OpenSL_StubSuccess, // RegisterCallback
    (void *)OpenSL_StubSuccess, // SetCallbackEventsMask
    (void *)OpenSL_StubSuccess, // GetCallbackEventsMask
    (void *)OpenSL_StubSuccess, // SetMarkerPosition
    (void *)OpenSL_StubSuccess, // ClearMarkerPosition
    (void *)OpenSL_StubSuccess, // GetMarkerPosition
    (void *)OpenSL_StubSuccess, // SetPositionUpdatePeriod
    (void *)OpenSL_StubSuccess  // GetPositionUpdatePeriod
};

// 2. BufferQueue Interface
static uint32_t BQ_Enqueue(void *self, const void *pBuffer, uint32_t size) {
    BufferQueueItfInstance *bq = (BufferQueueItfInstance *)self;
    if (!pBuffer || size == 0) return SL_RESULT_PARAMETER_INVALID;

    uint32_t result = SL_RESULT_SUCCESS;
    rmutexLock(&s_audio_lock);
    if (bq->count >= MAX_QUEUED) {
        result = SL_RESULT_BUFFER_INSUFFICIENT;
    } else {
        QueuedBuffer *slot = &bq->queue[(bq->head + bq->count) % MAX_QUEUED];
        slot->data = pBuffer;
        slot->size = size;
        bq->count++;
    }
    rmutexUnlock(&s_audio_lock);
    return result;
}

static uint32_t BQ_Clear(void *self) {
    BufferQueueItfInstance *bq = (BufferQueueItfInstance *)self;
    rmutexLock(&s_audio_lock);
    bq->head = 0;
    bq->count = 0;
    bq->offset = 0;
    bq->frac = 0;
    rmutexUnlock(&s_audio_lock);
    return SL_RESULT_SUCCESS;
}

static uint32_t BQ_GetState(void *self, void *pState) {
    BufferQueueItfInstance *bq = (BufferQueueItfInstance *)self;
    if (pState) {
        typedef struct {
            uint32_t count;
            uint32_t playIndex;
        } BQState;
        BQState *st = (BQState *)pState;
        st->count = bq->count;
        st->playIndex = bq->play_index;
    }
    return SL_RESULT_SUCCESS;
}

static uint32_t BQ_RegisterCallback(void *self, void *callback, void *pContext) {
    BufferQueueItfInstance *bq = (BufferQueueItfInstance *)self;
    l_debug("[OpenSL] BQ_RegisterCallback registered");
    rmutexLock(&s_audio_lock);
    bq->callback = callback;
    bq->context = pContext;
    rmutexUnlock(&s_audio_lock);
    return SL_RESULT_SUCCESS;
}

static const struct SLBufferQueueItf_ s_BQItf_Vtbl = {
    BQ_Enqueue,
    BQ_Clear,
    BQ_GetState,
    BQ_RegisterCallback
};

// 3. Volume Interface
static uint32_t Volume_SetVolumeLevel(void *self, int16_t level) {
    VolumeItfInstance *vol = (VolumeItfInstance *)self;
    vol->volume = level;
    return SL_RESULT_SUCCESS;
}

static uint32_t Volume_GetVolumeLevel(void *self, int16_t *pLevel) {
    VolumeItfInstance *vol = (VolumeItfInstance *)self;
    if (pLevel) *pLevel = vol->volume;
    return SL_RESULT_SUCCESS;
}

static uint32_t Volume_GetMaxVolumeLevel(void *self, int16_t *pMaxLevel) {
    (void)self;
    if (pMaxLevel) *pMaxLevel = 0;
    return SL_RESULT_SUCCESS;
}

static uint32_t Volume_SetMute(void *self, uint32_t mute) {
    VolumeItfInstance *vol = (VolumeItfInstance *)self;
    vol->mute = mute;
    return SL_RESULT_SUCCESS;
}

static uint32_t Volume_GetMute(void *self, uint32_t *pMute) {
    VolumeItfInstance *vol = (VolumeItfInstance *)self;
    if (pMute) *pMute = vol->mute;
    return SL_RESULT_SUCCESS;
}

static const struct SLVolumeItf_ s_VolumeItf_Vtbl = {
    Volume_SetVolumeLevel,
    Volume_GetVolumeLevel,
    Volume_GetMaxVolumeLevel,
    Volume_SetMute,
    Volume_GetMute,
    (void *)OpenSL_StubSuccess, // EnableStereoPosition
    (void *)OpenSL_StubSuccess, // IsEnabledStereoPosition
    (void *)OpenSL_StubSuccess, // SetStereoPosition
    (void *)OpenSL_StubSuccess  // GetStereoPosition
};

// 4. Seek Interface
static uint32_t Seek_SetLoop(void *self, uint32_t loopEnable, uint32_t startPos, uint32_t endPos) {
    (void)startPos; (void)endPos;
    SeekItfInstance *seek = (SeekItfInstance *)self;
    seek->loop = loopEnable;
    return SL_RESULT_SUCCESS;
}

static uint32_t Seek_GetLoop(void *self, uint32_t *pLoopEnable, uint32_t *pStartPos, uint32_t *pEndPos) {
    SeekItfInstance *seek = (SeekItfInstance *)self;
    if (pLoopEnable) *pLoopEnable = seek->loop;
    if (pStartPos) *pStartPos = 0;
    if (pEndPos) *pEndPos = 0xFFFFFFFF;
    return SL_RESULT_SUCCESS;
}

static const struct SLSeekItf_ s_SeekItf_Vtbl = {
    (void *)OpenSL_StubSuccess, // SetPosition
    Seek_SetLoop,
    Seek_GetLoop
};

// 5. Android Configuration Interface
static uint32_t AndroidConfig_SetConfiguration(void *self, const char *configKey, const void *pConfigValue, uint32_t valueSize) {
    (void)self; (void)pConfigValue; (void)valueSize;
    l_debug("[OpenSL] AndroidConfiguration_SetConfiguration '%s'", configKey ? configKey : "(null)");
    return SL_RESULT_SUCCESS;
}

static uint32_t AndroidConfig_GetConfiguration(void *self, const char *configKey, uint32_t *pValueSize, void *pConfigValue) {
    (void)self; (void)configKey; (void)pConfigValue;
    if (pValueSize) *pValueSize = 0;
    return SL_RESULT_SUCCESS;
}

static const struct SLAndroidConfigurationItf_ s_AndroidConfigItf_Vtbl = {
    AndroidConfig_SetConfiguration,
    AndroidConfig_GetConfiguration
};

// Player GetInterface dispatcher
static uint32_t PlayerObj_GetInterface(void *self, const SLInterfaceID iid, void *pInterface) {
    AudioPlayerObjectInstance *player = (AudioPlayerObjectInstance *)self;
    if (!pInterface || !iid) return SL_RESULT_PARAMETER_INVALID;

    if (memcmp(iid, SL_IID_PLAY, sizeof(SLInterfaceID_)) == 0) {
        *(void **)pInterface = &player->playItf;
        return SL_RESULT_SUCCESS;
    }
    if (memcmp(iid, SL_IID_BUFFERQUEUE, sizeof(SLInterfaceID_)) == 0 ||
        memcmp(iid, SL_IID_ANDROIDSIMPLEBUFFERQUEUE, sizeof(SLInterfaceID_)) == 0) {
        *(void **)pInterface = &player->bqItf;
        return SL_RESULT_SUCCESS;
    }
    if (memcmp(iid, SL_IID_VOLUME, sizeof(SLInterfaceID_)) == 0) {
        *(void **)pInterface = &player->volItf;
        return SL_RESULT_SUCCESS;
    }
    if (memcmp(iid, SL_IID_SEEK, sizeof(SLInterfaceID_)) == 0) {
        *(void **)pInterface = &player->seekItf;
        return SL_RESULT_SUCCESS;
    }
    if (memcmp(iid, SL_IID_ANDROIDCONFIGURATION, sizeof(SLInterfaceID_)) == 0) {
        *(void **)pInterface = &player->configItf;
        return SL_RESULT_SUCCESS;
    }

    l_warn("[OpenSL] Player GetInterface -> Unknown IID: 0x%08x", iid->time_low);
    *(void **)pInterface = NULL;
    return SL_RESULT_PARAMETER_INVALID;
}

// ============================================================================
// Engine Interface Methods
// ============================================================================
static uint32_t Engine_CreateAudioPlayer(void *self, void **pPlayer, void *pAudioSrc, void *pAudioSnk,
                                         uint32_t numInterfaces, const SLInterfaceID *pInterfaceIds, const uint32_t *pInterfaceRequired) {
    (void)self; (void)pAudioSnk; (void)numInterfaces; (void)pInterfaceIds; (void)pInterfaceRequired;

    AudioPlayerObjectInstance *player = (AudioPlayerObjectInstance *)calloc(1, sizeof(AudioPlayerObjectInstance));
    if (!player) return SL_RESULT_MEMORY_FAILURE;

    player->channels = 2;
    player->rate = 44100;
    const SLDataSource *src = (const SLDataSource *)pAudioSrc;
    if (src && src->pFormat) {
        const SLDataFormat_PCM *fmt = (const SLDataFormat_PCM *)src->pFormat;
        if (fmt->formatType == 2) {
            if (fmt->numChannels == 1 || fmt->numChannels == 2) player->channels = fmt->numChannels;
            if (fmt->samplesPerSec >= 8000000) player->rate = fmt->samplesPerSec / 1000;
            if (fmt->bitsPerSample != 16)
                l_warn("[OpenSL] unsupported sample size %u bits; treating as 16", fmt->bitsPerSample);
        }
    }
    l_info("[OpenSL] CreateAudioPlayer: %u Hz, %u channel(s)", player->rate, player->channels);

    player->vtable = &s_PlayerObj_Vtbl;

    player->playItf.vtable = &s_PlayItf_Vtbl;
    player->playItf.player = player;
    player->playItf.state = SL_PLAYSTATE_STOPPED;

    player->bqItf.vtable = &s_BQItf_Vtbl;
    player->bqItf.player = player;

    player->volItf.vtable = &s_VolumeItf_Vtbl;
    player->volItf.player = player;

    player->seekItf.vtable = &s_SeekItf_Vtbl;
    player->seekItf.player = player;

    player->configItf.vtable = &s_AndroidConfigItf_Vtbl;
    player->configItf.player = player;

    bool registered = false;
    rmutexLock(&s_audio_lock);
    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (!s_players[i]) {
            s_players[i] = player;
            registered = true;
            break;
        }
    }
    rmutexUnlock(&s_audio_lock);
    if (!registered) l_warn("[OpenSL] too many audio players; this one will be silent");

    *pPlayer = (void *)player;
    return SL_RESULT_SUCCESS;
}

static uint32_t Engine_CreateAudioRecorder(void *self, void **pRecorder, void *pAudioSrc, void *pAudioSnk,
                                           uint32_t numInterfaces, const SLInterfaceID *pInterfaceIds, const uint32_t *pInterfaceRequired) {
    (void)self; (void)pAudioSrc; (void)pAudioSnk; (void)numInterfaces; (void)pInterfaceIds; (void)pInterfaceRequired;
    if (pRecorder) *pRecorder = NULL;
    return SL_RESULT_RESOURCE_ERROR;
}

static uint32_t Engine_CreateOutputMix(void *self, void **pMix, uint32_t numInterfaces,
                                       const SLInterfaceID *pInterfaceIds, const uint32_t *pInterfaceRequired) {
    (void)self; (void)numInterfaces; (void)pInterfaceIds; (void)pInterfaceRequired;
    l_info("[OpenSL] Engine_CreateOutputMix called");
    if (pMix) *pMix = (void *)&s_globalOutputMix;
    return SL_RESULT_SUCCESS;
}

static const struct SLEngineItf_ s_EngineItf_Vtbl = {
    (void *)OpenSL_StubSuccess, // CreateLEDDevice
    (void *)OpenSL_StubSuccess, // CreateVibraDevice
    Engine_CreateAudioPlayer,   // CreateAudioPlayer (Index 2, offset 0x10)
    Engine_CreateAudioRecorder, // CreateAudioRecorder (Index 3, offset 0x18)
    (void *)OpenSL_StubSuccess, // CreateMidiPlayer
    (void *)OpenSL_StubSuccess, // CreateListener
    (void *)OpenSL_StubSuccess, // Create3DGroup
    Engine_CreateOutputMix,     // CreateOutputMix (Index 7, offset 0x38)
    (void *)OpenSL_StubSuccess, // CreateMetadataExtractor
    (void *)OpenSL_StubSuccess, // CreateExtensionObject
    (void *)OpenSL_StubSuccess, // QueryNumSupportedEngineInterfaces
    (void *)OpenSL_StubSuccess  // QuerySupportedEngineInterface
};

static EngineObjectInstance s_globalEngineObj = {
    &s_EngineObj_Vtbl,
    { &s_EngineItf_Vtbl }
};

static uint32_t EngineObj_GetInterface(void *self, const SLInterfaceID iid, void *pInterface) {
    (void)self;
    if (!pInterface || !iid) return SL_RESULT_PARAMETER_INVALID;

    if (memcmp(iid, SL_IID_ENGINE, sizeof(SLInterfaceID_)) == 0) {
        l_debug("[OpenSL] EngineObj_GetInterface -> SL_IID_ENGINE");
        *(void **)pInterface = &s_globalEngineObj.engineItf;
        return SL_RESULT_SUCCESS;
    }

    l_warn("[OpenSL] EngineObj_GetInterface -> Unknown IID: 0x%08x", iid->time_low);
    *(void **)pInterface = NULL;
    return SL_RESULT_PARAMETER_INVALID;
}

// ============================================================================
// Public Entry Points
// ============================================================================
void opensles_init(void) {
    if (s_audio_inited) return;
    s_audio_inited = true;

    Result rc = audoutInitialize();
    if (R_FAILED(rc)) {
        l_warn("OpenSL ES bridge: audoutInitialize failed: 0x%x", rc);
        return;
    }
    for (int i = 0; i < OUTPUT_BUFFERS; i++) {
        // audout wants page-aligned buffers of whole pages; a period is one.
        void *data = memalign(0x1000, OUTPUT_BYTES);
        if (!data) {
            l_warn("OpenSL ES bridge: no memory for the audio buffers");
            return;
        }
        memset(data, 0, OUTPUT_BYTES);
        s_output[i].next = NULL;
        s_output[i].buffer = data;
        s_output[i].buffer_size = OUTPUT_BYTES;
        s_output[i].data_size = OUTPUT_BYTES;
        s_output[i].data_offset = 0;
    }
    rc = audoutStartAudioOut();
    if (R_FAILED(rc)) {
        l_warn("OpenSL ES bridge: audoutStartAudioOut failed: 0x%x", rc);
        return;
    }

    // Same priority as the thread that starts the game, as before, but free
    // to run on any core, so a busy core does not hold the sound back.
    s32 prio = 0x2C;
    svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
    rc = threadCreate(&s_output_thread, output_main, NULL, NULL, OUTPUT_STACK, prio, -2);
    if (R_FAILED(rc)) {
        l_warn("OpenSL ES bridge: could not create the audio thread: 0x%x", rc);
        return;
    }
    u64 mask = 0;
    if (R_SUCCEEDED(svcGetInfo(&mask, InfoType_CoreMask, CUR_PROCESS_HANDLE, 0)) && mask)
        svcSetThreadCoreMask(s_output_thread.handle, -1, (u32)mask);
    rc = threadStart(&s_output_thread);
    if (R_FAILED(rc)) {
        l_warn("OpenSL ES bridge: could not start the audio thread: 0x%x", rc);
        threadClose(&s_output_thread);
        return;
    }
    l_info("OpenSL ES bridge: audio output opened (freq=%u, ch=%u)",
           (unsigned)audoutGetSampleRate(), (unsigned)audoutGetChannelCount());
}

uint32_t wrap_slCreateEngine(
    SLObjectItf *pEngine,
    uint32_t numOptions,
    const void *pEngineOptions,
    uint32_t numInterfaces,
    const SLInterfaceID *pInterfaceIds,
    const uint32_t *pInterfaceRequired
) {
    (void)numOptions; (void)pEngineOptions; (void)numInterfaces; (void)pInterfaceIds; (void)pInterfaceRequired;
    l_info("[OpenSL] slCreateEngine invoked");
    opensles_init();

    if (pEngine) {
        *pEngine = (void *)&s_globalEngineObj;
    }
    return SL_RESULT_SUCCESS;
}
