/* Frozen ABI 1.3 declarations for an independently compiled old caller. */
#include <stddef.h>
#include <stdint.h>
typedef int32_t pgm_status;
typedef struct pgm_instance pgm_instance;
typedef struct pgm_error pgm_error;
typedef struct pgm_log_record pgm_log_record;
typedef struct pgm_setting { const char *name; const char *value; } pgm_setting;
typedef void (*pgm_log_callback)(void *, const pgm_log_record *);
typedef struct pgm_instance_options
{
	uint32_t	struct_size;
	uint32_t	create;
	const char *path;
	const char *executable_path;
	const char *resource_root;
	const pgm_setting *settings;
	size_t		setting_count;
	size_t		control_queue_capacity;
	size_t		transport_queue_capacity;
	uint32_t	executor_worker_count;
	uint32_t	execution_queue_capacity;
	uint32_t	logical_umask;
	uint32_t	reserved;
	void	   *user_data;
	size_t		result_buffer_limit;
	size_t		maximum_value_size;
	size_t		event_queue_capacity;
	pgm_log_callback log_callback;
	void	   *log_user_data;
} pgm_instance_options;

#define PGM_INSTANCE_OPTIONS_INIT \
	{sizeof(pgm_instance_options), UINT32_C(0), NULL, NULL, NULL, NULL, 0, \
	 64, 64U * 1024U, UINT32_C(4), UINT32_C(0), UINT32_C(0077), \
	 UINT32_C(0), NULL, 0, 0, 64, NULL, NULL}

pgm_status pgm_instance_open(const pgm_instance_options *, pgm_instance **, pgm_error **);
void pgm_error_free(pgm_error *);
