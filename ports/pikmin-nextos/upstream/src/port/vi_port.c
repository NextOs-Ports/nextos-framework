// Video Interface: the retrace clock.
//
// Aurora implements VIConfigure/VIFlush but not retraces, and Pikmin leans on
// them hard.  The post-retrace callback is what nudges the DVD thread, what
// nudges the load idler, and what releases DGXGraphics::waitPostRetrace - so a
// retrace has to arrive on its own, exactly like the console's VI interrupt,
// not only when the main thread happens to ask for one.  Driving it from inside
// VIWaitForRetrace would deadlock the moment the game throttles itself to 30 Hz
// (waitPostRetrace blocks *before* it ever calls VIWaitForRetrace).
//
// So: a dedicated thread ticks at the field rate and runs the callbacks while
// holding the scheduler lock, which is exactly the mutual exclusion the console
// got from running them in interrupt context.

#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include <dolphin/os.h>
#include <dolphin/vi.h>

#include "Dolphin/vitypes.h"

#include "port/os_port.h"

#define VI_FIELD_NS 16683333L // NTSC field period, 59.94 Hz

static VIRetraceCallback sPreCallback;
static VIRetraceCallback sPostCallback;
static volatile u32 sRetraceCount;
static void* sCurrentFrameBuffer;
static void* sNextFrameBuffer;
static BOOL sBlack;
static int sThreadStarted;
static volatile int sViStop;
static pthread_t sViThread;
static pthread_cond_t sRetraceCond = PTHREAD_COND_INITIALIZER;
static pthread_mutex_t sRetraceLock = PTHREAD_MUTEX_INITIALIZER;

static void* vi_thread(void* arg)
{
	struct timespec next;
	(void)arg;

	clock_gettime(CLOCK_MONOTONIC, &next);
	for (;;) {
		next.tv_nsec += VI_FIELD_NS;
		while (next.tv_nsec >= 1000000000L) {
			next.tv_nsec -= 1000000000L;
			next.tv_sec++;
		}
		clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, NULL);

		// Deliberately NOT keyed on PikiPortIsRunning(). When the exit flag goes
		// down, the main thread is usually already inside app->idle(), parked in
		// DGXGraphics::waitPostRetrace on an OS message queue that only the
		// post-retrace callback below ever posts to. Stopping here would leave
		// it parked forever: the process stays alive holding the framebuffer,
		// which on the Mali means a wedged box that needs a power cycle. So keep
		// the field ticking until portmain has actually left the game's loop and
		// calls PikiPortViStop().
		if (sViStop) {
			break;
		}

		// Run the callbacks with the scheduler lock held: on the console these
		// ran in interrupt context, so nothing else was executing.
		OSPortSchedulerAcquire();
		sRetraceCount++;
		sCurrentFrameBuffer = sNextFrameBuffer;
		if (sPreCallback != NULL) {
			sPreCallback(sRetraceCount);
		}
		if (sPostCallback != NULL) {
			sPostCallback(sRetraceCount);
		}
		OSPortSchedulerRelease();

		pthread_mutex_lock(&sRetraceLock);
		pthread_cond_broadcast(&sRetraceCond);
		pthread_mutex_unlock(&sRetraceLock);
	}
	return NULL;
}

static void ensure_vi_thread(void)
{
	if (sThreadStarted) {
		return;
	}
	sThreadStarted = 1;
	if (pthread_create(&sViThread, NULL, vi_thread, NULL) != 0) {
		fprintf(stderr, "[pikmin] failed to start VI retrace thread\n");
		sThreadStarted = 0;
	}
}

// Called once the game's own loop has returned, so nothing can still be waiting
// on a field. Must run before Aurora is torn down: the callbacks touch GX state.
void PikiPortViStop(void)
{
	if (!sThreadStarted) {
		return;
	}
	sViStop = 1;
	pthread_join(sViThread, NULL);
	sThreadStarted = 0;

	pthread_mutex_lock(&sRetraceLock);
	pthread_cond_broadcast(&sRetraceCond);
	pthread_mutex_unlock(&sRetraceLock);
}

void VIWaitForRetrace(void)
{
	u32 start;

	ensure_vi_thread();
	start = sRetraceCount;

	// The caller holds the scheduler lock; drop it so the VI thread (and any
	// other OS thread) can run while we wait for the field to flip.
	OSPortSchedulerRelease();
	pthread_mutex_lock(&sRetraceLock);
	while (sRetraceCount == start && PikiPortIsRunning()) {
		struct timespec deadline;
		clock_gettime(CLOCK_REALTIME, &deadline);
		deadline.tv_nsec += 100000000L; // 100 ms guard so a stall can't wedge us
		if (deadline.tv_nsec >= 1000000000L) {
			deadline.tv_nsec -= 1000000000L;
			deadline.tv_sec++;
		}
		pthread_cond_timedwait(&sRetraceCond, &sRetraceLock, &deadline);
	}
	pthread_mutex_unlock(&sRetraceLock);
	OSPortSchedulerAcquire();
}

u32 VIGetRetraceCount(void) { return sRetraceCount; }

// Fields alternate; the game only uses this to skip one field at boot.
u32 VIGetNextField(void) { return (sRetraceCount & 1) ? VI_FIELD_BELOW : VI_FIELD_ABOVE; }

u32 VIGetCurrentLine(void) { return 0; }
u32 VIGetDTVStatus(void) { return 0; }

void __VIGetCurrentPosition(s16* x, s16* y)
{
	if (x != NULL) {
		*x = 0;
	}
	if (y != NULL) {
		*y = 0;
	}
}

VIRetraceCallback VISetPreRetraceCallback(VIRetraceCallback callback)
{
	VIRetraceCallback prev = sPreCallback;
	sPreCallback           = callback;
	ensure_vi_thread();
	return prev;
}

VIRetraceCallback VISetPostRetraceCallback(VIRetraceCallback callback)
{
	VIRetraceCallback prev = sPostCallback;
	sPostCallback          = callback;
	ensure_vi_thread();
	return prev;
}

void VISetNextFrameBuffer(void* fb) { sNextFrameBuffer = fb; }
void VISetNextRightFrameBuffer(void* fb) { (void)fb; }
void* VIGetCurrentFrameBuffer(void) { return sCurrentFrameBuffer; }
void* VIGetNextFrameBuffer(void) { return sNextFrameBuffer; }

// Aurora presents through its own swapchain, so blanking is only bookkeeping.
void VISetBlack(BOOL black) { sBlack = black; }

void VISet3D(void) {}
void __VIInit(VITVMode mode) { (void)mode; }
