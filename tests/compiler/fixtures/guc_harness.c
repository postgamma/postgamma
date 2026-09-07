#include "postgamma/guc_runtime.h"

#include <assert.h>
#include <stdbool.h>


struct LocalSettings
{
	bool enable_hashjoin;
	int work_mem;
};

extern int read_values(void);
extern void write_values(void);
extern void update_work_mem(void);
extern void *global_address(bool choose_work_mem);
extern int shadow_values(int work_mem, struct LocalSettings *settings);


int
main(void)
{
	PostgammaInstanceContext instance;
	PostgammaRoleContext role;
	PostgammaExecutionContext session;
	PostgammaExecutionContext *previous;
	struct LocalSettings local = {.enable_hashjoin = true, .work_mem = 10};

	assert(postgamma_instance_context_init(&instance) == 0);
	assert(postgamma_role_context_init(&role, &instance) == 0);
	assert(postgamma_execution_context_init(&session, &instance, &role) == 0);
	session.session_guc.enable_hashjoin = true;
	session.session_guc.work_mem = 4096;
	previous = postgamma_execution_context_bind(&session);
	assert(read_values() == 4097);
	assert(global_address(true) == &session.session_guc.work_mem);
	assert(global_address(false) == &session.session_guc.enable_hashjoin);
	write_values();
	assert(session.session_guc.work_mem == 8192);
	assert(!session.session_guc.enable_hashjoin);
	update_work_mem();
	assert(session.session_guc.work_mem == 8209);
	assert(shadow_values(7, &local) == 19);
	assert(local.work_mem == 11);
	assert(session.session_guc.work_mem == 8209);
	postgamma_execution_context_restore(&session, previous);
	return 0;
}
