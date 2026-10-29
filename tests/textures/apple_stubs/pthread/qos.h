/* Declaration-only stand-in for Apple's <pthread/qos.h> and <sys/qos.h>, used ONLY by
 * apple_syntax_check.sh to type-check the __APPLE__ branch of runtime/textures/src/platform.c on
 * a Linux host. Signatures and values are copied from Apple's open-source libpthread:
 *   include/pthread/qos.h:155   int pthread_set_qos_class_self_np(qos_class_t, int);
 *   include/sys/qos.h:130-141   QOS_CLASS_* values;  :153  QOS_MIN_RELATIVE_PRIORITY (-15)
 * (https://github.com/apple-oss-distributions/libpthread). It is not used for any real build. */
#ifndef RLTEX_APPLE_STUB_QOS_H
#define RLTEX_APPLE_STUB_QOS_H
typedef enum {
	QOS_CLASS_USER_INTERACTIVE = 0x21,
	QOS_CLASS_USER_INITIATED = 0x19,
	QOS_CLASS_DEFAULT = 0x15,
	QOS_CLASS_UTILITY = 0x11,
	QOS_CLASS_BACKGROUND = 0x09,
	QOS_CLASS_UNSPECIFIED = 0x00
} qos_class_t;
#define QOS_MIN_RELATIVE_PRIORITY (-15)
int pthread_set_qos_class_self_np(qos_class_t qos_class, int relative_priority);
#endif
