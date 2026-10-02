/* 静态 ARM64 ks 转发器：
 * 1) 丢掉 mdm_helper 的 -r（那是 ramdump 模式），改走真正的映像传输/启动路径
 * 2) fork+exec 原厂 ks.real，记录退出码与输出
 * 3) 循环重试，接住 SBL1→AMSS 第二阶段可能的新一轮请求
 * 4) 最终返回 0，避免 mdm_helper 进入 fail state 退出
 */
static long sc4(long n,long a,long b,long c,long d){
    register long x8 __asm__("x8")=n; register long x0 __asm__("x0")=a;
    register long x1 __asm__("x1")=b; register long x2 __asm__("x2")=c; register long x3 __asm__("x3")=d;
    __asm__ volatile("svc #0":"+r"(x0):"r"(x8),"r"(x1),"r"(x2),"r"(x3):"memory");
    return x0;
}
static long sc6(long n,long a,long b,long c,long d,long e,long f){
    register long x8 __asm__("x8")=n; register long x0 __asm__("x0")=a;
    register long x1 __asm__("x1")=b; register long x2 __asm__("x2")=c; register long x3 __asm__("x3")=d;
    register long x4 __asm__("x4")=e; register long x5 __asm__("x5")=f;
    __asm__ volatile("svc #0":"+r"(x0):"r"(x8),"r"(x1),"r"(x2),"r"(x3),"r"(x4),"r"(x5):"memory");
    return x0;
}
static long slen(const char*s){ long n=0; while(s[n]) n++; return n; }
static void wl(long fd,const char*s){ if(fd>=0&&s) sc4(64,fd,(long)s,slen(s),0); }
static void wn(long fd,long v){ char b[24]; int i=23; b[i]=0; if(v<=0){wl(fd,"0");return;} while(v>0){ b[--i]='0'+(v%10); v/=10;} wl(fd,b+i); }
static void ms(long m){ /* nanosleep 近似用忙等，够用 */ for(volatile long k=0;k<m*20000;k++); }

void real_start(unsigned long *sp){
    (void)sp;
    long lf=sc4(56,-100,(long)"/data/local/tmp/diag/ksfwd.log",0x0241,0644);
    static char *nargv[] = {
        "/system/bin/ks.real",
        "-w", "/cpdump/",
        "-p", "/dev/ks_hsic_bridge",
        "-j", "4",
        "-s", "21:/firmware/image/sbl1.mbn",
        "-s", "25:/firmware/image/tz.mbn",
        "-s", "30:/firmware/image/sdi.mbn",
        "-s", "23:/firmware/image/rpm.mbn",
        "-s", "31:/firmware/image/mba.mbn",
        "-s", "8:/firmware/image/qdsp6sw.mbn",
        "-s", "28:/firmware/image/dsp2.mbn",
        "-s", "6:/firmware/image/apps.mbn",
        "-s", "16:/dev/block/modem/m9kefs1",
        "-s", "17:/dev/block/modem/m9kefs2",
        "-s", "20:/dev/block/modem/m9kefs3",
        "-s", "29:/firmware/image/acdb.mbn",
        0
    };
    static char *nenvp[] = { "PATH=/system/bin:/system/xbin", "ANDROID_ROOT=/system", "ANDROID_DATA=/data", 0 };
    wl(lf,"==== ksfwd(transfer mode, no -r) ====\n");
    for (int round=0; round<6; round++) {
        long pid=sc4(220,17,0,0,0);              /* clone(SIGCHLD) == fork */
        if (pid==0) {
            long of=sc4(56,-100,(long)"/data/local/tmp/diag/ks_child.log",0x0241,0644);
            if(of>=0){ sc4(24,of,1,0,0); sc4(24,of,2,0,0); }   /* dup3 -> stdout/stderr */
            sc6(221,(long)"/system/bin/ks.real",(long)nargv,(long)nenvp,0,0,0);
            sc4(93,127,0,0,0); for(;;);
        }
        wl(lf,"round "); wn(lf,round); wl(lf,": child pid="); wn(lf,pid); wl(lf,"\n");
        long status=0;
        sc4(260,pid,(long)&status,0,0);          /* wait4 */
        wl(lf,"  ks exit status="); wn(lf,(status>>8)&0xff);
        wl(lf," signal="); wn(lf,status&0x7f); wl(lf,"\n");
        ms(1500);
    }
    wl(lf,"ksfwd done, returning 0\n");
    sc4(93,0,0,0,0); for(;;);
}
void _start(void){ __asm__ volatile("mov x0, sp\n bl real_start\n"); }
