/* Per-thread priority for the background ASTC encode worker.
 *
 * Apple (iOS / macOS): <pthread/qos.h>, verified against Apple's open-source libpthread
 *   https://github.com/apple-oss-distributions/libpthread/blob/main/include/pthread/qos.h
 *     int pthread_set_qos_class_self_np(qos_class_t __qos_class, int __relative_priority);
 *   (declared `__API_AVAILABLE(macos(10.10), ios(8.0))`, line 153-156; returns 0 or an errno
 *   value) and the QOS_CLASS_* constants from <sys/qos.h> (QOS_CLASS_BACKGROUND,
 *   QOS_CLASS_UTILITY; libpthread include/sys/qos.h). relative_priority must be <= 0 and
 *   >= QOS_MIN_RELATIVE_PRIORITY; 0 is the class default. Note the header's warning: later calls
 *   such as pthread_setschedparam() on the same thread opt it out of QoS, so none are made.
 *
 *   pthread_setname_np on Apple takes only the name and names the calling thread
 *   (libpthread include/pthread/pthread.h:535: `int pthread_setname_np(const char*);`).
 *
 * Linux (this repo's CI / Debian test host): per-thread nice 19 via setpriority(PRIO_PROCESS,
 *   tid), the documented way to nice a single thread (man 2 setpriority, "NOTES"/NPTL).
 *
 * Anything else: no-op, reported as not applied.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif
#include "rltex_internal.h"

#include <stdio.h>
#include <string.h>

#if defined(__APPLE__)
#include <pthread.h>
#include <pthread/qos.h>
#elif defined(__linux__)
#include <pthread.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

int rltex_platform_lower_priority(rltex_qos qos, char *text, size_t cap)
{
#if defined(__APPLE__)
	qos_class_t cls = (qos == RLTEX_QOS_UTILITY) ? QOS_CLASS_UTILITY : QOS_CLASS_BACKGROUND;
	int rc = pthread_set_qos_class_self_np(cls, 0);
	snprintf(text, cap, "apple qos %s rc=%d", qos == RLTEX_QOS_UTILITY ? "utility" : "background", rc);
	return rc == 0;
#elif defined(__linux__)
	pid_t tid = (pid_t)syscall(SYS_gettid);
	int rc = setpriority(PRIO_PROCESS, (id_t)tid, 19);
	(void)qos;
	snprintf(text, cap, "linux nice 19 tid=%ld rc=%d", (long)tid, rc);
	return rc == 0;
#else
	(void)qos;
	snprintf(text, cap, "no priority control on this platform");
	return 0;
#endif
}

void rltex_platform_name_thread(const char *name)
{
#if defined(__APPLE__)
	pthread_setname_np(name); /* Apple: names the calling thread only */
#elif defined(__linux__)
	pthread_setname_np(pthread_self(), name); /* glibc, max 15 chars */
#else
	(void)name;
#endif
}
