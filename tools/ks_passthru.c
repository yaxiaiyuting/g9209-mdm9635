/* ks_passthru.c — /system/bin/ks 的转发器（静态 ARM64，纯 syscall）
 *
 * 原厂 mdm_helper 会执行：
 *   /system/bin/ks -w /cpdump/ -p /dev/ks_hsic_bridge -r 21 -s 21:/firmware/image/sbl1.mbn ...
 * 其中 **-r 21 是 ramdump（收集崩溃转储）模式**，不是启动路径（见原厂 ks 的 usage：
 *   "-r <img_id>  --ramdumpimage  Image ID which must be transferred before forcing
 *                                 Sahara memory dump mode"）。
 * 直接带 -r 跑会走错分支（旧记录里 ks 因此 rc=1、modem 被拉进 dump）。
 *
 * 本转发器：
 *   1) 逐字复制 mdm_helper 传来的 argv，**只丢掉 -r 及其参数**（以及 --ramdumpimage）
 *   2) fork + execve("/system/bin/ks.real", argv', envp)
 *   3) 循环重试若干轮，接住 flashless boot 的多阶段（PBL 阶段 + SBL1 阶段 + EFS 同步）
 *   4) 始终返回 0，避免 mdm_helper 进入 fail state（它会 ESOC_SET_CRASH + SSR 复位 modem）
 */
static long sc4(long n,long a,long b,long c,long d){
    register long x8 __asm__("x8")=n; register long x0 __asm__("x0")=a;
    register long x1 __asm__("x1")=b; register long x2 __asm__("x2")=c; register long x3 __asm__("x3")=d;
    __asm__ volatile("svc #0":"+r"(x0):"r"(x8),"r"(x1),"r"(x2),"r"(x3):"memory");
    return x0;
}
static long slen(const char*s){ long n=0; while(s[n]) n++; return n; }
static int seq(const char*a,const char*b){ while(*a&&*b&&*a==*b){a++;b++;} return *a==*b; }
static void wl(long fd,const char*s){ if(fd>=0&&s) sc4(64,fd,(long)s,slen(s),0); }
static void wn(long fd,long v){ char b[24]; int i=23; b[i]=0; if(v<=0){wl(fd,"0");return;} while(v>0){ b[--i]='0'+(v%10); v/=10;} wl(fd,b+i); }
static void sleep_ms(long ms){ struct { long s; long ns; } t; t.s=ms/1000; t.ns=(ms%1000)*1000000; sc4(101,(long)&t,0,0,0); }

#define MAXA 48
static char *nargv[MAXA+1];
static char *nenvp[] = { "PATH=/system/bin:/system/xbin", "ANDROID_ROOT=/system",
                         "ANDROID_DATA=/data", "LD_LIBRARY_PATH=/system/lib64", 0 };

void real_start(unsigned long *sp){
    long argc=(long)sp[0];
    char **argv=(char**)(sp+1);
    (void)argc;

    long lf=sc4(56,-100,(long)"/data/local/tmp/diag/kspassthru.log",0x0241,0644);

    /* ---- 组装 argv'：丢掉 -r/--ramdumpimage 及其参数 ---- */
    int n=0;
    nargv[n++]="/system/bin/ks.real";
    for(long i=1;i<argc && n<MAXA;i++){
        char *a=argv[i];
        if(seq(a,"-r")||seq(a,"--ramdumpimage")){
            /* 丢掉开关本身；若下一项不是另一个开关，也丢掉它（那是 image id） */
            if(i+1<argc && argv[i+1][0]!='-') i++;
            continue;
        }
        /* 也丢掉 -q/--quitafter（会让 ks 传完指定镜像就退出） */
        if(seq(a,"-q")||seq(a,"--quitafter")){
            if(i+1<argc && argv[i+1][0]!='-') i++;
            continue;
        }
        nargv[n++]=a;
    }
    nargv[n]=0;

    wl(lf,"==== ks_passthru 收到 "); wn(lf,argc-1); wl(lf," 个参数，转发 "); wn(lf,n-1); wl(lf," 个 ====\n");
    wl(lf,"argv' =");
    for(int i=0;i<n;i++){ wl(lf," "); wl(lf,nargv[i]); }
    wl(lf,"\n");

    /* ---- 循环 exec ks.real，接住多阶段 ---- */
    for(int round=0; round<8; round++){
        long pid=sc4(220,17,0,0,0);            /* fork */
        if(pid==0){
            sc4(221,(long)"/system/bin/ks.real",(long)nargv,(long)nenvp,0);  /* execve */
            sc4(93,127,0,0,0);                  /* exec 失败 */
            for(;;);
        }
        if(pid<0){ wl(lf,"fork 失败\n"); break; }

        int st=0;
        sc4(260,pid,(long)&st,0,0);             /* wait4 */
        int code=(st>>8)&0xff, sig=st&0x7f;
        wl(lf,"round "); wn(lf,round); wl(lf,": pid="); wn(lf,pid);
        wl(lf," exit="); wn(lf,code); wl(lf," sig="); wn(lf,sig); wl(lf,"\n");

        /* ks.real 正常跑完（或没有更多请求）就退出循环；
           否则稍等再试——多阶段时它会以各种码返回 */
        sleep_ms(500);
        if(code==0) continue;                   /* 继续接下一阶段 */
        if(round>=5) break;
    }
    wl(lf,"==== ks_passthru 结束，返回 0（避免 mdm_helper 进 fail state）====\n");
    sc4(93,0,0,0,0);
    for(;;);
}
void _start(void){ __asm__ volatile("mov x0, sp\n bl real_start\n"); }
