#define _GNU_SOURCE

#include "postgamma/private/bootstrap_probe.h"

#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>


typedef pgm_bootstrap_result (*PostgammaBootstrapFunction)(
	uint64_t generation,
	const char *data_directory,
	const char *resource_root,
	const char *bootstrap_input);

typedef struct ResourceSnapshot
{
	int descriptors;
	int threads;
	int children;
	int sysv_mappings;
} ResourceSnapshot;

typedef struct SignalSnapshot
{
	struct sigaction actions[4];
	sigset_t	mask;
} SignalSnapshot;

typedef struct BootstrapRun
{
	pgm_bootstrap_result result;
	ResourceSnapshot after;
	uint64_t	system_identifier;
	bool		cwd_restored;
	bool		stdin_restored;
	bool		host_pid_unchanged;
	bool		signals_restored;
	bool		pid_file_absent;
	bool		resources_restored;
	int			driver_error;
} BootstrapRun;


static const int tracked_signals[] = {SIGHUP, SIGINT, SIGTERM, SIGQUIT};

static const char *cluster_directories[] = {
	"global",
	"pg_wal",
	"pg_wal/archive_status",
	"pg_wal/summaries",
	"pg_commit_ts",
	"pg_dynshmem",
	"pg_notify",
	"pg_serial",
	"pg_snapshots",
	"pg_subtrans",
	"pg_twophase",
	"pg_multixact",
	"pg_multixact/members",
	"pg_multixact/offsets",
	"base",
	"base/1",
	"pg_replslot",
	"pg_tblspc",
	"pg_stat",
	"pg_stat_tmp",
	"pg_xact",
	"pg_logical",
	"pg_logical/snapshots",
	"pg_logical/mappings",
};


static int
join_path(char *output, size_t output_size, const char *left,
		  const char *right)
{
	int length = snprintf(output, output_size, "%s/%s", left, right);

	return length < 0 || (size_t) length >= output_size ? ENAMETOOLONG : 0;
}


static int
make_directory(const char *path)
{
	if (mkdir(path, S_IRWXU) == 0)
		return 0;
	return errno == EEXIST ? 0 : errno;
}


static int
write_text_file(const char *path, const char *content)
{
	int descriptor = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC,
						 S_IRUSR | S_IWUSR);
	size_t length = strlen(content);
	ssize_t written;
	int status = 0;

	if (descriptor < 0)
		return errno;
	written = write(descriptor, content, length);
	if (written < 0 || (size_t) written != length)
		status = errno != 0 ? errno : EIO;
	if (close(descriptor) != 0 && status == 0)
		status = errno;
	return status;
}


static int
prepare_cluster(const char *path)
{
	char target[PATH_MAX];
	size_t index;
	int status;

	status = make_directory(path);
	if (status != 0)
		return status;
	for (index = 0;
		 index < sizeof(cluster_directories) / sizeof(cluster_directories[0]);
		 index++)
	{
		status = join_path(target, sizeof(target), path,
					   cluster_directories[index]);
		if (status != 0)
			return status;
		status = make_directory(target);
		if (status != 0)
			return status;
	}
	status = join_path(target, sizeof(target), path, "PG_VERSION");
	if (status == 0)
		status = write_text_file(target, "19\n");
	if (status == 0)
		status = join_path(target, sizeof(target), path, "postgresql.conf");
	if (status == 0)
		status = write_text_file(target, "");
	if (status == 0)
		status = join_path(target, sizeof(target), path,
					   "postgresql.auto.conf");
	if (status == 0)
		status = write_text_file(target, "");
	return status;
}


static int
count_directory(const char *path)
{
	DIR *directory = opendir(path);
	struct dirent *entry;
	int count = 0;

	if (directory == NULL)
		return -1;
	while ((entry = readdir(directory)) != NULL)
	{
		if (strcmp(entry->d_name, ".") != 0 &&
			strcmp(entry->d_name, "..") != 0)
			count++;
	}
	if (closedir(directory) != 0)
		return -1;
	return count;
}


static int
count_children(void)
{
	char path[64];
	char content[4096];
	ssize_t length;
	int descriptor;
	int count = 0;
	bool in_number = false;
	ssize_t index;

	if (snprintf(path, sizeof(path), "/proc/self/task/%ld/children",
				 (long) getpid()) >= (int) sizeof(path))
		return -1;
	descriptor = open(path, O_RDONLY | O_CLOEXEC);
	if (descriptor < 0)
		return -1;
	length = read(descriptor, content, sizeof(content));
	if (close(descriptor) != 0 || length < 0)
		return -1;
	for (index = 0; index < length; index++)
	{
		if (content[index] >= '0' && content[index] <= '9')
		{
			if (!in_number)
				count++;
			in_number = true;
		}
		else
			in_number = false;
	}
	return count;
}


static int
count_sysv_mappings(void)
{
	FILE *maps = fopen("/proc/self/maps", "r");
	char *line = NULL;
	size_t capacity = 0;
	int count = 0;

	if (maps == NULL)
		return -1;
	while (getline(&line, &capacity, maps) >= 0)
	{
		if (strstr(line, "/SYSV") != NULL)
			count++;
	}
	free(line);
	if (fclose(maps) != 0)
		return -1;
	return count;
}


static int
capture_resources(ResourceSnapshot *snapshot)
{
	if (snapshot == NULL)
		return EINVAL;
	snapshot->descriptors = count_directory("/proc/self/fd");
	snapshot->threads = count_directory("/proc/self/task");
	snapshot->children = count_children();
	snapshot->sysv_mappings = count_sysv_mappings();
	return snapshot->descriptors < 0 || snapshot->threads < 0 ||
		snapshot->children < 0 || snapshot->sysv_mappings < 0 ? EIO : 0;
}


static bool
same_resources(const ResourceSnapshot *left, const ResourceSnapshot *right)
{
	return left->descriptors == right->descriptors &&
		left->threads == right->threads &&
		left->children == right->children &&
		left->sysv_mappings == right->sysv_mappings;
}


static int
capture_signals(SignalSnapshot *snapshot)
{
	size_t index;

	if (pthread_sigmask(SIG_SETMASK, NULL, &snapshot->mask) != 0)
		return EIO;
	for (index = 0;
		 index < sizeof(tracked_signals) / sizeof(tracked_signals[0]);
		 index++)
	{
		if (sigaction(tracked_signals[index], NULL,
					  &snapshot->actions[index]) != 0)
			return errno;
	}
	return 0;
}


static int
restore_signals(const SignalSnapshot *snapshot)
{
	size_t index;
	int status = 0;

	for (index = 0;
		 index < sizeof(tracked_signals) / sizeof(tracked_signals[0]);
		 index++)
	{
		if (sigaction(tracked_signals[index], &snapshot->actions[index], NULL) != 0 &&
			status == 0)
			status = errno;
	}
	if (pthread_sigmask(SIG_SETMASK, &snapshot->mask, NULL) != 0 && status == 0)
		status = EIO;
	return status;
}


static bool
same_signal_mask(const sigset_t *left, const sigset_t *right)
{
	int signal_number;

	for (signal_number = 1; signal_number < NSIG; signal_number++)
	{
		if (sigismember(left, signal_number) != sigismember(right, signal_number))
			return false;
	}
	return true;
}


static bool
same_signals(const SignalSnapshot *left, const SignalSnapshot *right)
{
	size_t index;
	const int observable_flags = SA_NOCLDSTOP | SA_NOCLDWAIT | SA_NODEFER |
		SA_ONSTACK | SA_RESETHAND | SA_RESTART | SA_SIGINFO;

	if (!same_signal_mask(&left->mask, &right->mask))
		return false;
	for (index = 0;
		 index < sizeof(tracked_signals) / sizeof(tracked_signals[0]);
		 index++)
	{
		const struct sigaction *a = &left->actions[index];
		const struct sigaction *b = &right->actions[index];

		if (a->sa_handler != b->sa_handler ||
			(a->sa_flags & observable_flags) !=
			(b->sa_flags & observable_flags) ||
			!same_signal_mask(&a->sa_mask, &b->sa_mask))
			return false;
	}
	return true;
}


static uint64_t
read_system_identifier(const char *cluster)
{
	char path[PATH_MAX];
	uint64_t identifier = 0;
	int descriptor;
	ssize_t length;

	if (join_path(path, sizeof(path), cluster, "global/pg_control") != 0)
		return 0;
	descriptor = open(path, O_RDONLY | O_CLOEXEC);
	if (descriptor < 0)
		return 0;
	length = read(descriptor, &identifier, sizeof(identifier));
	if (close(descriptor) != 0 || length != (ssize_t) sizeof(identifier))
		return 0;
	return identifier;
}


static BootstrapRun
run_bootstrap(PostgammaBootstrapFunction bootstrap, uint64_t generation,
			  const char *cluster, const char *resource_root,
			  const char *bootstrap_input, const ResourceSnapshot *baseline,
			  pid_t host_pid)
{
	BootstrapRun run;
	SignalSnapshot before_signals;
	SignalSnapshot after_signals;
	char before_cwd[PATH_MAX];
	char after_cwd[PATH_MAX];
	char pid_path[PATH_MAX];
	struct stat before_stdin;
	struct stat after_stdin;
	int saved_stdin = -1;
	int input = -1;
	int status;

	memset(&run, 0, sizeof(run));
	run.result.status = PGM_BOOTSTRAP_PROBE_FAIL;
	if (getcwd(before_cwd, sizeof(before_cwd)) == NULL ||
		fstat(STDIN_FILENO, &before_stdin) != 0 ||
		capture_signals(&before_signals) != 0)
	{
		run.driver_error = errno != 0 ? errno : EIO;
		return run;
	}
	saved_stdin = dup(STDIN_FILENO);
	input = open(bootstrap_input, O_RDONLY | O_CLOEXEC);
	if (saved_stdin < 0 || input < 0 || dup2(input, STDIN_FILENO) < 0)
	{
		run.driver_error = errno;
		goto restore;
	}
	if (close(input) != 0)
	{
		run.driver_error = errno;
		input = -1;
		goto restore;
	}
	input = -1;
	clearerr(stdin);
	run.result = bootstrap(generation, cluster, resource_root, bootstrap_input);

restore:
	if (input >= 0)
		(void) close(input);
	if (saved_stdin >= 0)
	{
		if (dup2(saved_stdin, STDIN_FILENO) < 0 && run.driver_error == 0)
			run.driver_error = errno;
		if (close(saved_stdin) != 0 && run.driver_error == 0)
			run.driver_error = errno;
		clearerr(stdin);
	}
	if (chdir(before_cwd) != 0 && run.driver_error == 0)
		run.driver_error = errno;
	status = restore_signals(&before_signals);
	if (status != 0 && run.driver_error == 0)
		run.driver_error = status;
	run.cwd_restored = getcwd(after_cwd, sizeof(after_cwd)) != NULL &&
		strcmp(before_cwd, after_cwd) == 0;
	run.stdin_restored = fstat(STDIN_FILENO, &after_stdin) == 0 &&
		before_stdin.st_dev == after_stdin.st_dev &&
		before_stdin.st_ino == after_stdin.st_ino &&
		before_stdin.st_mode == after_stdin.st_mode &&
		before_stdin.st_rdev == after_stdin.st_rdev;
	run.host_pid_unchanged = getpid() == host_pid;
	run.signals_restored = capture_signals(&after_signals) == 0 &&
		same_signals(&before_signals, &after_signals);
	if (join_path(pid_path, sizeof(pid_path), cluster, "postmaster.pid") == 0)
		run.pid_file_absent = access(pid_path, F_OK) != 0 && errno == ENOENT;
	if (capture_resources(&run.after) == 0)
		run.resources_restored = same_resources(baseline, &run.after);
	run.system_identifier = read_system_identifier(cluster);
	return run;
}


static bool
run_passed(const BootstrapRun *run, uint64_t generation)
{
	return run->driver_error == 0 && run->result.status == PGM_BOOTSTRAP_PROBE_PASS &&
		run->result.generation == generation && run->result.exit_code == 0 &&
		run->result.resources_remaining == 0 && run->system_identifier != 0 &&
		run->cwd_restored && run->stdin_restored && run->host_pid_unchanged &&
		run->signals_restored && run->pid_file_absent && run->resources_restored;
}


int
main(int argc, char **argv)
{
	PostgammaBootstrapFunction bootstrap = NULL;
	ResourceSnapshot baseline;
	BootstrapRun first;
	BootstrapRun second;
	char first_cluster[PATH_MAX];
	char second_cluster[PATH_MAX];
	void *handle;
	void *symbol;
	bool passed;
	pid_t host_pid = getpid();

	if (argc != 5)
		return 2;
	if (make_directory(argv[4]) != 0 ||
		join_path(first_cluster, sizeof(first_cluster), argv[4], "cluster-a") != 0 ||
		join_path(second_cluster, sizeof(second_cluster), argv[4], "cluster-b") != 0 ||
		prepare_cluster(first_cluster) != 0 ||
		prepare_cluster(second_cluster) != 0)
		return 3;
	handle = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
	if (handle == NULL)
	{
		fprintf(stderr, "cannot load bootstrap library: %s\n", dlerror());
		return 4;
	}
	symbol = dlsym(handle, "pgm_bootstrap_run");
	if (symbol == NULL || sizeof(symbol) != sizeof(bootstrap))
	{
		fprintf(stderr, "cannot resolve bootstrap probe: %s\n", dlerror());
		return 5;
	}
	memcpy(&bootstrap, &symbol, sizeof(bootstrap));
	if (capture_resources(&baseline) != 0)
		return 6;
	first = run_bootstrap(bootstrap, 1, first_cluster, argv[3], argv[2],
					  &baseline, host_pid);
	second = run_bootstrap(bootstrap, 2, second_cluster, argv[3], argv[2],
					   &baseline, host_pid);
	passed = run_passed(&first, 1) && run_passed(&second, 2) &&
		first.system_identifier != second.system_identifier;
	printf(
		"{\"schema_version\":1,\"kind\":\"postgamma.bootstrap-run\","
		"\"status\":\"%s\",\"library_loads\":1,\"bootstrap_calls\":2,"
		"\"distinct_system_identifiers\":%s,"
		"\"baseline\":{\"host_pid\":%ld,\"descriptors\":%d,\"threads\":%d,\"children\":%d,"
		"\"sysv_mappings\":%d},"
		"\"runs\":["
		"{\"generation\":1,\"status\":%d,\"checks\":%u,\"exit_code\":%d,"
		"\"resources_remaining\":%u,\"system_identifier\":%llu,"
		"\"cwd_restored\":%s,\"stdin_restored\":%s,"
		"\"host_pid_unchanged\":%s,\"signals_restored\":%s,"
		"\"pid_file_absent\":%s,\"resources_restored\":%s,"
		"\"driver_error\":%d},"
		"{\"generation\":2,\"status\":%d,\"checks\":%u,\"exit_code\":%d,"
		"\"resources_remaining\":%u,\"system_identifier\":%llu,"
		"\"cwd_restored\":%s,\"stdin_restored\":%s,"
		"\"host_pid_unchanged\":%s,\"signals_restored\":%s,"
		"\"pid_file_absent\":%s,\"resources_restored\":%s,"
		"\"driver_error\":%d}]}",
		passed ? "pass" : "fail",
		first.system_identifier != 0 && second.system_identifier != 0 &&
			first.system_identifier != second.system_identifier ? "true" : "false",
		(long) host_pid, baseline.descriptors, baseline.threads, baseline.children,
		baseline.sysv_mappings,
		(int) first.result.status, first.result.checks, first.result.exit_code,
		first.result.resources_remaining,
		(unsigned long long) first.system_identifier,
		first.cwd_restored ? "true" : "false",
		first.stdin_restored ? "true" : "false",
		first.host_pid_unchanged ? "true" : "false",
		first.signals_restored ? "true" : "false",
		first.pid_file_absent ? "true" : "false",
		first.resources_restored ? "true" : "false", first.driver_error,
		(int) second.result.status, second.result.checks, second.result.exit_code,
		second.result.resources_remaining,
		(unsigned long long) second.system_identifier,
		second.cwd_restored ? "true" : "false",
		second.stdin_restored ? "true" : "false",
		second.host_pid_unchanged ? "true" : "false",
		second.signals_restored ? "true" : "false",
		second.pid_file_absent ? "true" : "false",
		second.resources_restored ? "true" : "false", second.driver_error);
	return passed ? 0 : 1;
}
