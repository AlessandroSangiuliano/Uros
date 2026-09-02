#include <stdio.h>
#include <string.h>
#include <stdarg.h>
extern void _doprnt(const char *, va_list, int, void (*)(void *, int), void *);
static char out[1024]; static int outn;
static void put(void *a,int c){(void)a; if(outn<1023) out[outn++]=(char)c;}
static void ours(char *d,const char *f,...)
{va_list ap;outn=0;va_start(ap,f);_doprnt(f,ap,0,put,0);va_end(ap);out[outn]=0;strcpy(d,out);}
static unsigned long long st=1234567891234567ULL;
static unsigned long long rnd(void){st^=st<<13;st^=st>>7;st^=st<<17;return st;}
int main(void){
  int fails=0,total=0; char fmt[24],a[1024],b[1024];
  const char *cv="diuoxX"; const char *fl="-+ #0";
  for(int it=0;it<40000;it++){
    unsigned long long v=rnd(); int n=0; fmt[n++]='%';
    for(int k=0;k<2;k++) if(rnd()%3==0) fmt[n++]=fl[rnd()%5];
    if(rnd()%2) n+=sprintf(fmt+n,"%d",(int)(rnd()%25));
    if(rnd()%3==0) n+=sprintf(fmt+n,".%d",(int)(rnd()%20));
    int len=rnd()%3; if(len==1) fmt[n++]='l'; else if(len==2){fmt[n++]='l';fmt[n++]='l';}
    char c=cv[rnd()%6]; fmt[n++]=c; fmt[n]=0;
    if(len==2){ours(a,fmt,(unsigned long long)v);snprintf(b,sizeof b,fmt,(unsigned long long)v);}
    else if(len==1){ours(a,fmt,(unsigned long)v);snprintf(b,sizeof b,fmt,(unsigned long)v);}
    else {ours(a,fmt,(unsigned)v);snprintf(b,sizeof b,fmt,(unsigned)v);}
    total++;
    if(strcmp(a,b)){fails++; if(fails<=6) printf("  %-14s glibc=[%s] ours=[%s]\n",fmt,b,a);}
  }
  printf("=== %d random integers, %d differ ===\n",total,fails); return 0;}
