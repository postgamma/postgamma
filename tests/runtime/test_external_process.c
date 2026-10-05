#define _GNU_SOURCE
#include "postgamma/external_process.h"
#include "postgamma/contract_runtime.h"
#include "../check.h"
#include <errno.h>
#include <setjmp.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>

static sigjmp_buf exit_jump;
static bool interrupt_wait;

void
postgamma_runtime_contract_violation(void)
{
    abort();
}

static void
dispatch(void)
{
    if (interrupt_wait)
        siglongjmp(exit_jump, 1);
}

static void
require_no_children(PostgammaExternalProcessState *state)
{
    PostgammaInstanceRuntimeTelemetry telemetry;
    CHECK(postgamma_instance_runtime_telemetry(state->runtime, &telemetry) == 0);
    CHECK(telemetry.external_processes_started == telemetry.external_processes_finished);
    CHECK(telemetry.external_process_reservations == 0);
    CHECK(telemetry.external_processes_active == 0);
    CHECK(state->system_child_pid == 0);
}

int
main(void)
{
    PostgammaInstanceRuntimeOptions options = POSTGAMMA_INSTANCE_RUNTIME_OPTIONS_INIT;
    PostgammaExternalProcessState *state = calloc(1, sizeof(*state));
    struct sigaction ignored = {0}, old_int, old_quit, current;
    FILE *stream;
    char text[16];
    int status;
    CHECK(state != NULL);
    state->dispatch = dispatch;
    options.profile = POSTGAMMA_RUNTIME_PROFILE_THREADED_SERVER;
    CHECK(postgamma_instance_runtime_create(&state->runtime, &options) == 0);
    ignored.sa_handler = SIG_IGN;
    sigemptyset(&ignored.sa_mask);
    CHECK(sigaction(SIGINT, &ignored, &old_int) == 0);
    CHECK(sigaction(SIGQUIT, &ignored, &old_quit) == 0);
    status = postgamma_external_system(state, "exit 7");
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 7);
    status = postgamma_external_system(state, "kill -TERM $$");
    CHECK(WIFSIGNALED(status) && WTERMSIG(status) == SIGTERM);
    stream = postgamma_external_popen(state, "printf hello", "re");
    CHECK(stream != NULL && fgets(text, sizeof(text), stream) != NULL);
    CHECK(strcmp(text, "hello") == 0);
    CHECK(postgamma_external_pclose(state, stream) == 0);
    stream = postgamma_external_popen(state, "read value; test \"$value\" = hello", "w");
    CHECK(stream != NULL && fputs("hello\n", stream) >= 0);
    CHECK(postgamma_external_pclose(state, stream) == 0);
    errno = 0;
    CHECK(postgamma_external_popen(state, "true", "a") == NULL && errno == EINVAL);
    require_no_children(state);

    /* An error or virtual signal can leave the wait through a nonlocal exit. */
    interrupt_wait = true;
    if (sigsetjmp(exit_jump, 1) == 0)
    {
        (void) postgamma_external_system(state, "exec sleep 30");
        CHECK(false);
    }
    CHECK(state->system_child_pid != 0);
    CHECK(postgamma_instance_runtime_destroy(state->runtime) == EBUSY);
    postgamma_cleanup_external_children(state);
    require_no_children(state);
    stream = postgamma_external_popen(state, "exec sleep 30", "r");
    CHECK(stream != NULL);
    if (sigsetjmp(exit_jump, 1) == 0)
    {
        (void) postgamma_external_pclose(state, stream);
        CHECK(false);
    }
    postgamma_cleanup_external_children(state);
    postgamma_release_external_pipes(state);
    CHECK(state->external_pipes == NULL);
    require_no_children(state);
    CHECK(sigaction(SIGINT, NULL, &current) == 0 && current.sa_handler == SIG_IGN);
    CHECK(sigaction(SIGQUIT, NULL, &current) == 0 && current.sa_handler == SIG_IGN);
    CHECK(sigaction(SIGINT, &old_int, NULL) == 0);
    CHECK(sigaction(SIGQUIT, &old_quit, NULL) == 0);
    CHECK(postgamma_instance_runtime_destroy(state->runtime) == 0);

    options.profile = POSTGAMMA_RUNTIME_PROFILE_EMBEDDED;
    CHECK(postgamma_instance_runtime_create(&state->runtime, &options) == 0);
    CHECK(postgamma_external_system(state, "exit 0") == -1 && errno == ENOTSUP);
    CHECK(postgamma_external_popen(state, "exit 0", "r") == NULL && errno == ENOTSUP);
    require_no_children(state);
    CHECK(postgamma_instance_runtime_destroy(state->runtime) == 0);
    free(state);
    puts("external processes: exit, signal, pipe IO and interrupted cleanup passed");
    return 0;
}
