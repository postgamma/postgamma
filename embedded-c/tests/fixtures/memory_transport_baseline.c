#define _GNU_SOURCE

#include <dirent.h>
#include <unistd.h>

static DIR *baseline_opendir(const char *path);
static struct dirent *baseline_readdir(DIR *directory);
static int baseline_access(const char *path, int mode);

/* Inject procfs observations without changing the transport or its test. */
#define opendir baseline_opendir
#define readdir baseline_readdir
#define access baseline_access
#define main transport_test_main
#include "../test_memory_transport.c"
#undef main
#undef readdir
#undef opendir
#undef access

static DIR *task_directory;
static unsigned int task_samples;
static unsigned int task_entries;
static unsigned int exit_checks;
static bool persistent_thread;
static bool helper_thread;
static bool stuck_warm_thread;

static int
baseline_access(const char *path, int mode)
{
	if (strncmp(path, "/proc/self/task/", strlen("/proc/self/task/")) == 0)
	{
		if (++exit_checks <= 3 || stuck_warm_thread)
			return 0;
		errno = ENOENT;
		return -1;
	}
	return access(path, mode);
}

static DIR *
baseline_opendir(const char *path)
{
	DIR *directory = opendir(path);

	task_directory = NULL;
	if (strcmp(path, "/proc/self/task") == 0)
	{
		task_directory = directory;
		task_samples++;
		task_entries = 0;
	}
	return directory;
}

static struct dirent *
baseline_readdir(DIR *directory)
{
	static struct dirent entry;

	if (directory == task_directory)
	{
		unsigned int count = 1 + helper_thread;

		if (persistent_thread || (exit_checks == 0 && task_samples == 1))
			count++;

		if (task_entries++ >= count)
			return NULL;
		strcpy(entry.d_name, task_entries == 1 ? "main" : "worker");
		return &entry;
	}
	return readdir(directory);
}

int
main(int argc, char **argv)
{
	int baseline_descriptors;
	int final_descriptors;
	int final_threads;

	if (argc < 2)
		return 2;
	helper_thread = strcmp(argv[1], "helper-thread") == 0;
	stuck_warm_thread = strcmp(argv[1], "stuck-warm-thread") == 0;
	if (strcmp(argv[1], "transient-thread") == 0 ||
		helper_thread || stuck_warm_thread)
		return transport_test_main(argc - 1, argv + 1);

	baseline_descriptors = count_directory("/proc/self/fd");
	if (strcmp(argv[1], "persistent-thread") == 0)
		persistent_thread = true;
	else if (strcmp(argv[1], "leaked-descriptor") == 0)
	{
		/* Keep an actual extra descriptor open until the helper rejects it. */
		if (fopen("/dev/null", "r") == NULL)
			return 2;
	}
	else
		return 2;
	wait_for_process_baseline(baseline_descriptors, 1,
						  &final_descriptors, &final_threads);
	fprintf(stderr, "resource leak was not rejected\n");
	return 1;
}
