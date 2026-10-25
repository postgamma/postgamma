#define FIELD(id, local, member, copy, reloc) \
	{id, POSTGAMMA_BACKEND_STATE_OWNER_ROLE, local, \
	 offsetof(PreparationFixtureData, member), sizeof(((PreparationFixtureData *) 0)->member), \
	 _Alignof(PreparationFixtureData), copy, reloc}
FIELD("a", 0, a, true, true),
FIELD("b", 1, b, true, true),
FIELD("largezero", 2, large_zero, false, false),
/* Integers require only integer alignment, unlike the containing struct. */
{"lazy", POSTGAMMA_BACKEND_STATE_OWNER_ROLE, 3, offsetof(PreparationFixtureData, lazy), sizeof(int), _Alignof(int), true, false},
{"nonzero", POSTGAMMA_BACKEND_STATE_OWNER_ROLE, 4, offsetof(PreparationFixtureData, value), sizeof(int), _Alignof(int), true, false},
FIELD("self", 5, self, true, true),
{"session", POSTGAMMA_BACKEND_STATE_OWNER_SESSION, 0, 0, sizeof(void *), _Alignof(void *), false, false},
{"volatile", POSTGAMMA_BACKEND_STATE_OWNER_ROLE, 6, offsetof(PreparationFixtureData, pending), sizeof(int), _Alignof(int), true, false},
FIELD("zero", 7, zero, true, false),
#undef FIELD
