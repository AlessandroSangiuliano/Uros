/*
 * Userspace C implementations for libsa_mach string routines.
 *
 * memcpy, memmove, bcopy are provided by libmach (i386/memcpy.s, SSE2).
 * memset, bzero are provided by i386/memset.s (SSE2).
 */
#include <stddef.h>

char *strcpy(char *dst, const char *src) {
    char *ret = dst;
    while ((*dst++ = *src++));
    return ret;
}

int strcmp(const char *s1, const char *s2) {
    while (*s1 && (*s1 == *s2)) { s1++; s2++; }
    return (unsigned char)*s1 - (unsigned char)*s2;
}

int strncmp(const char *s1, const char *s2, size_t n) {
    while (n && *s1 && (*s1 == *s2)) { s1++; s2++; n--; }
    if (n == 0) return 0;
    return (unsigned char)*s1 - (unsigned char)*s2;
}

char *strncpy(char *dst, const char *src, size_t n) {
    char *ret = dst;
    while (n && (*dst++ = *src++)) n--;
    if (n) while (--n) *dst++ = '\0';
    return ret;
}

char *strncat(char *dst, const char *src, size_t n) {
    char *ret = dst;
    while (*dst) dst++;
    while (n-- && (*dst++ = *src++));
    if (n == (size_t)-1) dst[-1] = '\0';
    return ret;
}

void __main(void) {
    // Stub function, does nothing
}

int memcmp(const void *s1, const void *s2, size_t n) {
    const unsigned char *p1 = s1, *p2 = s2;
    while (n--) {
        if (*p1 != *p2) return *p1 - *p2;
        p1++; p2++;
    }
    return 0;
}

char *strcat(char *dst, const char *src) {
    char *ret = dst;
    while (*dst) dst++;
    while ((*dst++ = *src++));
    return ret;
}

size_t strlen(const char *s) {
    size_t len = 0;
    while (*s++) len++;
    return len;
}
