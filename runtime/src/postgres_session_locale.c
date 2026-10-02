/*
 * Copyright 2026 Shujie Zhang
 * SPDX-License-Identifier: Apache-2.0
 */

/* Keep PostgreSQL's configured feature boundary outside the portable module.
 * The adapter installs session_locale.c alongside this wrapper as an include.
 */
#include "pg_config.h"

#if !defined(WIN32) && defined(HAVE_USELOCALE)
#include "postgamma_session_locale_impl.inc"
#else
/* Keep the disabled translation unit valid under strict C compilation. */
typedef int PostgammaSessionLocaleUnavailable;
#endif
