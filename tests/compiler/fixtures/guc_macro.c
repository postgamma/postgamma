#include <stdbool.h>


bool enable_hashjoin = true;

#define CURRENT_HASHJOIN() (enable_hashjoin)


int
read_macro_value(void)
{
	return CURRENT_HASHJOIN() ? 1 : 0;
}
