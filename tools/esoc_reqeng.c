/* esoc_reqeng.c — 常驻 ESOC 引擎守护（静态 ARM64，纯 syscall）
 *
 * 背景（全部由 dmesg 实测得出）：
 *   1) modem 上电后，内核 ext-mdm 把"请求"投进 ESOC req fifo，
 *      **只有注册为 request engine 的进程**能取走（ESOC_WAIT_FOR_REQ）。
 *      无人持有 → PBL 收不到 "Signaling request engine for images" → 不发 Sahara HELLO。
 *   2) modem 跑起来后拉高 MDM2AP_STATUS，驱动经 evt fifo 投递 ESOC_RUN_STATE，
 *      esoc-mdm-drv 的 mdm_handle_clink_evt 收到后才 complete(&boot_done)，
 *      mdm_subsys_powerup 才能返回、modem 才算"启动成功"。
 *      ★ evt fifo 只有 4 格（ESOC_MAX_EVT=4）且 **无人读** → 塞满后
 *        "unable to queue event for MDM9x35" → boot_done 永不完成 →
 *        modem 在 STATUS 拉高约 11s 后被判 ESOC_UNEXPECTED_RESET 复位。
 *        这正是"12 个镜像全 status=0、modem 却起不来"的真正原因。
 *
 * 本程序两件事一起做：
 *   - 父进程：注册 request engine，循环 ESOC_WAIT_FOR_REQ 取请求并应答；
 *   - 子进程：fork 后循环 ESOC_WAIT_FOR_CRASH 把 evt fifo 抽干（该 ioctl 不要求持有引擎）。
 *   两者都必须常驻，绝不退出。
 */
#define ESOC_WAIT_FOR_REQ   0x8004CC02UL
#define ESOC_NOTIFY         0x4004CC03UL
#define ESOC_GET_STATUS     0x8004CC04UL
#define ESOC_WAIT_FOR_CRASH 0x8004CC06UL
#define ESOC_REG_REQ_ENG    0x0000CC07UL

#define ESOC_REQ_IMG      1
#define ESOC_REQ_DEBUG    2
#define ESOC_REQ_SHUTDOWN 3

#define ESOC_IMG_XFER_DONE 1
#define ESOC_BOOT_DONE     2

/* esoc_evt 名称，便于读日志 */
static const char* evt_name(long e){
    switch(e){
    case 0x1: return "ESOC_RUN_STATE";
    case 0x2: return "ESOC_UNEXPECTED_RESET";
    case 0x3: return "ESOC_ERR_FATAL";
    case 0x4: return "ESOC_IN_DEBUG";
    case 0x5: return "ESOC_REQ_ENG_ON";
    case 0x6: return "ESOC_REQ_ENG_OFF";
    case 0x7: return "ESOC_CMD_ENG_ON";
    case 0x8: return "ESOC_CMD_ENG_OFF";
    case 0x9: return "ESOC_INVALID_STATE";
    default:  return "?";
    }
}

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

/* ---------- 子进程②：等 MDM2AP_STATUS=hi 后发 ESOC_BOOT_DONE ---------- */
static void boot_done_watcher(void){
    long efd = sc4(56,-100,(long)"/dev/esoc-0",0x802,0);
    if(efd<0){ msg("[boot] open /dev/esoc-0 failed"); sc4(93,0,0,0,0); for(;;); }
    msg("[boot] watcher started: 等 MDM2AP_STATUS 拉高后发 ESOC_BOOT_DONE");
    long gfd = sc4(56,-100,(long)"/sys/kernel/debug/gpio",0,0);
    if(gfd<0) msg("[boot] WARN: cannot open /sys/kernel/debug/gpio");
    for(long t=0; t<2400; t++){          /* 最多 ~1200s */
        int hi=0;
        if(gfd>=0){
            char buf[8192];
            sc4(62,gfd,0,0,0);
            long n=sc4(63,gfd,(long)buf,sizeof(buf)-1,0);
            if(n>0){
                buf[n]=0;
                for(long i=0;i<n;i++){
                    if(buf[i]!='M'||i+13>=n) continue;
                    if(buf[i+1]!='D'||buf[i+2]!='M'||buf[i+3]!='2'||buf[i+4]!='A'||
                       buf[i+5]!='P'||buf[i+6]!='_'||buf[i+7]!='S'||buf[i+8]!='T'||
                       buf[i+9]!='A'||buf[i+10]!='T'||buf[i+11]!='U'||buf[i+12]!='S') continue;
                    long j=i; while(j<n && buf[j]!='\n') j++;
                    for(long k=i;k+1<j;k++){
                        if(buf[k]=='h'&&buf[k+1]=='i'&&(k==0||buf[k-1]==' '||buf[k-1]=='\t')){
                            hi=1; break;
                        }
                    }
                    break;
                }
            }
        }
        if(hi){
            msg("[boot] ★ MDM2AP_STATUS is now high -> ESOC_BOOT_DONE");
            long bd = ESOC_BOOT_DONE;
            msgn("[boot] ESOC_NOTIFY(ESOC_BOOT_DONE) -> ", sc4(29,efd,ESOC_NOTIFY,(long)&bd,0));
            /* 只发一次；之后继续监视，若又掉低再拉起（重试一次） */
            for(long t2=0;t2<600;t2++){      /* 再观察 300s */
                sleep_ms(500);
                int still=0; char b2[8192];
                sc4(62,gfd,0,0,0);
                long n2=sc4(63,gfd,(long)b2,sizeof(b2)-1,0);
                if(n2>0){ b2[n2]=0;
                    for(long i=0;i<n2;i++){
                        if(b2[i]!='M'||i+13>=n2) continue;
                        if(b2[i+1]!='D'||b2[i+2]!='M'||b2[i+3]!='2'||b2[i+4]!='A'||
                           b2[i+5]!='P'||b2[i+6]!='_'||b2[i+7]!='S'||b2[i+8]!='T'||
                           b2[i+9]!='A'||b2[i+10]!='T'||b2[i+11]!='U'||b2[i+12]!='S') continue;
                        long j=i; while(j<n2 && b2[j]!='\n') j++;
                        for(long k=i;k+1<j;k++)
                            if(b2[k]=='h'&&b2[k+1]=='i'&&(k==0||b2[k-1]==' '||b2[k-1]=='\t')){ still=1; break; }
                        break;
                    }
                }
                if(!still){ msg("[boot] !! STATUS 掉低了"); break; }
            }
            msg("[boot] 监视结束（保留进程常驻）");
            for(;;) sleep_ms(10000);
        }
        sleep_ms(500);
    }
    msg("[boot] TIMEOUT: STATUS 从未拉高");
    for(;;) sleep_ms(10000);
}

/* ---------- 子进程①：抽干 evt fifo ---------- */
static void event_drainer(void){
    long fd = sc4(56,-100,(long)"/dev/esoc-0",0x802,0);
    if(fd<0){ msg("[evt] open failed"); sc4(93,0,0,0,0); for(;;); }
    msg("[evt] drainer started (ESOC_WAIT_FOR_CRASH loop)");
    long count=0;
    for(;;){
        unsigned int evt=0;
        long r = sc4(29,fd,ESOC_WAIT_FOR_CRASH,(long)&evt,0);
        if(r==0){
            count++;
            ts(); outs("[evt] "); outn(count); outs(" = "); outn((long)evt);
            outs(" "); outs(evt_name((long)evt)); outs("\n");
        } else {
            sleep_ms(50);
        }
    }
}

void real_start(unsigned long *sp){
    (void)sp;
    L=sc4(56,-100,(long)"/data/local/tmp/diag/reqeng.log",0x0241,0644);
    msg("=== esoc engine daemon start ===");

    /* 先 fork 出 evt 抽干进程（必须在 modem 就绪前就绪） */
    long pid = sc4(220,17,0,0,0);            /* fork (clone) */
    if(pid==0){ event_drainer(); for(;;); }  /* 子进程永不返回 */
    msgn("fork'd evt drainer pid=",pid);

    /* 再 fork 出 boot_done 监视进程（等 MDM2AP_STATUS=hi → ESOC_BOOT_DONE） */
    long pid2 = sc4(220,17,0,0,0);
    if(pid2==0){ boot_done_watcher(); for(;;); }
    msgn("fork'd boot_done watcher pid=",pid2);

    long efd = sc4(56,-100,(long)"/dev/esoc-0",0x802,0);
    if(efd<0){ msgn("open /dev/esoc-0 failed ",efd); sc4(93,0,0,0,0); for(;;); }
    msgn("esoc fd=",efd);

    long r = sc4(29,efd,ESOC_REG_REQ_ENG,0,0);
    msgn("ESOC_REG_REQ_ENG -> ",r);
    if(r<0){
        msg("engine busy (mdm_helper?); retrying until free");
        for(int i=0;i<3000;i++){
            sleep_ms(200);
            r = sc4(29,efd,ESOC_REG_REQ_ENG,0,0);
            if(r==0){ msg("engine acquired after retry"); break; }
        }
        if(r<0){ msg("FATAL: never acquired req engine"); sc4(93,0,0,0,0); for(;;); }
    }

    /* 通知驱动"镜像已交付"，与厂商 mdm_helper 行为一致（会武装 MDM2AP_STATUS 检查） */
    unsigned int one=1;
    msgn("ESOC_NOTIFY(IMG_XFER_DONE) -> ", sc4(29,efd,ESOC_NOTIFY,(long)&one,0));

    /* boot_done 由 fork 出的 watcher 子进程负责（见 boot_done_watcher） */

    long last_st=-999; long req_count=0;
    for(;;){
        unsigned int req=0;
        long rr = sc4(29,efd,ESOC_WAIT_FOR_REQ,(long)&req,0);
        if(rr>=0){
            req_count++;
            if(req_count<=30 || (req_count%20)==0){
                ts(); outs("REQ #"); outn(req_count); outs(" = "); outn((long)req);
                if(req==ESOC_REQ_IMG) outs(" (ESOC_REQ_IMG — modem 重新进 PBL)");
                else if(req==ESOC_REQ_DEBUG) outs(" (ESOC_REQ_DEBUG — modem 请求 dump)");
                else if(req==ESOC_REQ_SHUTDOWN) outs(" (ESOC_REQ_SHUTDOWN — modem 主动关机)");
                outs("\n");
            }
            unsigned int st=0;
            if(sc4(29,efd,ESOC_GET_STATUS,(long)&st,0)==0 && (long)st!=last_st){
                last_st=(long)st; msgn("GET_STATUS -> ",(long)st);
            }
        } else {
            sleep_ms(100);
        }
    }
}
void _start(void){ __asm__ volatile("mov x0, sp\n bl real_start\n"); }
