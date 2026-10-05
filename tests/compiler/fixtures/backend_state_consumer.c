extern int external_state;
extern int kernel_state;


int
consume_external_state(void)
{
	return external_state + kernel_state;
}
