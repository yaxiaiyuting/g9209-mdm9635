/* mdm_runtime_handshake.c — 复刻原厂 mdm_helper 的「运行时(runtime)」握手
 * 静态 ARM64，纯 syscall，常驻不退出。
 *
 * 依据（本机原厂二进制反汇编 + 同族机型原厂 C 源码 + modem 固件字符串，三方互证）：
 *   原厂 configure_flashless_boot_dev(dev, MODE_RUNTIME=2) 在「modem 拉高 MDM2AP_STATUS」
 *   之后执行：
 *     1) 写 /sys/bus/platform/drivers/s5p-ehci/unbind  ("15510000.usb")
 *     2) usleep(10000)
 *     3) 写 /sys/bus/platform/drivers/s5p-ehci/bind    ("15510000.usb")
 *        → 让 EHCI 根端口重新枚举，暴露出 AMSS 阶段的复合设备
 *          （05c6:9048 / 904C / 9075 …，见内核 ks_bridge.c:576-585 的 ksb_usb_ids）
 *     4) ioctl(/dev/esoc-0, ESOC_SET_HSIC_READY = 0xCC0C)
 *        → 产生 AP2MDM_HSIC_READY 的**上升沿**
 *          （内核刻意在 ESOC_PWR_ON 里把它清 0，见 esoc-mdm-4x.c:388）
 *     5) 等 /dev/efs_hsic_bridge 出现（EFS/NV 在 AP 侧 m9kefs1/2/3，modem 无 flash）
 *     6) ioctl(/dev/esoc-0, ESOC_NOTIFY, ESOC_BOOT_DONE = 2)
 *     7) ioctl(/dev/esoc-0, ESOC_NOTIFY, ESOC_DIAG_DISABLE = 12)
 *
 * 关键顺序：**不能**在 Sahara 之前拉高 HSIC_READY（那样 modem 的
 * SS_Hsic_host_ready_Raising_Signal_Isr 收不到边沿），必须等 STATUS 之后再拉。
 */
#define ESOC_NOTIFY         0x4004CC03UL
#define ESOC_SET_HSIC_READY 0x0000CC0CUL

#define ESOC_IMG_XFER_DONE 1
#define ESOC_BOOT_DONE     2
#define ESOC_DIAG_DISABLE  12

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
static void msg(const char*s){ ts(); outs(s); outs("\n"); }
static void msgn(const char*a,long v){ ts(); outs(a); outn(v); outs("\n"); }
static void sleep_ms(long ms){ struct { long s; long ns; } t; t.s=ms/1000; t.ns=(ms%1000)*1000000; sc4(101,(long)&t,0,0,0); }
static void usleep10(void){ struct { long s; long ns; } t; t.s=0; t.ns=10000000; sc4(101,(long)&t,0,0,0); }

/* 打开 /sys/.../unbind|bind 并写入 devname */
static long sysfs_write(const char*path,const char*val){
    long fd=sc4(56,-100,(long)path,1,0);      /* O_WRONLY */
    if(fd<0) return fd;
    long w=sc4(64,fd,(long)val,slen(val),0);
    sc4(57,fd,0,0,0);
    return w;
}
static int exists(const char*p){
    long fd=sc4(56,-100,(long)p,0,0);
    if(fd<0) return 0;
    sc4(57,fd,0,0,0);
    return 1;
}
/* 读 /sys/kernel/debug/gpio，返回 MDM2AP_STATUS 是否为 hi */
static int status_is_hi(long gfd){
    if(gfd<0) return 0;
    char buf[8192];
    sc4(62,gfd,0,0,0);
    long n=sc4(63,gfd,(long)buf,sizeof(buf)-1,0);
    if(n<=0) return 0;
    buf[n]=0;
    for(long i=0;i<n;i++){
        if(buf[i]!='M'||i+13>=n) continue;
        if(buf[i+1]!='D'||buf[i+2]!='M'||buf[i+3]!='2'||buf[i+4]!='A'||
           buf[i+5]!='P'||buf[i+6]!='_'||buf[i+7]!='S'||buf[i+8]!='T'||
           buf[i+9]!='A'||buf[i+10]!='T'||buf[i+11]!='U'||buf[i+12]!='S') continue;
        long j=i; while(j<n && buf[j]!='\n') j++;
        for(long k=i;k+1<j;k++)
            if(buf[k]=='h'&&buf[k+1]=='i'&&(k==0||buf[k-1]==' '||buf[k-1]=='\t')) return 1;
        return 0;
    }
    return 0;
}

void real_start(unsigned long *sp){
    (void)sp;
    L=sc4(56,-100,(long)"/data/local/tmp/diag/runtime.log",0x0241,0644);
    msg("=== mdm runtime handshake start ===");

    long efd = sc4(56,-100,(long)"/dev/esoc-0",0x802,0);
    if(efd<0){ msgn("open /dev/esoc-0 failed ",efd); sc4(93,1,0,0,0); for(;;); }
    msgn("esoc fd=",efd);

    long gfd = sc4(56,-100,(long)"/sys/kernel/debug/gpio",0,0);
    if(gfd<0) msg("WARN: no /sys/kernel/debug/gpio");

    /* ---- 1) 等 modem 拉高 MDM2AP_STATUS ---- */
    msg("waiting for MDM2AP_STATUS=hi ...");
    int hi=0;
    for(long t=0;t<2400;t++){          /* 最多 600s（500ms 一次） */
        if(status_is_hi(gfd)){ hi=1; break; }
        sleep_ms(500);
    }
    if(!hi){ msg("TIMEOUT: STATUS never high; abort"); for(;;) sleep_ms(10000); }
    msg("★ MDM2AP_STATUS is now high");

    /* ---- 2) EHCI unbind → 10ms → bind （让 AMSS 阶段设备重新枚举）---- */
    msg("EHCI unbind (15510000.usb)");
    msgn("  unbind write -> ", sysfs_write("/sys/bus/platform/drivers/s5p-ehci/unbind","15510000.usb"));
    usleep10();
    msg("EHCI bind");
    msgn("  bind   write -> ", sysfs_write("/sys/bus/platform/drivers/s5p-ehci/bind","15510000.usb"));
    sleep_ms(300);

    /* ---- 3) 首次拉高 AP2MDM_HSIC_READY（产生上升沿）---- */
    msgn("ESOC_SET_HSIC_READY -> ", sc4(29,efd,ESOC_SET_HSIC_READY,0,0));

    /* ---- 4) 等 /dev/efs_hsic_bridge ---- */
    msg("waiting for /dev/efs_hsic_bridge ...");
    int got=0;
    for(long t=0;t<150;t++){           /* 15s */
        if(exists("/dev/efs_hsic_bridge")){ got=1; break; }
        sleep_ms(100);
    }
    if(got) msg("★ /dev/efs_hsic_bridge appeared");
    else    msg("!! /dev/efs_hsic_bridge did NOT appear (继续发 BOOT_DONE)");

    /* ---- 5) ESOC_BOOT_DONE ---- */
    {
        long bd=ESOC_BOOT_DONE;
        msgn("ESOC_NOTIFY(ESOC_BOOT_DONE) -> ", sc4(29,efd,ESOC_NOTIFY,(long)&bd,0));
    }
    /* ---- 6) ESOC_DIAG_DISABLE ---- */
    {
        long dd=ESOC_DIAG_DISABLE;
        msgn("ESOC_NOTIFY(ESOC_DIAG_DISABLE) -> ", sc4(29,efd,ESOC_NOTIFY,(long)&dd,0));
    }

    /* ---- 7) 常驻观察，记录 STATUS 是否保持 ---- */
    msg("runtime handshake done; monitoring (resident)");
    int last=-1; long held=0;
    for(long t=0;t<7200;t++){          /* 最长 1 小时 */
        int h=status_is_hi(gfd);
        if(h!=last){
            ts(); outs(h?"STATUS -> hi\n":"STATUS -> lo\n");
            last=h;
        }
        if(h) held++;
        sleep_ms(500);
    }
    msg("monitor window ended");
    for(;;) sleep_ms(10000);
}
void _start(void){ __asm__ volatile("mov x0, sp\n bl real_start\n"); }
