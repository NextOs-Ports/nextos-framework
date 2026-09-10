// Dolphin SDK entry points Aurora does not implement.
//
// Everything here is a peripheral: cache maintenance, the debugger transport,
// the reset switch, progressive-scan negotiation.  Each one answers "fine,
// carry on" so the game's own flow keeps moving; none of them fakes a state
// transition the engine is supposed to make for itself.

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <dolphin/ar.h>
#include <dolphin/card.h>
#include <dolphin/hio.h>
#include <dolphin/os.h>
#include <dolphin/pad.h>
#include <dolphin/vi.h>

#include "port/os_port.h"

// ---------------------------------------------------------------------------
// Reporting.
//
// Aurora declares OSReport/OSPanic weak and defines them in a static archive.
// A weak *reference* does not pull an archive member in, so without a strong
// definition here the whole game links against address zero and every
// ERROR()/assert path is a null call waiting to happen.  Define them, and send
// them to the port's log while we're at it.
// ---------------------------------------------------------------------------

void OSVReport(const char* msg, va_list args)
{
	vprintf(msg, args);
	fflush(stdout);
}

void OSReport(const char* msg, ...)
{
	va_list args;
	va_start(args, msg);
	vprintf(msg, args);
	va_end(args);
	fflush(stdout);
}

void OSPanic(const char* file, int line, const char* msg, ...)
{
	va_list args;
	fprintf(stderr, "[pikmin] PANIC %s:%d: ", file, line);
	va_start(args, msg);
	vfprintf(stderr, msg, args);
	va_end(args);
	fputc('\n', stderr);
	fflush(stderr);
	abort();
}

// ---------------------------------------------------------------------------
// Cache maintenance.  The AArch64 host is coherent with itself and Aurora reads
// vertex/texture memory straight out of the same address space.
// ---------------------------------------------------------------------------

void DCFlushRange(void* addr, u32 nBytes) { (void)addr; (void)nBytes; }
void DCFlushRangeNoSync(void* addr, u32 nBytes) { (void)addr; (void)nBytes; }
void DCInvalidateRange(void* addr, u32 nBytes) { (void)addr; (void)nBytes; }
void DCStoreRange(void* addr, u32 nBytes) { (void)addr; (void)nBytes; }
void DCStoreRangeNoSync(void* addr, u32 nBytes) { (void)addr; (void)nBytes; }
void DCZeroRange(void* addr, u32 nBytes) { memset(addr, 0, nBytes); }
void ICInvalidateRange(void* addr, u32 nBytes) { (void)addr; (void)nBytes; }

void LCEnable(void) {}
void LCDisable(void) {}
void PPCHalt(void) { abort(); }
void PPCSync(void) {}

u8 LC_CACHE_BASE[4096];

// ---------------------------------------------------------------------------
// Console state
// ---------------------------------------------------------------------------

u32 OSGetConsoleType(void) { return OS_CONSOLE_RETAIL1; }
u32 OSGetConsoleSimulatedMemSize(void) { return 24 * 1024 * 1024; }
u32 OSGetStackPointer(void) { return 0; }

// Pikmin's language handling: report English so no menu text can come up in
// Japanese, whatever the save data says.
u8 OSGetLanguage(void) { return 0; }

static u32 sSoundMode = OS_SOUND_MODE_STEREO;
u32 OSGetSoundMode(void) { return sSoundMode; }
void OSSetSoundMode(u32 mode) { sSoundMode = mode; }

static u32 sProgressiveMode;
u32 OSGetProgressiveMode(void) { return sProgressiveMode; }
void OSSetProgressiveMode(u32 mode) { sProgressiveMode = mode; }

BOOL OSGetResetSwitchState(void) { return FALSE; }
BOOL OSGetResetButtonState(void) { return FALSE; }

void OSResetSystem(int reset, u32 resetCode, BOOL forceMenu)
{
	(void)reset;
	(void)resetCode;
	(void)forceMenu;
	PikiPortRequestExit();
}

u32 __OSGetDIConfig(void) { return 0; }

void OSSetStringTable(void* stringTable) { (void)stringTable; }
u16 OSGetFontEncode(void) { return 0; } // OS_FONT_ENCODE_ANSI

void __OSCacheInit(void) {}
void __OSContextInit(void) {}
void __OSInterruptInit(void) {}
void __OSThreadInit(void) {}
void __OSInitSystemCall(void) {}
void __OSModuleInit(void) {}
void __OSInitAudioSystem(void) {}
void __OSStopAudioSystem(void) {}
void __OSInitMemoryProtection(void) {}

// Pikmin's own allocation wrappers. OSAlloc2 is supplied by the JAudio host
// boundary so audio data stays inside the heap configured by Jac_HeapSetup.
void OSFree2(void* ptr) { OSFreeToHeap(__OSCurrHeap, ptr); }

// ---------------------------------------------------------------------------
// Host I/O (the devkit's debugger link).  There is no devkit here.
// ---------------------------------------------------------------------------

// The devkit link is absent, so every entry point reports "no device" - which
// is what KIO::initialise checks before it gives up and moves on.
BOOL HIOEnumDevices(HIOEnumCallback callback)
{
	(void)callback;
	return FALSE;
}

BOOL HIOInit(s32 chan, HIOCallback callback)
{
	(void)chan;
	(void)callback;
	return FALSE;
}

BOOL HIOInitEx(s32 chan, u32 dev, HIOCallback callback)
{
	(void)chan;
	(void)dev;
	(void)callback;
	return FALSE;
}

BOOL HIOReadMailbox(u32* word)
{
	if (word != NULL) {
		*word = 0;
	}
	return FALSE;
}

BOOL HIOWriteMailbox(u32 word)
{
	(void)word;
	return FALSE;
}

BOOL HIORead(u32 addr, void* buffer, s32 size)
{
	(void)addr;
	(void)buffer;
	(void)size;
	return FALSE;
}

BOOL HIOWrite(u32 addr, void* buffer, s32 size)
{
	(void)addr;
	(void)buffer;
	(void)size;
	return FALSE;
}

BOOL HIOReadAsync(u32 addr, void* buffer, s32 size, HIOCallback callback)
{
	(void)addr;
	(void)buffer;
	(void)size;
	(void)callback;
	return FALSE;
}

BOOL HIOWriteAsync(u32 addr, void* buffer, s32 size, HIOCallback callback)
{
	(void)addr;
	(void)buffer;
	(void)size;
	(void)callback;
	return FALSE;
}

BOOL HIOReadStatus(u32* status)
{
	if (status != NULL) {
		*status = 0;
	}
	return FALSE;
}

// ---------------------------------------------------------------------------
// GX bits the SDK exported that Aurora has no equivalent for.
// ---------------------------------------------------------------------------

void __GXSetRange(f32 nearz, f32 farz)
{
	(void)nearz;
	(void)farz;
}

void __GXSetSUTexSize(void) {}

// Which OS thread owns the GX FIFO.  With the port's scheduler lock only one
// thread runs at a time, so ownership is bookkeeping - but the game reads the
// previous owner back, so hand it a real answer.
static OSThread* sGXThread;
OSThread* GXSetCurrentGXThread(void)
{
	OSThread* prev = sGXThread;
	sGXThread      = OSGetCurrentThread();
	return prev;
}

// Scissor box offset shifts the scissor rectangle relative to the EFB.  Pikmin
// only reaches this through DGXGraphics::setViewportOffset, which nothing in the
// shipped game calls, and Aurora has no equivalent - so record it and move on
// rather than pretend to apply it.
static s32 sScissorOffsetX;
static s32 sScissorOffsetY;
void GXSetScissorBoxOffset(s32 x_off, s32 y_off)
{
	sScissorOffsetX = x_off;
	sScissorOffsetY = y_off;
}
