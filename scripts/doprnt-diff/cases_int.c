#include <stdio.h>
#include <string.h>
#include <stdarg.h>
extern void _doprnt(const char *, va_list, int, void (*)(void *, int), void *);
static char out[512]; static int outn;
static void put(void *a, int c) { (void)a; if (outn < 511) out[outn++] = (char)c; }
static int ours(char *dst, const char *fmt, ...)
{ va_list ap; outn = 0; va_start(ap, fmt); _doprnt(fmt, ap, 0, put, 0); va_end(ap);
  out[outn] = 0; strcpy(dst, out); return outn; }
static int fails, total;
#define CHK(fmt, ...) do { char a[512], b[512]; \
    ours(a, fmt, __VA_ARGS__); snprintf(b, sizeof b, fmt, __VA_ARGS__); total++; \
    if (strcmp(a,b)) { fails++; if (fails<=12) printf("  DIFFER %-14s glibc=[%s] ours=[%s]\n", fmt, b, a); } } while (0)
int main(void) {
  long long vs[] = {0,1,7,9,10,15,16,99,100,255,256,1000,65535,65536,
                    2147483647LL,-2147483648LL,4294967295LL,4294967296LL,
                    -1,-7,-1000,1234567890123LL,-1234567890123LL,
                    9223372036854775807LL};
  const char *ifmt[] = {"%d","%5d","%-5d","%05d","%+d","% d","%.3d","%8.3d","%-8.3d","%ld","%lld"};
  const char *ufmt[] = {"%u","%x","%X","%o","%#x","%#X","%#o","%8x","%-8x","%08x","%.5x","%lu","%lx","%llx","%zu"};
  for (unsigned i=0;i<sizeof vs/sizeof*vs;i++) {
    for (unsigned f=0;f<sizeof ifmt/sizeof*ifmt;f++) {
      const char *F=ifmt[f];
      if (strstr(F,"ll")) CHK(F,(long long)vs[i]);
      else if (strchr(F,'l')) CHK(F,(long)vs[i]);
      else CHK(F,(int)vs[i]);
    }
    for (unsigned f=0;f<sizeof ufmt/sizeof*ufmt;f++) {
      const char *F=ufmt[f];
      if (strstr(F,"ll")) CHK(F,(unsigned long long)vs[i]);
      else if (strstr(F,"z")) CHK(F,(size_t)vs[i]);
      else if (strchr(F,'l')) CHK(F,(unsigned long)vs[i]);
      else CHK(F,(unsigned)vs[i]);
    }
  }
  const char *ss[]={"","a","hello","a rather longer string"};
  const char *sf[]={"%s","%10s","%-10s","%.2s","%10.3s","%-10.3s"};
  for (unsigned i=0;i<4;i++) for (unsigned f=0;f<6;f++) CHK(sf[f],ss[i]);
  CHK("%c",'x'); CHK("%5c",'y'); CHK("%-5c",'z');
  CHK("a%%b%d",7); CHK("%d %s %x",1,"two",3);
  printf("=== %d cases, %d differ ===\n", total, fails);
  return 0;
}
