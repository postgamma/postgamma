/*
 * Copyright 2026 Shujie Zhang
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef POSTGAMMA_SESSION_LOCALE_H
#define POSTGAMMA_SESSION_LOCALE_H

#include <locale.h>
#include <stdbool.h>

typedef enum PostgammaLocaleCategory
{
	POSTGAMMA_LOCALE_COLLATE = 0,
	POSTGAMMA_LOCALE_CTYPE,
	POSTGAMMA_LOCALE_MESSAGES,
	POSTGAMMA_LOCALE_MONETARY,
	POSTGAMMA_LOCALE_NUMERIC,
	POSTGAMMA_LOCALE_TIME,
	POSTGAMMA_LOCALE_CATEGORY_COUNT
} PostgammaLocaleCategory;

#define POSTGAMMA_SESSION_LOCALE_NAME_CAPACITY 128

/* Owned by the logical session; only its bound carrier may access this state. */
typedef struct PostgammaSessionLocale
{
    locale_t session_locale;
    char locale_names[POSTGAMMA_LOCALE_CATEGORY_COUNT][POSTGAMMA_SESSION_LOCALE_NAME_CAPACITY];
    bool locale_names_initialized;
} PostgammaSessionLocale;

char *postgamma_session_locale_set(PostgammaSessionLocale *state, int category, const char *locale);
int postgamma_session_locale_bind(PostgammaSessionLocale *state, locale_t *previous);
int postgamma_session_locale_unbind(locale_t *previous);
/* Destroy only after unbinding from the carrier. */
void postgamma_session_locale_destroy(PostgammaSessionLocale *state);

#endif
