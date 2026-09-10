#ifndef DEADTRIGGER_OPENSLES_DT_H
#define DEADTRIGGER_OPENSLES_DT_H

#include <stdint.h>

typedef uint32_t SLresult;
typedef uint32_t SLuint32;
typedef int32_t SLmillibel;
typedef uint32_t SLmillisecond;
typedef uint32_t SLBoolean;
typedef const void *SLInterfaceID;

#define SL_RESULT_SUCCESS ((SLresult)0)
#define SL_RESULT_FEATURE_UNSUPPORTED ((SLresult)12)
#define SL_RESULT_RESOURCE_ERROR ((SLresult)13)

#define SL_PLAYSTATE_STOPPED ((SLuint32)1)
#define SL_PLAYSTATE_PAUSED ((SLuint32)2)
#define SL_PLAYSTATE_PLAYING ((SLuint32)3)
#define SL_TIME_UNKNOWN ((SLmillisecond)0xffffffffu)

#define SL_PLAYEVENT_HEADATEND ((SLuint32)1)

#define SL_DATAFORMAT_PCM ((SLuint32)2)
#define SL_DATALOCATOR_ANDROIDSIMPLEBUFFERQUEUE ((SLuint32)0x800007bdu)

extern const SLInterfaceID sl_IID_ENGINE;
extern const SLInterfaceID sl_IID_PLAY;
extern const SLInterfaceID sl_IID_VOLUME;
extern const SLInterfaceID sl_IID_BUFFERQUEUE;
extern const SLInterfaceID sl_IID_EFFECTSEND;
extern const SLInterfaceID sl_IID_ENGINECAPABILITIES;
extern const SLInterfaceID sl_IID_ANDROIDCONFIGURATION;
extern const SLInterfaceID sl_IID_ENVIRONMENTALREVERB;

SLresult slCreateEngine_shim(void **engine, SLuint32 option_count,
                             const void *options, SLuint32 interface_count,
                             const SLInterfaceID *interface_ids,
                             const SLBoolean *interface_required);

void opensles_shim_pump_callbacks(void);
int opensles_shim_engine_active(void);

#endif
