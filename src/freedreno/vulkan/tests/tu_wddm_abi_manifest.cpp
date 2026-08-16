/*
 * Copyright 2026 DroidVM contributors
 * SPDX-License-Identifier: MIT
 */

#include "../tu_wddm_abi.h"

#include <stddef.h>
#include <stdio.h>

#if defined(_MSC_VER)
#include <fcntl.h>
#include <io.h>
#endif

static void
emit(const char *name, unsigned long long value)
{
   printf("%s=%llu\n", name, value);
}

int
main()
{
#if defined(_MSC_VER)
   _setmode(_fileno(stdout), _O_BINARY);
#endif

#define ABI_VALUE(label, expression, expected)                                      \
   static_assert((unsigned long long)(expression) ==                                \
                    (unsigned long long)(expected),                                 \
                 #label " value");                                                 \
   emit("value." #label, (unsigned long long)(expression))
#define ABI_SIZE(label, type, expected) ABI_VALUE(label, sizeof(type), expected)
#define ABI_OFFSET(label, type, field, expected)                                    \
   ABI_VALUE(label, offsetof(type, field), expected)

#include "tu_wddm_abi_manifest_entries.h"

#undef ABI_OFFSET
#undef ABI_SIZE
#undef ABI_VALUE
   return 0;
}
