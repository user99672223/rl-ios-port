/* Type-checks the __APPLE__ branch of platform.c against declaration-only stubs of Apple's QoS
 * API (see apple_stubs/pthread/qos.h). Apple's pthread_setname_np takes only the name
 * (libpthread include/pthread/pthread.h:535), glibc's takes a thread too, so it is renamed here.
 * Syntax/type check only; never linked or run. The real iOS build is exercised by CI. */
#include <pthread.h>
#define pthread_setname_np apple_pthread_setname_np
int apple_pthread_setname_np(const char *name);
#include "../../runtime/textures/src/platform.c"
