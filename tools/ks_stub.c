static long sc4(long n,long a,long b,long c,long d){
    register long x8 __asm__("x8")=n; register long x0 __asm__("x0")=a;
    register long x1 __asm__("x1")=b; register long x2 __asm__("x2")=c; register long x3 __asm__("x3")=d;
    __asm__ volatile("svc #0":"+r"(x0):"r"(x8),"r"(x1),"r"(x2),"r"(x3):"memory");
    return x0;
}
static long slen(const char*s){ long n=0; while(s[n]) n++; return n; }
void real_start(unsigned long *sp){
    long argc=(long)sp[0]; char **argv=(char**)&sp[1];
    if(argc<0||argc>32) argc=0;
    long fd=sc4(56,-100,(long)"/data/local/tmp/diag/ks_stub.log",0x0241,0644);
    if(fd>=0){
        sc4(64,fd,(long)"ks_stub called argc=",20,0);
        char b[8]; b[0]='0'+(argc%10); b[1]='\n'; sc4(64,fd,(long)b,2,0);
        for(long i=0;i<argc && i<16;i++){ if(argv[i]){ sc4(64,fd,(long)"  ",2,0); sc4(64,fd,(long)argv[i],slen(argv[i]),0); sc4(64,fd,(long)"\n",1,0);} }
        sc4(57,fd,0,0,0);
    }
    sc4(93,0,0,0,0); for(;;);
}
void _start(void){ __asm__ volatile("mov x0, sp\n bl real_start\n"); }
