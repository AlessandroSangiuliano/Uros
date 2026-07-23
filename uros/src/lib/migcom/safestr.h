#ifndef SAFESTR_H
#define SAFESTR_H

#include <stdio.h>

/* Safe snprintf wrapper for legacy code.
 * On truncation it calls fatal() to ensure we catch overflow bugs early.
 */
int SafeSnprintf(char *buf, size_t bufsiz, const char *fmt, ...);

#endif /* SAFESTR_H */
