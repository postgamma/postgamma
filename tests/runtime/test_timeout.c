/* Copyright 2026 Shujie Zhang
 * SPDX-License-Identifier: Apache-2.0
 */
#define _POSIX_C_SOURCE 200809L
#include "postgamma/virtual_signal.h"
#include "../check.h"
#include <string.h>
#include <errno.h>
#include <sys/types.h>
#include <unistd.h>

typedef int64_t TimestampTz;
static TimestampTz Now;
typedef struct PostgammaClientSession
{
	PostgammaVirtualTimer timer;
	PostgammaVirtualSignals signals;
	PostgammaBackendRegistry *registry;
	PostgammaBackendHandle handle;
} PostgammaClientSession;
typedef PostgammaClientSession PostgammaStandaloneSignalState;
struct pg_signal_info { pid_t pid; uid_t uid; };
typedef struct pg_signal_info pg_signal_info;
static PostgammaClientSession Session;
static PostgammaClientSession *PostgammaCurrentBackendSession = &Session;
static PostgammaStandaloneSignalState PostgammaStandaloneSignals;
static void *PostgammaCurrentStandaloneInput;
#define Timer Session.timer
#define PG_STATE(name) getpid()
#define postgamma_set_exit_status(kind, code) abort()
static uint64_t Pending;
static bool Paused;
static int Takes;
static int Waits;
static int ClockReads;
static int Fired[2];
static int Holdoff;
static int Wakes;
static int Arms;

#define Assert CHECK
#define elog(...) abort()
#define ereport(...) abort()
#define MemSet memset
#define HOLD_INTERRUPTS() (++Holdoff)
#define RESUME_INTERRUPTS() (--Holdoff)
#define SIGNAL_ARGS int signum __attribute__((unused)), const struct pg_signal_info *info __attribute__((unused))
#define SetLatch(latch) (++Wakes)
#define pqsignal(number, handler) (Session.signals.signal_handlers[number] = (handler))
#define TimestampTzPlusMilliseconds(t, ms) ((t) + (int64_t) (ms) * 1000)

static TimestampTz GetCurrentTimestamp(void) { ClockReads++; return Now; }
bool postgamma_backend_interrupt_poll_required(PostgammaBackendRegistry *registry, PostgammaBackendId id)
{
	(void) registry; (void) id;
	return Pending != 0 || Paused;
}
int postgamma_backend_take_pending_signals(PostgammaBackendRegistry *registry, PostgammaBackendId id, uint64_t *pending)
{
	(void) registry; (void) id;
	Takes++; *pending = Pending; Pending = 0; return 0;
}
int postgamma_backend_wait_while_paused(PostgammaBackendRegistry *registry, PostgammaBackendId id)
{
	(void) registry; (void) id;
	Waits++; Paused = false; return 0;
}
int postgamma_backend_requeue_pending_signals(PostgammaBackendRegistry *registry, PostgammaBackendId id, uint64_t pending)
{
	(void) registry; (void) id;
	Pending |= pending; return 0;
}
static void TimestampDifference(TimestampTz a, TimestampTz b, long *s, int *us)
{
	*s = (b - a) / 1000000;
	*us = (b - a) % 1000000;
}
static void postgamma_runtime_contract_violation(void) { abort(); }
static int set_timer(int which, const struct itimerval *value, struct itimerval *old)
{
	Arms++;
	return postgamma_virtual_timer_set(&Timer, which, value, old, Now);
}
#define setitimer set_timer

/* Actual upstream timeout module and runtime signal dispatcher. */
#include "timeout_module.inc"

static void first(void) { Fired[0]++; }
static void second(void) { Fired[1]++; }
static void external_signal(SIGNAL_ARGS) { Fired[1]++; }
static void poll_at(TimestampTz now)
{
	Now = now;
	postgamma_dispatch_pending_signals();
	CHECK(Holdoff == 0);
}

int main(void)
{
	sigemptyset(&Session.signals.logical_signal_mask);
	InitializeTimeouts();
	RegisterTimeout(STATEMENT_TIMEOUT, first);
	RegisterTimeout(LOCK_TIMEOUT, second);
	/* A canceled alarm may still arrive; a later logical arm must not be lost. */
	enable_timeout_after(STATEMENT_TIMEOUT, 10);
	CHECK(Timer.timeout_armed && signal_pending);
	disable_timeout(STATEMENT_TIMEOUT, false);
	CHECK(Timer.timeout_armed && signal_pending && !alarm_enabled);
	CHECK(postgamma_virtual_timer_adjust_wait(&Timer, -1, Now) == 10);
	enable_timeout_after(STATEMENT_TIMEOUT, 20);
	poll_at(10000);
	CHECK(Fired[0] == 0);
	poll_at(20000);
	CHECK(Fired[0] == 1 && get_timeout_indicator(STATEMENT_TIMEOUT, false));
	CHECK(!Timer.timeout_armed);
	/* An early leftover alarm must still schedule the remaining deadline. */
	enable_timeout_after(STATEMENT_TIMEOUT, 10);
	enable_timeout_after(LOCK_TIMEOUT, 30);
	disable_timeout(STATEMENT_TIMEOUT, true);
	CHECK(Timer.timeout_armed && signal_pending);
	poll_at(30000);
	CHECK(Fired[1] == 0 && Timer.timeout_armed);
	poll_at(50000);
	CHECK(Fired[1] == 1);
	/* Logical cancellation retains upstream's deferred physical alarm. */
	for (int mode = 0; mode < 3; mode++)
	{
		DisableTimeoutParams canceled[] = {{STATEMENT_TIMEOUT, false}, {LOCK_TIMEOUT, false}};
		enable_timeout_after(STATEMENT_TIMEOUT, 10);
		enable_timeout_after(LOCK_TIMEOUT, 20);
		if (mode == 0) disable_timeouts(canceled, 2);
		if (mode == 1) disable_all_timeouts(false);
		if (mode == 2) InitializeTimeouts();
		CHECK(Timer.timeout_armed && signal_pending && !alarm_enabled);
	}
	RegisterTimeout(STATEMENT_TIMEOUT, first);
	RegisterTimeout(LOCK_TIMEOUT, second);
	/* Repeat delivery, replacement, and error-recovery rescheduling. */
	enable_timeout_every(STATEMENT_TIMEOUT, Now + 1000, 1);
	poll_at(51000);
	CHECK(Fired[0] == 2 && Timer.timeout_armed);
	poll_at(52000);
	CHECK(Fired[0] == 3 && Timer.timeout_armed);
	enable_timeout_after(STATEMENT_TIMEOUT, 2);
	reschedule_timeouts();
	poll_at(54000);
	CHECK(Fired[0] == 4 && !Timer.timeout_armed);
	/* Poll every time, but only dequeue signals / enter pause handling if due. */
	enable_timeout_after(STATEMENT_TIMEOUT, 1000);
	Takes = Waits = ClockReads = 0;
	for (int i = 0; i < 100; i++) postgamma_dispatch_pending_signals();
	CHECK(ClockReads == 100 && Takes == 0 && Waits == 0);
	pqsignal(SIGUSR1, external_signal);
	Pending = UINT64_C(1) << (SIGUSR1 - 1);
	postgamma_dispatch_pending_signals();
	CHECK(Takes == 1 && Waits == 1 && Pending == 0 && Fired[1] == 2);
	Paused = true;
	postgamma_dispatch_pending_signals();
	CHECK(Takes == 2 && Waits == 2 && !Paused);
	disable_all_timeouts(false);
	/* Deferred cancellation still suppresses the actual timeout callback. */
	enable_timeout_after(STATEMENT_TIMEOUT, 1);
	int arms = Arms;
	disable_all_timeouts(false);
	CHECK(Timer.timeout_armed && signal_pending && Arms == arms);
	poll_at(55000);
	CHECK(Fired[0] == 4 && !signal_pending && Wakes > 0);
	return 0;
}
