#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <math.h>
extern void _doprnt(const char *, va_list, int, void (*)(void *, int), void *);
static char out[512]; static int outn;
static void put(void *a, int c) { (void)a; if (outn<511) out[outn++]=(char)c; }
static void ours(char *d, const char *f, ...)
{ va_list ap; outn=0; va_start(ap,f); _doprnt(f,ap,0,put,0); va_end(ap); out[outn]=0; strcpy(d,out); }
static int fails,total;
#define CHK(f,v) do { char a[512],b[512]; ours(a,f,(double)(v)); \
  snprintf(b,sizeof b,f,(double)(v)); total++; \
  if (strcmp(a,b)) { fails++; if (fails<=14) printf("  %-10s v=%-24.17g glibc=[%s] ours=[%s]\n",f,(double)(v),b,a); } } while(0)
int main(void){
  double vs[]={0.0,-0.0,1.0,-1.0,0.5,2.5,3.14159265358979,1e-5,1e-4,1.5e-4,
               123456.789,0.000123456,1e10,1e20,1e-20,9.999999,99999.99999,
               1.0/3.0,2.0/3.0,1e308,1e-308,255.5,0.1,0.25,1024.0,-273.15,
               INFINITY,-INFINITY,NAN};
  const char *fs[]={"%f","%.0f","%.1f","%.3f","%.10f","%12.3f","%-12.3f","%012.3f","%+.2f","%#.0f",
                    "%e","%.0e","%.2e","%.9e","%14.3e","%-14.3e","%E",
                    "%g","%.1g","%.3g","%.10g","%G","%#g","%12.4g","%-12.4g"};
  for (unsigned i=0;i<sizeof vs/sizeof*vs;i++)
    for (unsigned f=0;f<sizeof fs/sizeof*fs;f++) CHK(fs[f],vs[i]);
  printf("=== %d float cases, %d differ ===\n",total,fails);
  return 0; }
