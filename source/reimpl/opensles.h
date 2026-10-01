#ifndef __REIMPL_OPENSLES_H__
#define __REIMPL_OPENSLES_H__

#include <stdint.h>
#include <stddef.h>

#define SL_RESULT_SUCCESS             ((uint32_t) 0x00000000)
#define SL_RESULT_PRECONDITIONS_VIOLATED ((uint32_t) 0x00000001)
#define SL_RESULT_PARAMETER_INVALID   ((uint32_t) 0x00000002)
#define SL_RESULT_MEMORY_FAILURE      ((uint32_t) 0x00000003)
#define SL_RESULT_RESOURCE_ERROR      ((uint32_t) 0x00000004)
#define SL_RESULT_RESOURCE_LOST       ((uint32_t) 0x00000005)
#define SL_RESULT_IO_ERROR            ((uint32_t) 0x00000006)
#define SL_RESULT_BUFFER_INSUFFICIENT ((uint32_t) 0x00000007)
#define SL_RESULT_UNKNOWN_ERROR       ((uint32_t) 0x00000008)

#define SL_BOOLEAN_FALSE              ((uint32_t) 0x00000000)
#define SL_BOOLEAN_TRUE               ((uint32_t) 0x00000001)

#define SL_PLAYSTATE_STOPPED          ((uint32_t) 0x00000001)
#define SL_PLAYSTATE_PAUSED           ((uint32_t) 0x00000002)
#define SL_PLAYSTATE_PLAYING          ((uint32_t) 0x00000003)

typedef struct SLInterfaceID_ {
    uint32_t time_low;
    uint16_t time_mid;
    uint16_t time_hi_and_version;
    uint16_t clock_seq;
    uint8_t  node[6];
} SLInterfaceID_;

typedef const struct SLInterfaceID_ *SLInterfaceID;

extern const struct SLInterfaceID_ SL_IID_ENGINE_val;
extern const struct SLInterfaceID_ SL_IID_PLAY_val;
extern const struct SLInterfaceID_ SL_IID_BUFFERQUEUE_val;
extern const struct SLInterfaceID_ SL_IID_ANDROIDSIMPLEBUFFERQUEUE_val;
extern const struct SLInterfaceID_ SL_IID_ANDROIDCONFIGURATION_val;
extern const struct SLInterfaceID_ SL_IID_VOLUME_val;
extern const struct SLInterfaceID_ SL_IID_SEEK_val;
extern const struct SLInterfaceID_ SL_IID_RECORD_val;
extern const struct SLInterfaceID_ SL_IID_PREFETCHSTATUS_val;
extern const struct SLInterfaceID_ SL_IID_METADATAEXTRACTION_val;
extern const struct SLInterfaceID_ SL_IID_PITCH_val;

extern SLInterfaceID SL_IID_ENGINE_ptr;
extern SLInterfaceID SL_IID_PLAY_ptr;
extern SLInterfaceID SL_IID_BUFFERQUEUE_ptr;
extern SLInterfaceID SL_IID_ANDROIDSIMPLEBUFFERQUEUE_ptr;
extern SLInterfaceID SL_IID_ANDROIDCONFIGURATION_ptr;
extern SLInterfaceID SL_IID_VOLUME_ptr;
extern SLInterfaceID SL_IID_SEEK_ptr;
extern SLInterfaceID SL_IID_RECORD_ptr;
extern SLInterfaceID SL_IID_PREFETCHSTATUS_ptr;
extern SLInterfaceID SL_IID_METADATAEXTRACTION_ptr;
extern SLInterfaceID SL_IID_PITCH_ptr;

#define SL_IID_ENGINE                   (&SL_IID_ENGINE_val)
#define SL_IID_PLAY                     (&SL_IID_PLAY_val)
#define SL_IID_BUFFERQUEUE              (&SL_IID_BUFFERQUEUE_val)
#define SL_IID_ANDROIDSIMPLEBUFFERQUEUE (&SL_IID_ANDROIDSIMPLEBUFFERQUEUE_val)
#define SL_IID_ANDROIDCONFIGURATION     (&SL_IID_ANDROIDCONFIGURATION_val)
#define SL_IID_VOLUME                   (&SL_IID_VOLUME_val)
#define SL_IID_SEEK                     (&SL_IID_SEEK_val)
#define SL_IID_RECORD                   (&SL_IID_RECORD_val)
#define SL_IID_PREFETCHSTATUS           (&SL_IID_PREFETCHSTATUS_val)
#define SL_IID_METADATAEXTRACTION       (&SL_IID_METADATAEXTRACTION_val)
#define SL_IID_PITCH                    (&SL_IID_PITCH_val)

typedef void *SLObjectItf;

uint32_t wrap_slCreateEngine(
    SLObjectItf *pEngine,
    uint32_t numOptions,
    const void *pEngineOptions,
    uint32_t numInterfaces,
    const SLInterfaceID *pInterfaceIds,
    const uint32_t *pInterfaceRequired
);

void opensles_init(void);

#endif // __REIMPL_OPENSLES_H__
