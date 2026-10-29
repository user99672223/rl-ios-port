#ifndef CHECK_H
#define CHECK_H
#include <stdio.h>

extern int g_failures;

#define CHECK(c) do { if (!(c)) { printf( "  FAIL %s:%d: %s\n", __FILE__, __LINE__, #c ); g_failures++; } } while (0)
#define CHECK_EQ(a, b) do { unsigned long long _a = (unsigned long long)(a), _b = (unsigned long long)(b); \
    if (_a != _b) { printf( "  FAIL %s:%d: %s == %s (got 0x%llx, want 0x%llx)\n", __FILE__, __LINE__, #a, #b, _a, _b ); \
                    g_failures++; } } while (0)
#endif
