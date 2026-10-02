#define _GNU_SOURCE
#include "postgamma/virtual_signal.h"
#include "../check.h"
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>

static _Thread_local unsigned delivered;
static _Thread_local PostgammaVirtualSignals *current;

static void
receive(int number, const struct pg_signal_info *info)
{
    (void) info;
    CHECK(number == SIGUSR1 || number == SIGALRM);
    CHECK(current->dispatching_signals);
    delivered++;
}

static uint64_t
bit(int number)
{
    return UINT64_C(1) << (number - 1);
}

static void *
exercise(void *unused)
{
    PostgammaVirtualSignals states[2] = {0};
    PostgammaVirtualTimer timer = {0};
    sigset_t host_before, host_after, block, old;
    struct itimerval value = {0}, previous;
    PostgammaSignalDispatchResult result;
    (void) unused;
    CHECK(pthread_sigmask(SIG_SETMASK, NULL, &host_before) == 0);
    sigemptyset(&block);
    sigaddset(&block, SIGUSR1);
    for (int i = 0; i < 2; i++)
    {
        sigemptyset(&states[i].logical_signal_mask);
        states[i].signal_handlers[SIGUSR1] = receive;
        states[i].signal_handlers[SIGALRM] = receive;
    }
    for (int turn = 0; turn < 1000; turn++)
    {
        current = &states[turn % 2];
        CHECK(postgamma_virtual_signal_mask(&current->logical_signal_mask,
                                             SIG_BLOCK, &block, &old) == 0);
        unsigned before = delivered;
        result = postgamma_virtual_signals_dispatch(current, bit(SIGUSR1), true, NULL);
        CHECK(result.blocked == bit(SIGUSR1) && delivered == before);
        CHECK(result.exit_kind == POSTGAMMA_SIGNAL_NO_EXIT);
        CHECK(!current->dispatching_signals);
        CHECK(postgamma_virtual_signal_mask(&current->logical_signal_mask,
                                             SIG_SETMASK, &old, NULL) == 0);
        result = postgamma_virtual_signals_dispatch(current, result.blocked, true, NULL);
        CHECK(delivered == before + 1 && result.blocked == 0);
    }
    current = &states[0];
    result = postgamma_virtual_signals_dispatch(current, bit(SIGTERM), true, NULL);
    CHECK(result.exit_kind == POSTGAMMA_SIGNAL_DEFAULT_EXIT && result.signal_number == SIGTERM);
    result = postgamma_virtual_signals_dispatch(current, bit(SIGQUIT), true, NULL);
    CHECK(result.exit_kind == POSTGAMMA_SIGNAL_QUICK_EXIT && result.signal_number == SIGQUIT);
    current->signal_handlers[SIGTERM] = POSTGAMMA_SIGNAL_IGNORE;
    result = postgamma_virtual_signals_dispatch(current, bit(SIGTERM), true, NULL);
    CHECK(result.exit_kind == POSTGAMMA_SIGNAL_NO_EXIT);
    result = postgamma_virtual_signals_dispatch(current, bit(SIGQUIT), false, NULL);
    CHECK(result.exit_kind == POSTGAMMA_SIGNAL_NO_EXIT);
    CHECK(postgamma_virtual_signal_mask(&current->logical_signal_mask, SIG_BLOCK, &block, NULL) == 0);
    (void) postgamma_virtual_signals_dispatch(current, bit(SIGUSR1), false, NULL);
    CHECK(current->pending_signals == bit(SIGUSR1));
    CHECK(postgamma_virtual_signal_mask(&current->logical_signal_mask, SIG_UNBLOCK, &block, NULL) == 0);
    CHECK(postgamma_virtual_signal_mask(&current->logical_signal_mask, -99, &block, NULL) == -1);
    CHECK(errno == EINVAL);

    value.it_value.tv_usec = 1501;
    CHECK(postgamma_virtual_timer_set(&timer, ITIMER_REAL, &value, NULL, 1000) == 0);
    CHECK(postgamma_virtual_timer_adjust_wait(&timer, -1, 1000) == 2);
    CHECK(postgamma_virtual_timer_wait_expired(&timer, -1));
    CHECK(!postgamma_virtual_timer_wait_expired(&timer, -1));
    CHECK(!postgamma_virtual_timer_due(&timer, 2500));
    CHECK(postgamma_virtual_timer_due(&timer, 2501));
    CHECK(!postgamma_virtual_timer_due(&timer, 2502));
    (void) postgamma_virtual_signals_dispatch(current, bit(SIGALRM), true, NULL);
    value.it_interval.tv_usec = 200;
    value.it_value.tv_usec = 100;
    CHECK(postgamma_virtual_timer_set(&timer, ITIMER_REAL, &value, &previous, 1000) == 0);
    CHECK(previous.it_value.tv_sec == 0 && previous.it_value.tv_usec == 0);
    CHECK(postgamma_virtual_timer_due(&timer, 1600));
    CHECK(timer.timeout_due_at == 1700); /* skip missed periods without timer drift */
    value.it_value.tv_usec = 0;
    CHECK(postgamma_virtual_timer_set(&timer, ITIMER_REAL, &value, &previous, 1600) == 0);
    CHECK(previous.it_value.tv_usec == 100 && !timer.timeout_armed);
    value.it_value.tv_usec = 1000000;
    CHECK(postgamma_virtual_timer_set(&timer, ITIMER_REAL, &value, NULL, 0) == -1);
    CHECK(errno == EINVAL && !timer.timeout_armed);
    value.it_value.tv_usec = 1;
    CHECK(postgamma_virtual_timer_set(&timer, ITIMER_REAL, &value, NULL, INT64_MAX) == -1);
    CHECK(errno == EOVERFLOW && !timer.timeout_armed);
    CHECK(postgamma_virtual_timer_set(&timer, ITIMER_REAL, &value, NULL, INT64_MAX - 1) == 0);
    CHECK(postgamma_virtual_timer_due(&timer, INT64_MAX));
    CHECK(!timer.timeout_armed); /* no representable next periodic deadline */
    CHECK(pthread_sigmask(SIG_SETMASK, NULL, &host_after) == 0);
    for (int i = 1; i <= POSTGAMMA_BACKEND_SIGNAL_COUNT; i++)
        CHECK(sigismember(&host_before, i) == sigismember(&host_after, i));
    return NULL;
}

int
main(void)
{
    pthread_t carriers[2];
    for (int i = 0; i < 2; i++)
        CHECK(pthread_create(&carriers[i], NULL, exercise, NULL) == 0);
    for (int i = 0; i < 2; i++)
        CHECK(pthread_join(carriers[i], NULL) == 0);
    puts("virtual signals: masks, dispatch, periodic timers and carrier isolation passed");
    return 0;
}
