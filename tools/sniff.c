/* 静态 ARM64：抓 /dev/ks_hsic_bridge 上 modem 发来的原始字节（十六进制+ASCII） */
static long sc4(long n,long a,long b,long c,long d){
    register long x8 __asm__("x8")=n; register long x0 __asm__("x0")=a;
    register long x1 __asm__("x1")=b; register long x2 __asm__("x2")=c; register long x3 __asm__("x3")=d;
    __asm__ volatile("svc #0":"+r"(x0):"r"(x8),"r"(x1),"r"(x2),"r"(x3):"memory");
    return x0;
}
static const char HEX[]="0123456789abcdef";
static long L;
static void out(const char*s,long n){ if(L>=0) sc4(64,L,(long)s,n,0); }
static void outs(const char*s){ long n=0; while(s[n]) n++; out(s,n); }
static void outnum(long v){ char b[16]; int i=15; if(v==0){out("0",1);return;} while(v>0){b[--i]='0'+(v%10);v/=10;} out(b+i,15-i); }
static void dump(const unsigned char*b,long n){
    char line[80];
    for(long i=0;i<n;i+=16){
        int p=0;
        line[p++]=' '; line[p++]=' ';
        for(long j=0;j<16 && i+j<n;j++){ unsigned char c=b[i+j]; line[p++]=HEX[c>>4]; line[p++]=HEX[c&15]; line[p++]=' '; }
        while(p<3+16*3) line[p++]=' ';
        line[p++]='|';
        for(long j=0;j<16 && i+j<n;j++){ unsigned char c=b[i+j]; line[p++]=(c>=32&&c<127)?c:'.'; }
        line[p++]='|'; line[p++]='\n';
        out(line,p);
    }
}
void real_start(unsigned long *sp){
    (void)sp;
    L=sc4(56,-100,(long)"/data/local/tmp/diag/sniff.log",0x0241,0644);
    outs("=== sniffer start ===\n");
    long fd=-1;
    for(int t=0;t<120;t++){
        fd=sc4(56,-100,(long)"/dev/ks_hsic_bridge",2,0);
        if(fd>=0) break;
        for(volatile long k=0;k<40000000;k++);
    }
    if(fd<0){ outs("open failed\n"); sc4(93,1,0,0,0); for(;;); }
    outs("port opened\n");
    static unsigned char buf[8192];
    long total=0;
    for(int i=0;i<600;i++){
        long n=sc4(63,fd,(long)buf,sizeof(buf),0);   /* read */
        if(n>0){ outs("RX "); outnum(n); outs(" bytes:\n"); dump(buf,n); total+=n; }
        else if(n<0){ outs("read err\n"); sc4(35,0,0,0,0); }
        if(total>200000) break;
    }
    outs("=== sniffer exit ===\n");
    sc4(93,0,0,0,0); for(;;);
}
void _start(void){ __asm__ volatile("mov x0, sp\n bl real_start\n"); }
