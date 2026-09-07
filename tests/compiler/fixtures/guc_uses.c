#include <stdbool.h>


bool enable_hashjoin = true;
int work_mem = 4096;

struct LocalSettings
{
	bool enable_hashjoin;
	int work_mem;
};


int
read_values(void)
{
	return work_mem + (enable_hashjoin ? 1 : 0);
}


void
write_values(void)
{
	work_mem = 8192;
	enable_hashjoin = false;
}


void
update_work_mem(void)
{
	work_mem += 16;
	++work_mem;
}


void *
global_address(bool choose_work_mem)
{
	return choose_work_mem ? (void *) &work_mem : (void *) &enable_hashjoin;
}


int
shadow_values(int work_mem, struct LocalSettings *settings)
{
	bool enable_hashjoin = settings->enable_hashjoin;

	settings->work_mem += 1;
	return work_mem + settings->work_mem + (enable_hashjoin ? 1 : 0);
}
