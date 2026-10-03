/* esoc_pwron_hold.c — 上电 modem 并**常驻不退出**
 *
 * 为什么必须常驻（实测 dmesg，2026-10-03）：
 *   powerup 工具发完 ioctl(SUBSYS_POWERUP) 后阻塞等 boot_done；一旦 modem 起来、
 *   boot_done 完成，powerup 就 exit() → 释放 /dev/subsys_esoc0 的 fd →
 *   内核 arch/arm64/mach-exynos/subsystem_restart.c:909 的 subsys_device_close()
 *   → subsystem_put() → 引用计数归零 → mdm_subsys_shutdown() → ESOC_PWR_OFF
 *   → AP2MDM_STATUS=0 + 硬复位，**把我们刚启动的 modem 打掉**。
 *
 *   证据：
 *     [66.448] status = 1: mdm is now ready
 *     [66.754] subsys-restart: subsys_device_close(): subsys_device_close   ← powerup 退出
 *     [66.754] ext-mdm: Graceful shutdown fail, ret = -19
 *     [66.754] ext-mdm: Doing a hard reset                                  ← modem 被关掉
 *
 * 因此本工具发完 ioctl 后**永久常驻**（持有 fd），让内核保持 modem 上电。
 * 需要关机时再 kill 它（或单独用 poweroff 工具）。
 */
static long sc4(long n,long a,long b,long c,long d){
    register long x8 __asm__("x8")=n; register long x0 __asm__("x0")=a;
    register long x1 __asm__("x1")=b; register long x2 __asm__("x2")=c; register long x3 __asm__("x3")=d;
    __asm__ volatile("svc #0":"+r"(x0):"r"(x8),"r"(x1),"r"(x2),"r"(x3):"memory");
    return x0;
}
static long slen(const char*s){ long n=0; while(s[n]) n++; return n; }
static long L=-1;
static void outs(const char*s){ if(L>=0&&s) sc4(64,L,(long)s,slen(s),0); }
static void outn(long v){ char b[24]; int i=23; if(v==0){outs("0");return;} if(v<0){outs("-");v=-v;} while(v>0){b[--i]='0'+(v%10);v/=10;} sc4(64,L,(long)(b+i),23-i,0); }
static void ts(void){ struct { long s; long ns; } t; sc4(113,1,(long)&t,0,0);
    outs("["); outn(t.s); outs("."); long ms=t.ns/1000000; if(ms<100) outs("0"); if(ms<10) outs("0"); outn(ms); outs("] "); }

void real_start(unsigned long *sp){
    (void)sp;
    L=sc4(56,-100,(long)"/data/local/tmp/diag/pwron.log",0x0241,0644);
    ts(); outs("=== esoc_pwron_hold start ===\n");

    long fd = sc4(56,-100,(long)"/dev/subsys_esoc0",2,0);   /* O_RDWR */
    if(fd<0){ ts(); outs("open failed "); outn(fd); outs("\n"); sc4(93,1,0,0,0); for(;;); }
    ts(); outs("fd="); outn(fd); outs(" -> ioctl(SUBSYS_POWERUP)\n");

    long r = sc4(29,fd,0xCD01,0,0);                          /* SUBSYS_POWERUP */
    ts(); outs("ioctl returned "); outn(r); outs(" (阻塞期间 modem 正在启动)\n");

    /* ★ 关键：绝不退出，绝不关 fd —— 否则内核会 PWR_OFF 把 modem 打掉 */
    ts(); outs("HOLDING fd open forever (modem stays powered)\n");
    for(;;){
        sc4(101,(long)(long[]){60,0},0,0,0);   /* sleep 60s */
    }
}
void _start(void){ __asm__ volatile("mov x0, sp\n bl real_start\n"); }
