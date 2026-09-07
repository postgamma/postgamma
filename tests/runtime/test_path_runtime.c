#define _GNU_SOURCE

#include "postgamma/guc_runtime.h"
#include "postgamma/path_runtime.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>


int
main(void)
{
	char		template[] = "/tmp/postgamma-path-XXXXXX";
	char	   *root;
	char	   *logical_cwd;
	char	   *resolved;
	char		host_cwd_before[4096];
	char		host_cwd_after[4096];
	char		link_target[32];
	struct stat status_buffer;
	PostgammaPathRuntimeOptions options =
		POSTGAMMA_PATH_RUNTIME_OPTIONS_INIT;
	PostgammaPathRuntimeTelemetry telemetry;
	PostgammaPathRuntime *path_runtime;
	PostgammaInstanceContext instance;
	PostgammaRoleContext role;
	PostgammaExecutionContext execution;
	PostgammaExecutionContext *previous;
	mode_t		host_umask;
	mode_t		host_umask_after;
	int			root_fd;
	int			descriptor;
	ssize_t		link_length;
	FILE	   *stream;

	assert(getcwd(host_cwd_before, sizeof(host_cwd_before)) != NULL);
	host_umask = umask(0);
	(void) umask(host_umask);
	root = mkdtemp(template);
	assert(root != NULL);
	root_fd = open(root, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	assert(root_fd >= 0);
	options.generation = UINT64_C(7000000);
	options.data_directory_fd = root_fd;
	options.canonical_data_directory = root;
	options.canonical_resource_root = root;
	options.logical_umask = 0077;
	assert(postgamma_path_runtime_create(&path_runtime, &options) == 0);

	assert(postgamma_instance_context_init(&instance) == 0);
	postgamma_instance_context_attach_path_runtime(&instance, path_runtime);
	assert(postgamma_role_context_init(&role, &instance) == 0);
	assert(postgamma_execution_context_init(&execution, &instance, &role) == 0);
	previous = postgamma_execution_context_bind(&execution);
	assert(previous == NULL);

	assert(postgamma_path_chdir(root) == 0);
	logical_cwd = postgamma_path_getcwd(NULL, 0);
	assert(logical_cwd != NULL);
	assert(strcmp(logical_cwd, root) == 0);
	free(logical_cwd);
	assert(postgamma_path_umask(0077) == 0077);
	assert(postgamma_path_mkdir("nested", 0777) == 0);
	assert(postgamma_path_stat("nested", &status_buffer) == 0);
	assert((status_buffer.st_mode & 0777) == 0700);

	stream = postgamma_path_fopen("nested/payload", "w+");
	assert(stream != NULL);
	assert(fputs("path-runtime", stream) >= 0);
	assert(fclose(stream) == 0);
	assert(postgamma_path_rename(
		"nested/payload", "nested/renamed") == 0);
	assert(postgamma_path_link(
		"nested/renamed", "nested/hard-link") == 0);
	assert(postgamma_path_symlink("renamed", "nested/symbolic-link") == 0);
	link_length = postgamma_path_readlink(
		"nested/symbolic-link", link_target, sizeof(link_target) - 1);
	assert(link_length == (ssize_t) strlen("renamed"));
	link_target[link_length] = '\0';
	assert(strcmp(link_target, "renamed") == 0);
	assert(postgamma_path_lstat(
		"nested/symbolic-link", &status_buffer) == 0);
	assert(S_ISLNK(status_buffer.st_mode));
	resolved = postgamma_path_realpath("nested/renamed", NULL);
	assert(resolved != NULL);
	assert(strstr(resolved, "/nested/renamed") != NULL);
	free(resolved);
	descriptor = postgamma_path_openat(
		AT_FDCWD, "nested/renamed", O_RDONLY);
	assert(descriptor >= 0);
	assert(close(descriptor) == 0);

	assert(postgamma_path_mkdir("resources", 0777) == 0);
	stream = postgamma_path_fopen("resources/immutable", "w");
	assert(stream != NULL);
	assert(fputs("resource-provider", stream) >= 0);
	assert(fclose(stream) == 0);
	descriptor = postgamma_path_resource_open(
		path_runtime, "resources/immutable", O_RDONLY);
	assert(descriptor >= 0);
	assert(close(descriptor) == 0);
	assert(postgamma_path_resource_name_is_valid("resources/immutable"));
	assert(!postgamma_path_resource_name_is_valid("resources//immutable"));
	assert(!postgamma_path_resource_name_is_valid("resources/"));
	assert(!postgamma_path_resource_name_is_valid("resources\\immutable"));
	assert(!postgamma_path_resource_name_is_valid("resources\nimmutable"));
	errno = 0;
	assert(postgamma_path_resource_open(
		path_runtime, "../outside", O_RDONLY) == -1);
	assert(errno == EINVAL);
	errno = 0;
	assert(postgamma_path_resource_open(
		path_runtime, "/absolute", O_RDONLY) == -1);
	assert(errno == EINVAL);
	errno = 0;
	assert(postgamma_path_resource_open(
		path_runtime, "resources/immutable", O_WRONLY) == -1);
	assert(errno == EINVAL);
	assert(postgamma_path_symlink(
		"immutable", "resources/resource-link") == 0);
	errno = 0;
	assert(postgamma_path_resource_open(
		path_runtime, "resources/resource-link", O_RDONLY) == -1);
	assert(errno == ELOOP);
	assert(postgamma_path_unlink("resources/resource-link") == 0);
	assert(postgamma_path_unlink("resources/immutable") == 0);
	assert(postgamma_path_rmdir("resources") == 0);

	assert(postgamma_path_unlink("nested/symbolic-link") == 0);
	assert(postgamma_path_unlink("nested/hard-link") == 0);
	assert(postgamma_path_remove("nested/renamed") == 0);
	assert(postgamma_path_rmdir("nested") == 0);
	assert(postgamma_path_runtime_telemetry(
		path_runtime, &telemetry) == 0);
	assert(telemetry.generation == options.generation);
	assert(telemetry.relative_operations >= 15);
	assert(telemetry.virtual_chdir_calls == 1);
	assert(telemetry.logical_umask_calls == 1);
	assert(telemetry.resource_operations == 1);
	assert(strcmp(telemetry.canonical_data_directory, root) == 0);
	assert(strcmp(telemetry.canonical_resource_root, root) == 0);

	postgamma_execution_context_restore(&execution, previous);
	postgamma_execution_context_destroy(&execution);
	postgamma_role_context_destroy(&role);
	postgamma_instance_context_detach_path_runtime(&instance, path_runtime);
	postgamma_instance_context_destroy(&instance);
	assert(postgamma_path_runtime_destroy(path_runtime) == 0);
	assert(close(root_fd) == 0);
	assert(rmdir(root) == 0);

	assert(getcwd(host_cwd_after, sizeof(host_cwd_after)) != NULL);
	assert(strcmp(host_cwd_before, host_cwd_after) == 0);
	host_umask_after = umask(0);
	(void) umask(host_umask_after);
	assert(host_umask_after == host_umask);
	return 0;
}
