#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <stdlib.h>
extern void _doprnt(const char *, va_list, int, void (*)(void *, int), void *);
static char out[1024]; static int outn;
static void put(void *a,int c){(void)a; if(outn<1023) out[outn++]=(char)c;}
static void ours(char *d,const char *f,...)
{va_list ap;outn=0;va_start(ap,f);_doprnt(f,ap,0,put,0);va_end(ap);out[outn]=0;strcpy(d,out);}
static unsigned long long st=88172645463325252ULL;
static unsigned long long rnd(void){st^=st<<13;st^=st>>7;st^=st<<17;return st;}
int main(void){
  int fails=0,total=0; char fmt[16],a[1024],b[1024];
  const char *cv="fFeEgG"; const char *fl="-+ #0";
  for(int it=0;it<40000;it++){
    union{double d;unsigned long long u;}x;
    x.u=rnd();
    if(it%3==0){ /* ordinary magnitudes */
      x.u=(x.u&0x800FFFFFFFFFFFFFULL)|((unsigned long long)(900+rnd()%250)<<52);
    }
    int p=0,n=0; fmt[n++]='%';
    for(int k=0;k<2;k++) if(rnd()%3==0) fmt[n++]=fl[rnd()%5];
    if(rnd()%2){int w=rnd()%20; n+=sprintf(fmt+n,"%d",w);}
    if(rnd()%2){p=rnd()%18; n+=sprintf(fmt+n,".%d",p);}
    fmt[n++]=cv[rnd()%6]; fmt[n]=0;
    ours(a,fmt,x.d); snprintf(b,sizeof b,fmt,x.d); total++;
    if(strcmp(a,b)){fails++; if(fails<=8) printf("  %-12s %-24.17g\n     glibc=[%s]\n     ours =[%s]\n",fmt,x.d,b,a);}
  }
  printf("=== %d random doubles, %d differ ===\n",total,fails);
  return 0;}
