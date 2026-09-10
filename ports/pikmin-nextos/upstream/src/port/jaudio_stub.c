// JAudio, stubbed.
//
// Pikmin's sound library is JSystem's jaudio, and jaudio has no software seam:
// src/jaudio/dspboot.c ships raw GameCube DSP microcode (u16 jdsp[], 427 lines
// of opcodes) that it hands to the DSP over the mailbox, and the synthesis *is*
// that microcode.  There is no PC target inside it to fill in the way MusyX's
// SAL layer had one.  Making sound come out therefore means interpreting the
// DSP, which is a separate piece of work.
//
// Until then the game boots silent.  Every entry point below returns the value
// that keeps the engine's own sequencing moving - "the event exists", "the
// stream is idle", "the demo finished" - so that no state machine waits on
// audio that will never arrive.

#include <string.h>
#include <dolphin/os.h>

#include "types.h"

#include "jaudio/app_inter.h"
#include "jaudio/interface.h"
#include "jaudio/piki_bgm.h"
#include "jaudio/piki_player.h"
#include "jaudio/piki_scene.h"
#include "jaudio/pikidemo.h"
#include "jaudio/pikiinter.h"
#include "jaudio/verysimple.h"
#include "port/jaudio_state.h"

static const u16 sGameStreamVolumeTable[] = { 0, 600, 1000, 2000, 3000, 4000, 5000, 6000, 8000, 10000, 12000 };
static u8 sBgmVolume                     = 8;
static u8 sSeVolume                      = 8;
static BOOL sStereoOutput                = TRUE;

// ---------------------------------------------------------------------------
// Boot / per-frame
// ---------------------------------------------------------------------------

void Jac_Start(void* heap, u32 heapSize, u32 aramSize, immut char* rootPath)
{
	(void)heap;
	(void)heapSize;
	(void)aramSize;
	(void)rootPath;
}

// Called once per frame from System::run, before anything else.  On the console
// this waited for the DSP to acknowledge the previous frame's command list.
void Jac_Gsync(void) {}

void Jac_Freeze(void) {}
void Jac_Freeze_Precall(void) {}

void Jac_OutputMode(int mode) { sStereoOutput = mode != 0; }
void Jac_SetBGMVolume(u8 vol) { sBgmVolume = vol < 11 ? vol : 10; }
void Jac_SetSEVolume(u8 vol) { sSeVolume = vol < 11 ? vol : 10; }

u16 PikiJAudioBGMStreamLevel(void) { return sGameStreamVolumeTable[sBgmVolume]; }
u16 PikiJAudioSEStreamLevel(void) { return sGameStreamVolumeTable[sSeVolume]; }
int PikiJAudioStereoOutput(void) { return sStereoOutput; }

// ---------------------------------------------------------------------------
// Scene setup
// ---------------------------------------------------------------------------

void Jac_SceneSetup(u32 sceneID, u32 stageID)
{
	(void)sceneID;
	(void)stageID;
}

void Jac_SceneExit(u32 sceneID, u32 stageID)
{
	(void)sceneID;
	(void)stageID;
}

void Jac_EnterBossMode(void) {}
void Jac_ExitBossMode(void) {}

// ---------------------------------------------------------------------------
// Sound effects
// ---------------------------------------------------------------------------

void Jac_PlaySystemSe(s32 id) { (void)id; }
void Jac_StopSystemSe(s32 id) { (void)id; }
void Jac_StopSe(s32 id) { (void)id; }

void Jac_PlayOrimaSe(u32 orimaSoundID) { (void)orimaSoundID; }
void Jac_StopOrimaSe(s32 orimaSoundID) { (void)orimaSoundID; }
void Jac_Orima_Walk(s32 groundSoundID, u32 p2)
{
	(void)groundSoundID;
	(void)p2;
}
void Jac_Orima_Formation(s32 a, s32 b)
{
	(void)a;
	(void)b;
}
void Jac_Piki_Number(u32 count) { (void)count; }

// ---------------------------------------------------------------------------
// Positional event system.
//
// The game allocates an "event" per emitter and then updates its position every
// frame.  Handing back a real index keeps its bookkeeping honest: it will pair
// each create with a destroy instead of leaking slots or retrying forever.
// ---------------------------------------------------------------------------

#define JAC_STUB_MAX_EVENTS 64

static u8 sEventUsed[JAC_STUB_MAX_EVENTS];
static int sEventCount;

int Jac_CreateEvent(u32 eventType, struct SVector_* eventPos)
{
	int i;
	(void)eventType;
	(void)eventPos;
	for (i = 0; i < JAC_STUB_MAX_EVENTS; i++) {
		if (!sEventUsed[i]) {
			sEventUsed[i] = 1;
			sEventCount++;
			return i;
		}
	}
	return -1;
}

BOOL Jac_DestroyEvent(s32 idx)
{
	if (idx >= 0 && idx < JAC_STUB_MAX_EVENTS && sEventUsed[idx]) {
		sEventUsed[idx] = 0;
		sEventCount--;
		return TRUE;
	}
	return FALSE;
}

void Jac_InitAllEvent(void)
{
	memset(sEventUsed, 0, sizeof(sEventUsed));
	sEventCount = 0;
}

int Jac_CheckFreeEvents(void) { return JAC_STUB_MAX_EVENTS - sEventCount; }

int Jac_GetActiveEvents(u32* outCount)
{
	if (outCount != NULL) {
		*outCount = (u32)sEventCount;
	}
	return sEventCount;
}

BOOL Jac_PlayEventAction(int eventIdx, int actionId)
{
	(void)eventIdx;
	(void)actionId;
	return TRUE;
}

BOOL Jac_StopEventAction(int eventIdx, int actionId)
{
	(void)eventIdx;
	(void)actionId;
	return TRUE;
}

BOOL Jac_UpdateEventPosition(int idx, struct SVector_* eventPos)
{
	(void)idx;
	(void)eventPos;
	return TRUE;
}

void Jac_UpdateCamera(struct SVector_* listenerPos, struct SVector_* listenerDir)
{
	(void)listenerPos;
	(void)listenerDir;
}

// ---------------------------------------------------------------------------
// Cutscene ("demo") audio.  Jac_DemoFrame drives the caption/beat timing of a
// cutscene; reporting "not running" lets the cutscene player fall back on its
// own frame counter instead of waiting for a stream that never starts.
// ---------------------------------------------------------------------------

void Jac_StartDemo(u32 demoID) { (void)demoID; }
void Jac_FinishDemo(void) {}
BOOL Jac_DemoFrame(int frame)
{
	(void)frame;
	return FALSE;
}
void Jac_StartTextDemo(int id) { (void)id; }
void Jac_FinishTextDemo(void) {}
void Jac_StartPartsFindDemo(u32 jingleType, BOOL hasAudio)
{
	(void)jingleType;
	(void)hasAudio;
}
void Jac_FinishPartsFindDemo(void) {}
void Jac_SetDemoOnyons(int count) { (void)count; }
void Jac_SetDemoPartsCount(int count) { (void)count; }
void Jac_SetDemoPartsID(int id) { (void)id; }

void Jac_AddDVDBuffer(u8* buf, u32 bufferSize)
{
	(void)buf;
	(void)bufferSize;
}

void Jac_BackDVDBuffer(void) {}
