/*
 * Copyright 1991-1998 by Open Software Foundation, Inc.
 *              All Rights Reserved
 *
 * Kernel-level string.h for freestanding environment.
 * Provides declarations for string/memory functions implemented
 * by the kernel itself.
 */

#ifndef _KERNEL_STRING_H_
#define _KERNEL_STRING_H_

#include <mach/machine/vm_types.h>

/*
 * Standard string functions - implemented in kern/string.c or architecture code
 */

/* Memory copy */
extern void *memcpy(void *dst, const void *src, vm_size_t n);
extern void *memmove(void *dst, const void *src, vm_size_t n);

/* Memory set */
extern void *memset(void *s, int c, vm_size_t n);

/* Memory compare */
extern int memcmp(const void *s1, const void *s2, vm_size_t n);

/* String length */
extern vm_size_t strlen(const char *s);

/* String copy */
extern char *strcpy(char *dst, const char *src);
extern char *strncpy(char *dst, const char *src, vm_size_t n);

/* String compare */
extern int strcmp(const char *s1, const char *s2);
extern int strncmp(const char *s1, const char *s2, vm_size_t n);

/* String concatenate */
extern char *strcat(char *dst, const char *src);
extern char *strncat(char *dst, const char *src, vm_size_t n);

/* String search */
extern char *strchr(const char *s, int c);
extern char *strrchr(const char *s, int c);
extern char *strstr(const char *haystack, const char *needle);

/*
 * BSD-style functions (used heavily in Mach kernel)
 * These are typically defined in kern/misc_protos.h but we
 * declare them here for completeness
 */
extern void bcopy(const char *from, char *to, vm_size_t nbytes);
extern void bzero(char *from, vm_size_t nbytes);
extern int bcmp(const char *a, const char *b, vm_size_t len);

/*
 * Size type for compatibility
 */
#ifndef _SIZE_T
#define _SIZE_T
typedef vm_size_t size_t;
#endif

/*
 * NULL pointer constant
 */
#ifndef NULL
#define NULL ((void *)0)
#endif

#endif /* _KERNEL_STRING_H_ */
