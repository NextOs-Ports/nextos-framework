#ifndef _PIKI_PORT_OS_PORT_H
#define _PIKI_PORT_OS_PORT_H

// Entry points the port layer shares between its own translation units.

#ifdef __cplusplus
extern "C" {
#endif

// Scheduler lock (see src/port/os_thread.c).  Exactly one Dolphin OS thread
// runs at a time; these move the baton.
void OSPortSchedulerInit(void);
void OSPortSchedulerAcquire(void);
void OSPortSchedulerRelease(void);
void OSPortSchedulerYield(void);

// Wrap a call that blocks inside Aurora (a disc read, a card transfer).  On the
// console those block the calling thread and the scheduler runs someone else -
// most importantly the loading-screen thread, which is the only thing drawing
// while the main thread pulls files off the disc.  Holding the scheduler lock
// across them would leave the screen black for the whole load.
void OSPortBlockingEnter(void);
void OSPortBlockingLeave(void);

// Frame pump.  Called from DGXGraphics so the game keeps driving its own
// render cadence instead of the port imposing one.
void PikiPortBeginFrame(void);
void PikiPortEndFrame(void);
void PikiPortPumpEvents(void);
int PikiPortIsRunning(void);
void PikiPortRequestExit(void);

// Stops the VI retrace clock. Call once the game's loop has returned and before
// Aurora is torn down - see the comment in vi_port.c for why the thread must
// outlive the exit flag.
void PikiPortViStop(void);

// Widescreen factor: (real framebuffer aspect) / (the game's 4:3), read from
// the display Aurora actually opened - never hardcoded.  1.0 on a 4:3 panel;
// ~1.333 on 16:9.  The 3D projection is widened by this and the 2D ortho is
// squeezed by it; Aurora presents the EFB stretched to the full surface, which
// cancels both.  Computed once after aurora_initialize.
float PikiPortWidescreenFactor(void);

// Frame watchdog (src/port/watchdog.c).  A wedged game here can leave the Mali
// driver holding the framebuffer and take the whole box down with it.
void PikiPortWatchdogStart(void);
void PikiPortWatchdogFrame(void);

// Synthetic input (src/port/debug_input.c).  Off unless PIKMIN_AUTOSTART is set;
// only ever ORs bits onto what the real pad reported.
struct PADStatus;
void PikiPortDebugInput(struct PADStatus* pads);

// Disc image mounted by the port.
int PikiPortOpenDisc(const char* path);

#ifdef __cplusplus
}
#endif

#endif
