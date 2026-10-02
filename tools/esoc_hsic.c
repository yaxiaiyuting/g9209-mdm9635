/* 静态 ARM64：向 /dev/esoc-0 发 ESOC_SET_HSIC_READY (ioctl 0xCC0C)，可重复 N 次 */
static long sc4(long n,long a,long b,long c,long d){
    register long x8 __asm__("x8")=n; register long x0 __asm__("x0")=a;
    register long x1 __asm__("x1")=b; register long x2 __asm__("x2")=c; register long x3 __asm__("x3")=d;
    __asm__ volatile("svc #0":"+r"(x0):"r"(x8),"r"(x1),"r"(x2),"r"(x3):"memory");
    return x0;
}
static void wr(long fd,const char*s){ long n=0; while(s[n]) n++; sc4(64,fd,(long)s,n,0); }
void _start(void){
    const char *log="/data/local/tmp/diag/hsic.log";
    long lf=sc4(56,-100,(long)log,0x0241,0644);
    long fd=sc4(56,-100,(long)"/dev/esoc-0",2,0);
    if(fd<0){ wr(lf,"open /dev/esoc-0 FAILED\n"); sc4(93,1,0,0,0); for(;;); }
    for(int i=0;i<3;i++){
        long r=sc4(29,fd,0xCC0C,0,0);            /* ioctl ESOC_SET_HSIC_READY */
        wr(lf, r<0 ? "SET_HSIC_READY rc<0\n" : "SET_HSIC_READY ok\n");
        sc4(35,0,0,0,0); sc4(35,0,0,0,0);        /* nanosleep 近似：用 syscall 101 更准，简单起见用循环 */
        for(volatile long k=0;k<20000000;k++);
    }
    sc4(93,0,0,0,0); for(;;);
}
