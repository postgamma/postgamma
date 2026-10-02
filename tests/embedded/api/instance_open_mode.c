/* Public C entrypoints with a controlled initdb boundary; no PostgreSQL kernel. */
#include "postgamma/private/public_runtime.h"
#include "postgamma/private/initdb_host.h"
#include "postgamma/private/postgres_static_modules.h"
#include "../../check.h"
#include <errno.h>
#include <stdio.h>
#include <string.h>
static int creates;
static bool missing;
const PostgammaStaticModuleProvider *
postgamma_product_static_module_provider(void)
{
    return NULL;
}

int
postgamma_static_module_provider_install(const PostgammaStaticModuleProvider *provider)
{
    (void) provider;
    return 0;
}

const PostgammaKernelEntrypoints *
postgamma_embedded_kernel_entrypoints(void)
{
    return NULL;
}

int
postgamma_initdb_create_locked(const PostgammaInitdbOptions *options,
                              PostgammaInitdbResult *result,
                              PostgammaDataDirectoryLock **lock)
{
    (void) lock;
    creates++;
    missing = options->require_missing;
    snprintf(result->message, sizeof(result->message), "fixture destination exists");
    return EEXIST;
}

int
postgamma_public_event_router_create(pgm_instance *instance, size_t capacity,
                                    pgm_log_callback callback, void *argument)
{
    (void) instance;
    (void) capacity;
    (void) callback;
    (void) argument;
    return EIO;
}

int
postgamma_public_event_router_destroy(pgm_instance *instance)
{
    (void) instance;
    return 0;
}

int
postgamma_public_event_enqueue_kernel_log(void *argument, uint64_t generation,
                                        const PostgammaKernelLogRecord *record)
{
    (void) argument;
    (void) generation;
    (void) record;
    return 0;
}

void test_legacy_open(const char *path);

int main(int argc, char **argv)
{
    pgm_instance_options options = PGM_INSTANCE_OPTIONS_INIT;
    pgm_instance *instance = NULL;
    pgm_error *error = NULL;
    pgm_open_mode mode;
    CHECK(argc == 2);
    options.path = argv[1];
    options.resource_root = argv[1];
    options.executable_path = "/proc/self/exe";
    CHECK(pgm_abi_version() == 65540);
    for (mode = -1; mode <= 3; mode++)
    {
        pgm_status status;
        creates = 0;
        status = pgm_instance_open_with_mode(&options, mode, &instance, &error);
        CHECK(status != PGM_STATUS_OK && instance == NULL && error != NULL);
        if (mode < 0 || mode > 2)
            CHECK(status == PGM_STATUS_INVALID_ARGUMENT && creates == 0);
        else if (mode == PGM_OPEN_EXISTING)
            CHECK(creates == 0);
        else
        {
            CHECK(creates == 1 && missing == (mode == PGM_CREATE_NEW));
            CHECK(status == PGM_STATUS_INSTANCE_FAILED);
            CHECK(pgm_error_status(error) == status);
            CHECK(strcmp(pgm_error_detail(error), "fixture destination exists") == 0);
        }
        pgm_error_free(error);
    }
    options.create = 1;
    creates = 0;
    CHECK(pgm_instance_open_with_mode(&options, PGM_CREATE_NEW,
                                      &instance, &error) == PGM_STATUS_INVALID_ARGUMENT);
    CHECK(creates == 0);
    pgm_error_free(error);
    options.create = 0;
    options.reserved = 1;
    CHECK(pgm_instance_open_with_mode(&options, PGM_CREATE_NEW,
                                      &instance, &error) == PGM_STATUS_INVALID_ARGUMENT);
    pgm_error_free(error);
    options.reserved = 0;
    options.struct_size--;
    CHECK(pgm_instance_open_with_mode(&options, PGM_CREATE_NEW,
                                      &instance, &error) == PGM_STATUS_INVALID_ARGUMENT);
    pgm_error_free(error);
    CHECK(pgm_instance_open_with_mode(NULL, PGM_CREATE_NEW,
                                      &instance, &error) == PGM_STATUS_INVALID_ARGUMENT);
    pgm_error_free(error);
    creates = 0;
    test_legacy_open(argv[1]);
    CHECK(creates == 1 && !missing);
    puts("open mode and old caller boundary: pass");
    return 0;
}
