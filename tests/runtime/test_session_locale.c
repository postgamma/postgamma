#define _GNU_SOURCE
#include "postgamma/session_locale.h"
#include "../check.h"
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>

static void *
exercise_sessions(void *unused)
{
    PostgammaSessionLocale first = {0}, second = {0};
    PostgammaSessionLocale *sessions[] = {&first, &second};
    const char *names[] = {"C", "POSIX"};
    locale_t carrier_locale = uselocale((locale_t) 0);
    (void) unused;
    for (int turn = 0; turn < 2000; turn++)
    {
        PostgammaSessionLocale *state = sessions[turn % 2];
        const char *name = names[turn % 2];
        locale_t previous = (locale_t) 0;
        CHECK(postgamma_session_locale_bind(state, &previous) == 0);
        if (turn < 2)
            CHECK(strcmp(postgamma_session_locale_set(state, LC_NUMERIC, name), name) == 0);
        CHECK(strcmp(postgamma_session_locale_set(state, LC_NUMERIC, NULL), name) == 0);
        CHECK(uselocale((locale_t) 0) == state->session_locale);
        errno = 0;
        CHECK(postgamma_session_locale_set(state, LC_ALL, "C") == NULL && errno == EINVAL);
        CHECK(postgamma_session_locale_set(state, LC_NUMERIC, "postgamma-missing-locale") == NULL);
        CHECK(strcmp(postgamma_session_locale_set(state, LC_NUMERIC, NULL), name) == 0);
        CHECK(uselocale((locale_t) 0) == state->session_locale);
        CHECK(postgamma_session_locale_unbind(&previous) == 0 && previous == (locale_t) 0);
        CHECK(uselocale((locale_t) 0) == carrier_locale);
    }
    postgamma_session_locale_destroy(&first);
    postgamma_session_locale_destroy(&second);
    CHECK(first.session_locale == (locale_t) 0 && second.session_locale == (locale_t) 0);
    return NULL;
}

int
main(void)
{
    pthread_t threads[2];
    char global_name[128];
    snprintf(global_name, sizeof(global_name), "%s", setlocale(LC_NUMERIC, NULL));
    for (int i = 0; i < 2; i++)
        CHECK(pthread_create(&threads[i], NULL, exercise_sessions, NULL) == 0);
    for (int i = 0; i < 2; i++)
        CHECK(pthread_join(threads[i], NULL) == 0);
    CHECK(strcmp(global_name, setlocale(LC_NUMERIC, NULL)) == 0);
    puts("session locale: two carriers, isolated state and host locale preserved");
    return 0;
}
