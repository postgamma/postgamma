/* Mirrors the small qualified fixture; production facts come from the generator. */
#ifndef POSTGAMMA_BACKEND_STATE_DIRECT_H
#define POSTGAMMA_BACKEND_STATE_DIRECT_H
#define PostgammaCurrentPreparedRoleState PostgammaFixturePreparedRoleState
static inline __attribute__((always_inline)) size_t
postgamma_backend_state_prepared_offset(PostgammaBackendStateSlot slot)
{
	switch (slot)
	{
		case S_A: return offsetof(PreparationFixtureData, a);
		case S_B: return offsetof(PreparationFixtureData, b);
		case S_LARGE_ZERO: return offsetof(PreparationFixtureData, large_zero);
		case S_VALUE: return offsetof(PreparationFixtureData, value);
		case S_SELF: return offsetof(PreparationFixtureData, self);
		case S_ZERO: return offsetof(PreparationFixtureData, zero);
		case S_VOLATILE: return offsetof(PreparationFixtureData, pending);
		default: return SIZE_MAX;
	}
}
#endif
