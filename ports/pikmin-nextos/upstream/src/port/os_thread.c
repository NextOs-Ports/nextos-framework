// Dolphin OS threading on pthreads.
//
// Pikmin runs four background threads (DVD, card, load idler, movie playback)
// alongside the main game thread and coordinates them with OS mutexes,
// condition variables and message queues.  Aurora implements the rest of OS but
// not the scheduler, so the port supplies one.
//
// The console's scheduler only ever switches on an interrupt or an explicit
// blocking call, and the game is written for that: the DVD thread mutates
// structures the main thread reads without any lock of its own.  Reproducing
// that is what the *scheduler lock* below is for - exactly one OS thread runs
// at a time, and a thread gives the lock up only where the console would have
// yielded (sleep, join, suspend, mutex contention, message wait, yield,
// retrace).  Running these as freely preempting pthreads instead would be a
// data race the game never had.

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <dolphin/os.h>

#include "port/os_port.h"

#define MAX_OS_THREADS 32

typedef struct ThreadSlot {
	OSThread* thread;   // the game-owned OSThread this slot describes
	pthread_t native;   // valid once started
	pthread_cond_t cond;// signalled whenever this thread may re-check its state
	void* (*func)(void*);
	void* param;
	int started;
	int finished;
	int inUse;
} ThreadSlot;

static pthread_mutex_t sSchedLock = PTHREAD_MUTEX_INITIALIZER;
static ThreadSlot sSlots[MAX_OS_THREADS];
static OSThread sMainThread;
static pthread_key_t sCurrentKey;
static pthread_once_t sOnce = PTHREAD_ONCE_INIT;
static OSThreadQueue sActiveQueue;
static int sActiveCount;

// ---------------------------------------------------------------------------

static ThreadSlot* slot_alloc(OSThread* thread);

static void init_once(void)
{
	ThreadSlot* slot;

	pthread_key_create(&sCurrentKey, NULL);
	memset(&sMainThread, 0, sizeof(sMainThread));
	sMainThread.state    = OS_THREAD_STATE_RUNNING;
	sMainThread.priority = 16;
	pthread_setspecific(sCurrentKey, &sMainThread);
	sActiveCount = 1;

	// The main thread gets a slot like any other so that when it blocks - and it
	// does, on waitPostRetrace's message queue every frame - it can wait on a
	// condition variable instead of spinning.
	slot = slot_alloc(&sMainThread);
	if (slot != NULL) {
		slot->started = 1;
	}
}

static ThreadSlot* slot_for(OSThread* thread)
{
	int i;
	for (i = 0; i < MAX_OS_THREADS; i++) {
		if (sSlots[i].inUse && sSlots[i].thread == thread) {
			return &sSlots[i];
		}
	}
	return NULL;
}

static ThreadSlot* slot_alloc(OSThread* thread)
{
	int i;
	for (i = 0; i < MAX_OS_THREADS; i++) {
		if (!sSlots[i].inUse) {
			memset(&sSlots[i], 0, sizeof(sSlots[i]));
			pthread_cond_init(&sSlots[i].cond, NULL);
			sSlots[i].inUse  = 1;
			sSlots[i].thread = thread;
			return &sSlots[i];
		}
	}
	return NULL;
}

// Wake every slot so each blocked thread re-tests its own predicate.  Cheap:
// there are never more than a handful of OS threads.
static void wake_all(void)
{
	int i;
	for (i = 0; i < MAX_OS_THREADS; i++) {
		if (sSlots[i].inUse) {
			pthread_cond_broadcast(&sSlots[i].cond);
		}
	}
}

void OSPortSchedulerInit(void) { pthread_once(&sOnce, init_once); }

void OSPortSchedulerAcquire(void)
{
	pthread_once(&sOnce, init_once);
	pthread_mutex_lock(&sSchedLock);
}

void OSPortSchedulerRelease(void) { pthread_mutex_unlock(&sSchedLock); }

// Hand the scheduler lock to whoever else is ready, then take it back.  This is
// the yield point the console got for free from its timer interrupt.
void OSPortBlockingEnter(void)
{
	pthread_once(&sOnce, init_once);
	wake_all();
	pthread_mutex_unlock(&sSchedLock);
}

void OSPortBlockingLeave(void) { pthread_mutex_lock(&sSchedLock); }

void OSPortSchedulerYield(void)
{
	pthread_once(&sOnce, init_once);
	wake_all();
	pthread_mutex_unlock(&sSchedLock);
	sched_yield();
	pthread_mutex_lock(&sSchedLock);
}

// ---------------------------------------------------------------------------

static void* thread_trampoline(void* arg)
{
	ThreadSlot* slot = (ThreadSlot*)arg;
	OSThread* thread = slot->thread;
	void* result;

	pthread_setspecific(sCurrentKey, thread);
	pthread_mutex_lock(&sSchedLock);

	// A freshly resumed thread still has to wait its turn for the lock, and it
	// may have been suspended again before it ever got to run.
	while (thread->suspend > 0) {
		pthread_cond_wait(&slot->cond, &sSchedLock);
	}

	thread->state = OS_THREAD_STATE_RUNNING;
	result        = slot->func(slot->param);
	thread->val   = result;
	thread->state = OS_THREAD_STATE_MORIBUND;
	slot->finished = 1;
	sActiveCount--;
	wake_all();
	pthread_mutex_unlock(&sSchedLock);
	return result;
}

BOOL OSCreateThread(OSThread* thread, void* (*func)(void*), void* param, void* stack, u32 stackSize, OSPriority priority, u16 attr)
{
	ThreadSlot* slot;

	pthread_once(&sOnce, init_once);
	(void)stack;
	(void)stackSize;

	memset(thread, 0, sizeof(*thread));
	thread->state    = OS_THREAD_STATE_READY;
	thread->attr     = attr;
	thread->priority = priority;
	thread->base     = priority;
	thread->suspend  = 1; // created suspended, as on the console

	slot = slot_alloc(thread);
	if (slot == NULL) {
		OSReport("OSCreateThread: no free thread slots\n");
		return FALSE;
	}
	slot->func  = func;
	slot->param = param;
	sActiveCount++;
	return TRUE;
}

s32 OSResumeThread(OSThread* thread)
{
	ThreadSlot* slot;
	s32 prev;

	pthread_once(&sOnce, init_once);
	slot = slot_for(thread);
	prev = thread->suspend;
	if (thread->suspend > 0) {
		thread->suspend--;
	}

	if (slot != NULL && thread->suspend <= 0 && !slot->started) {
		pthread_attr_t pattr;
		slot->started = 1;
		pthread_attr_init(&pattr);
		pthread_attr_setdetachstate(&pattr, PTHREAD_CREATE_JOINABLE);
		if (pthread_create(&slot->native, &pattr, thread_trampoline, slot) != 0) {
			OSReport("OSResumeThread: pthread_create failed\n");
			slot->started = 0;
			slot->finished = 1;
			thread->state  = OS_THREAD_STATE_MORIBUND;
			thread->val    = NULL;
			sActiveCount--;
		}
		pthread_attr_destroy(&pattr);
	}
	wake_all();
	return prev;
}

s32 OSSuspendThread(OSThread* thread)
{
	ThreadSlot* slot;
	s32 prev;

	pthread_once(&sOnce, init_once);
	prev = thread->suspend;
	thread->suspend++;

	if (thread == OSGetCurrentThread()) {
		slot = slot_for(thread);
		if (slot != NULL) {
			while (thread->suspend > 0) {
				pthread_cond_wait(&slot->cond, &sSchedLock);
			}
		}
	}
	return prev;
}

void OSYieldThread(void) { OSPortSchedulerYield(); }

BOOL OSJoinThread(OSThread* thread, void** val)
{
	ThreadSlot* slot;

	pthread_once(&sOnce, init_once);
	slot = slot_for(thread);
	if (slot == NULL) {
		return FALSE;
	}

	while (!slot->finished) {
		// Give the joined thread the lock so it can make progress, then wait to
		// be told it finished.
		wake_all();
		pthread_cond_wait(&slot->cond, &sSchedLock);
	}
	if (val != NULL) {
		*val = thread->val;
	}
	if (slot->started) {
		pthread_mutex_unlock(&sSchedLock);
		pthread_join(slot->native, NULL);
		pthread_mutex_lock(&sSchedLock);
	}
	pthread_cond_destroy(&slot->cond);
	slot->inUse = 0;
	return TRUE;
}

void OSDetachThread(OSThread* thread)
{
	if (thread != NULL) {
		thread->attr |= OS_THREAD_ATTR_DETACH;
	}
}

void OSCancelThread(OSThread* thread)
{
	ThreadSlot* slot = slot_for(thread);
	thread->state    = OS_THREAD_STATE_MORIBUND;
	if (slot != NULL) {
		slot->finished = 1;
	}
	wake_all();
}

void OSExitThread(void* val)
{
	OSThread* thread = OSGetCurrentThread();
	if (thread != NULL) {
		thread->val   = val;
		thread->state = OS_THREAD_STATE_MORIBUND;
	}
	pthread_mutex_unlock(&sSchedLock);
	pthread_exit(val);
}

OSThread* OSGetCurrentThread(void)
{
	OSThread* thread;
	pthread_once(&sOnce, init_once);
	thread = (OSThread*)pthread_getspecific(sCurrentKey);
	return thread != NULL ? thread : &sMainThread;
}

BOOL OSIsThreadSuspended(OSThread* thread) { return thread->suspend > 0; }
BOOL OSIsThreadTerminated(OSThread* thread) { return thread->state == OS_THREAD_STATE_MORIBUND; }

BOOL OSSetThreadPriority(OSThread* thread, OSPriority priority)
{
	thread->priority = priority;
	return TRUE;
}

s32 OSGetThreadPriority(OSThread* thread) { return thread->priority; }

void OSSetThreadSpecific(s32 index, void* ptr)
{
	OSThread* thread = OSGetCurrentThread();
	if (index >= 0 && index < OS_THREAD_SPECIFIC_MAX) {
		thread->specific[index] = ptr;
	}
}

void* OSGetThreadSpecific(s32 index)
{
	OSThread* thread = OSGetCurrentThread();
	if (index >= 0 && index < OS_THREAD_SPECIFIC_MAX) {
		return thread->specific[index];
	}
	return NULL;
}

// The game calls this once per frame; on the console it walked the active list
// checking stack magic.  Here it doubles as the frame's yield point, which is
// what actually lets the DVD and card threads run.
s32 OSCheckActiveThreads(void)
{
	OSPortSchedulerYield();
	return sActiveCount;
}

s32 OSEnableScheduler(void) { return 0; }
s32 OSDisableScheduler(void) { return 0; }
void OSClearStack(u8 val) { (void)val; }

OSSwitchThreadCallback OSSetSwitchThreadCallback(OSSwitchThreadCallback callback)
{
	(void)callback;
	return NULL;
}

OSThread* OSSetIdleFunction(OSIdleFunction idleFunction, void* param, void* stack, u32 stackSize)
{
	(void)idleFunction;
	(void)param;
	(void)stack;
	(void)stackSize;
	return NULL;
}

OSThread* OSGetIdleFunction(void) { return NULL; }

// ---------------------------------------------------------------------------
// Thread queues
// ---------------------------------------------------------------------------

void OSInitThreadQueue(OSThreadQueue* queue)
{
	queue->head = NULL;
	queue->tail = NULL;
}

void OSSleepThread(OSThreadQueue* queue)
{
	OSThread* thread = OSGetCurrentThread();
	ThreadSlot* slot = slot_for(thread);

	pthread_once(&sOnce, init_once);

	// Enqueue, then block until OSWakeupThread pulls us back off.
	thread->queue = queue;
	thread->state = OS_THREAD_STATE_WAITING;
	if (queue->tail == NULL) {
		queue->head = thread;
	} else {
		queue->tail->link.next = thread;
	}
	thread->link.prev = queue->tail;
	thread->link.next = NULL;
	queue->tail       = thread;

	wake_all();
	while (thread->queue == queue) {
		if (slot != NULL) {
			pthread_cond_wait(&slot->cond, &sSchedLock);
		} else {
			// Main thread has no slot of its own; poll under the lock.
			pthread_mutex_unlock(&sSchedLock);
			sched_yield();
			pthread_mutex_lock(&sSchedLock);
		}
	}
	thread->state = OS_THREAD_STATE_RUNNING;
}

void OSWakeupThread(OSThreadQueue* queue)
{
	OSThread* thread = queue->head;
	while (thread != NULL) {
		OSThread* next = thread->link.next;
		thread->queue  = NULL;
		thread->state  = OS_THREAD_STATE_READY;
		thread->link.next = NULL;
		thread->link.prev = NULL;
		thread = next;
	}
	queue->head = NULL;
	queue->tail = NULL;
	wake_all();
}

// ---------------------------------------------------------------------------
// Mutexes and condition variables
// ---------------------------------------------------------------------------

void OSInitMutex(OSMutex* mutex)
{
	OSInitThreadQueue(&mutex->queue);
	mutex->thread = NULL;
	mutex->count  = 0;
}

void OSLockMutex(OSMutex* mutex)
{
	OSThread* self = OSGetCurrentThread();

	pthread_once(&sOnce, init_once);
	while (mutex->thread != NULL && mutex->thread != self) {
		OSSleepThread(&mutex->queue);
	}
	mutex->thread = self;
	mutex->count++;
}

void OSUnlockMutex(OSMutex* mutex)
{
	OSThread* self = OSGetCurrentThread();
	if (mutex->thread != self) {
		return;
	}
	if (--mutex->count <= 0) {
		mutex->count  = 0;
		mutex->thread = NULL;
		OSWakeupThread(&mutex->queue);
	}
}

BOOL OSTryLockMutex(OSMutex* mutex)
{
	OSThread* self = OSGetCurrentThread();
	if (mutex->thread != NULL && mutex->thread != self) {
		return FALSE;
	}
	mutex->thread = self;
	mutex->count++;
	return TRUE;
}

void OSInitCond(OSCond* cond) { OSInitThreadQueue(&cond->queue); }

void OSWaitCond(OSCond* cond, OSMutex* mutex)
{
	OSThread* self = OSGetCurrentThread();
	s32 count      = mutex->count;

	// Release the mutex entirely (the SDK does the same) and reacquire it with
	// the same recursion depth once we are signalled.
	mutex->count  = 0;
	mutex->thread = NULL;
	OSWakeupThread(&mutex->queue);

	OSSleepThread(&cond->queue);

	while (mutex->thread != NULL && mutex->thread != self) {
		OSSleepThread(&mutex->queue);
	}
	mutex->thread = self;
	mutex->count  = count;
}

void OSSignalCond(OSCond* cond) { OSWakeupThread(&cond->queue); }

// ---------------------------------------------------------------------------
// Message queues
// ---------------------------------------------------------------------------

void OSInitMessageQueue(OSMessageQueue* mq, OSMessage* msgArray, s32 msgCount)
{
	OSInitThreadQueue(&mq->queueSend);
	OSInitThreadQueue(&mq->queueReceive);
	mq->msgArray   = msgArray;
	mq->msgCount   = msgCount;
	mq->firstIndex = 0;
	mq->usedCount  = 0;
}

BOOL OSSendMessage(OSMessageQueue* mq, OSMessage msg, s32 flags)
{
	s32 lastIndex;

	while (mq->usedCount >= mq->msgCount) {
		if (flags != OS_MESSAGE_BLOCK) {
			return FALSE;
		}
		OSSleepThread(&mq->queueSend);
	}

	lastIndex               = (mq->firstIndex + mq->usedCount) % mq->msgCount;
	mq->msgArray[lastIndex] = msg;
	mq->usedCount++;
	OSWakeupThread(&mq->queueReceive);
	return TRUE;
}

BOOL OSJamMessage(OSMessageQueue* mq, OSMessage msg, s32 flags)
{
	while (mq->usedCount >= mq->msgCount) {
		if (flags != OS_MESSAGE_BLOCK) {
			return FALSE;
		}
		OSSleepThread(&mq->queueSend);
	}

	mq->firstIndex = (mq->firstIndex + mq->msgCount - 1) % mq->msgCount;
	mq->msgArray[mq->firstIndex] = msg;
	mq->usedCount++;
	OSWakeupThread(&mq->queueReceive);
	return TRUE;
}

BOOL OSReceiveMessage(OSMessageQueue* mq, OSMessage* msg, s32 flags)
{
	while (mq->usedCount == 0) {
		if (flags != OS_MESSAGE_BLOCK) {
			return FALSE;
		}
		OSSleepThread(&mq->queueReceive);
	}

	if (msg != NULL) {
		*msg = mq->msgArray[mq->firstIndex];
	}
	mq->firstIndex = (mq->firstIndex + 1) % mq->msgCount;
	mq->usedCount--;
	OSWakeupThread(&mq->queueSend);
	return TRUE;
}

// ---------------------------------------------------------------------------
// Interrupt masking.  With the scheduler lock held there is nothing to mask -
// the caller already has exclusive control - so these only need to be honest
// about nesting so that OSRestoreInterrupts doesn't confuse the game.
// ---------------------------------------------------------------------------

static __thread int sInterruptDisableDepth;

BOOL OSDisableInterrupts(void)
{
	BOOL prev = sInterruptDisableDepth == 0;
	sInterruptDisableDepth++;
	return prev;
}

BOOL OSRestoreInterrupts(BOOL level)
{
	if (sInterruptDisableDepth > 0) {
		sInterruptDisableDepth--;
	}
	return level;
}

BOOL OSEnableInterrupts(void)
{
	BOOL prev             = sInterruptDisableDepth == 0;
	sInterruptDisableDepth = 0;
	return prev;
}
