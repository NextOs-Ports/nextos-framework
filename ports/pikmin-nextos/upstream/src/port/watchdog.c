// Frame watchdog.
//
// A GameCube game that wedges on this hardware does not just stop drawing - it
// can leave the Mali driver holding the framebuffer, and the box stops
// responding entirely.  Recovering that costs a power cycle.
//
// So: if no frame has been presented for a while, say so and leave.  Exiting is
// what lets the kernel tear the driver state down; hanging on is what does not.
// This is the same shape of protection the Diddy Kong Racing port carries.

#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

#include "port/os_port.h"

#define WATCHDOG_POLL_SECONDS 2

static volatile unsigned long sFrameTick;
static int sStarted;
static pthread_t sThread;
static long sTimeoutSeconds = 45;

void PikiPortWatchdogFrame(void) { sFrameTick++; }

static void* watchdog_main(void* arg)
{
	unsigned long lastSeen = sFrameTick;
	long stalledFor        = 0;
	(void)arg;

	for (;;) {
		struct timespec delay = { WATCHDOG_POLL_SECONDS, 0 };
		nanosleep(&delay, NULL);

		if (!PikiPortIsRunning()) {
			return NULL;
		}

		if (sFrameTick != lastSeen) {
			lastSeen   = sFrameTick;
			stalledFor = 0;
			continue;
		}

		stalledFor += WATCHDOG_POLL_SECONDS;
		if (stalledFor >= sTimeoutSeconds) {
			fprintf(stderr,
			        "[pikmin] no frame presented for %ld s - exiting so the display driver "
			        "is released cleanly (set PIKMIN_WATCHDOG=0 to disable)\n",
			        stalledFor);
			fflush(stderr);
			fflush(stdout);
			_exit(2);
		}
	}
}

void PikiPortWatchdogStart(void)
{
	const char* env = getenv("PIKMIN_WATCHDOG");

	if (sStarted) {
		return;
	}
	if (env != NULL) {
		const long value = strtol(env, NULL, 10);
		if (value <= 0) {
			printf("[pikmin] frame watchdog disabled\n");
			return;
		}
		sTimeoutSeconds = value;
	}

	sStarted = 1;
	if (pthread_create(&sThread, NULL, watchdog_main, NULL) != 0) {
		fprintf(stderr, "[pikmin] could not start the frame watchdog\n");
		sStarted = 0;
		return;
	}
	pthread_detach(sThread);
	printf("[pikmin] frame watchdog armed (%ld s)\n", sTimeoutSeconds);
}
