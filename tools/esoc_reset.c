/* 静态 ARM64：注册 ESOC cmd engine 并对 modem 做一次彻底复位
 *  ioctl: ESOC_REG_CMD_ENG=0xCC08, ESOC_CMD_EXE=0xCC01 (arg=cmd)
 *  cmd:   ESOC_PWR_ON=1, ESOC_PWR_OFF=2, ESOC_RESET=3
 */
static long sc4(long n,long a,long b,long c,long d){
    register long x8 __asm__("x8")=n; register long x0 __asm__("x0")=a;
    register long x1 __asm__("x1")=b; register long x2 __asm__("x2")=c; register long x3 __asm__("x3")=d;
    __asm__ volatile("svc #0":"+r"(x0):"r"(x8),"r"(x1),"r"(x2),"r"(x3):"memory");
    return x0;
}
static long L=-1;
static long slen(const char*s){ long n=0; while(s[n]) n++; return n; }
static void outs(const char*s){ if(L>=0) sc4(64,L,(long)s,slen(s),0); }
static void outn(long v){ char b[20]; int i=19; if(v==0){outs("0");return;} if(v<0){outs("-");v=-v;} while(v>0){b[--i]='0'+(v%10);v/=10;} sc4(64,L,(long)(b+i),19-i,0); }
static void delay(long k){ for(volatile long i=0;i<k;i++); }
void real_start(unsigned long *sp){
    (void)sp;
    L=sc4(56,-100,(long)"/data/local/tmp/diag/esoc_reset.log",0x0241,0644);
    outs("=== esoc reset tool ===\n");
    long fd=sc4(56,-100,(long)"/dev/esoc-0",2,0);
    if(fd<0){ outs("open esoc failed\n"); sc4(93,1,0,0,0); for(;;); }
    long r=sc4(29,fd,0xCC08,0,0);          /* ESOC_REG_CMD_ENG */
    outs("REG_CMD_ENG rc="); outn(r); outs("\n");
    if(r<0){ outs("(mdm_helper 可能仍占着 cmd engine)\n"); }
    unsigned cmd;
    cmd=2; r=sc4(29,fd,0xCC01,(long)&cmd,0); outs("PWR_OFF rc="); outn(r); outs("\n"); delay(200000000);
    cmd=3; r=sc4(29,fd,0xCC01,(long)&cmd,0); outs("RESET  rc="); outn(r); outs("\n"); delay(200000000);
    cmd=1; r=sc4(29,fd,0xCC01,(long)&cmd,0); outs("PWR_ON rc="); outn(r); outs("\n");
    outs("done\n");
    sc4(93,0,0,0,0); for(;;);
}
void _start(void){ __asm__ volatile("mov x0, sp\n bl real_start\n"); }
