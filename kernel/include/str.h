#ifndef STR_H
#define STR_H

#include "types.h"

size_t strlen(const char *s);
int    strcmp(const char *a, const char *b);
int    strncmp(const char *a, const char *b, size_t n);
char*  strcpy(char *dst, const char *src);
char*  strchr(const char *s, int c);
char*  strrchr(const char *s, int c);
void*  memcpy(void *dst, const void *src, size_t n);
void*  memset(void *s, int c, unsigned long n);

#endif