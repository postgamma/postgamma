#include "instance_options_v1_3.h"
#include "../../check.h"

void test_legacy_open(const char *path)
{
    pgm_instance_options options = PGM_INSTANCE_OPTIONS_INIT;
    pgm_instance *instance = NULL;
    pgm_error *error = NULL;
    options.path = path;
    options.executable_path = "/proc/self/exe";
    options.resource_root = path;
    options.create = 1;
    CHECK(pgm_instance_open(&options, &instance, &error) != 0);
    CHECK(instance == NULL && error != NULL);
    pgm_error_free(error);
}
