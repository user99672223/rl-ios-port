#ifndef RLTEX_INTERNAL_H
#define RLTEX_INTERNAL_H
#include "rltex.h"

/* Lowers the calling thread's priority. Writes a description to text; returns 1 if applied. */
int rltex_platform_lower_priority(rltex_qos qos, char *text, size_t cap);
void rltex_platform_name_thread(const char *name);

#endif
