static int trace;


void
postgamma_at_entry(void)
{
	trace = trace * 10 + 4;
}


void
postgamma_before_anchor(void)
{
	trace = trace * 10 + 1;
}


void
postgamma_after_anchor(void)
{
	trace = trace * 10 + 3;
}


void
postgamma_after_declaration(void)
{
	trace = trace * 10 + 6;
}


void
ConditionalAnchor(void)
{
	trace = trace * 10 + 7;
}


void
postgamma_after_conditional(void)
{
	trace = trace * 10 + 8;
}


void
MemoryContextInit(void)
{
	trace = trace * 10 + 2;
}


int
LookupValue(void)
{
	trace = trace * 10 + 5;
	return 0;
}


int
declaration_anchor(void)
{
	int			value = LookupValue();

	return value;
}


int
main(void)
{
	int			expected = 41237856;

	MemoryContextInit();
	if (trace >= 0)
		ConditionalAnchor();
	return declaration_anchor() == 0 && trace == expected ? 0 : 1;
}
