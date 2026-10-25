/* A small layout exercises cycles independently of a PostgreSQL release. */
#ifndef POSTGAMMA_BACKEND_STATE_LAYOUT_H
#define POSTGAMMA_BACKEND_STATE_LAYOUT_H
#define POSTGAMMA_BACKEND_STATE_LAYOUT_ID "preparation-fixture-v1"
typedef enum PostgammaBackendStateSlot
{
	S_A, S_B, S_LARGE_ZERO, S_LAZY, S_VALUE, S_SELF, S_SESSION, S_VOLATILE, S_ZERO,
	POSTGAMMA_BACKEND_STATE_SLOT_COUNT
} PostgammaBackendStateSlot;
typedef struct PreparationFixtureData
{
	void *a;
	void *b;
	int lazy;
	int value;
	struct { void *next; void *previous; } self;
	void *zero;
	volatile int pending;
	unsigned char large_zero[128 * 1024];
} PreparationFixtureData;
#define POSTGAMMA_BACKEND_INSTANCE_STATE_BYTES 0
#define POSTGAMMA_BACKEND_INSTANCE_STATE_SLOTS 0
#define POSTGAMMA_BACKEND_ROLE_STATE_BYTES sizeof(PreparationFixtureData)
#define POSTGAMMA_BACKEND_ROLE_STATE_SLOTS 8
#define POSTGAMMA_BACKEND_SESSION_STATE_BYTES sizeof(void *)
#define POSTGAMMA_BACKEND_SESSION_STATE_SLOTS 1
#endif
