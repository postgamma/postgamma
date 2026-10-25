/* Copyright 2026 Shujie Zhang
 * SPDX-License-Identifier: Apache-2.0
 */
#include "../check.h"
#include <pthread.h>
#include <stdatomic.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

typedef struct PostgammaBuiltinGucDescriptor
{
	const char *name;
	int vartype;
} PostgammaBuiltinGucDescriptor;
static const PostgammaBuiltinGucDescriptor PostgammaBuiltinGucs[] = {
	{"a", 1}, {"b", 2}, {"c", 3}};
/* PostgreSQL's compiled order need not match the sorted generated catalog. */
static PostgammaBuiltinGucDescriptor ConfigureNames[] = {
	{"c", 3}, {"a", 1}, {"b", 2}, {NULL, 0}};
static _Atomic unsigned Comparisons;
static int compare(const char *a, const char *b)
{
	atomic_fetch_add(&Comparisons, 1);
	return strcmp(a, b);
}
static void postgamma_runtime_contract_violation(void) { abort(); }
#define strcmp compare
#define lengthof(a) ((int) (sizeof(a) / sizeof((a)[0])))
#define elog(...) _exit(37)
#include "guc_catalog.inc"
#undef strcmp

static void *reader(void *unused)
{
	(void) unused;
	for (int i = 0; i < 1000; i++)
	{
		const PostgammaBuiltinGucDescriptor *const *catalog = postgamma_builtin_guc_catalog();
		CHECK(catalog[0] == &PostgammaBuiltinGucs[2]);
		CHECK(catalog[1] == &PostgammaBuiltinGucs[0]);
		CHECK(catalog[2] == &PostgammaBuiltinGucs[1]);
	}
	return NULL;
}

int main(void)
{
	pthread_t threads[8];
	/* Each invalid catalog must fail before publishing any usable mapping. */
	for (int mode = 0; mode < 3; mode++)
	{
		int status;
		pid_t pid = fork();
		CHECK(pid >= 0);
		if (pid == 0)
		{
			if (mode == 0) ConfigureNames[2].name = NULL;
			if (mode == 1) ConfigureNames[1].name = "unknown";
			if (mode == 2) ConfigureNames[0].vartype = 2;
			(void) postgamma_builtin_guc_catalog();
			_exit(1);
		}
		CHECK(waitpid(pid, &status, 0) == pid);
		CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 37);
	}
	for (int i = 0; i < 8; i++) CHECK(pthread_create(&threads[i], NULL, reader, NULL) == 0);
	for (int i = 0; i < 8; i++) CHECK(pthread_join(threads[i], NULL) == 0);
	/* 8000 consumers perform just one catalog's name resolution. */
	CHECK(atomic_load(&Comparisons) == 5);
	return 0;
}
