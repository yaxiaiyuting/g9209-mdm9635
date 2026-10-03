/* 自写静态 ARM64 Sahara 主机加载器（完整日志版）
 * 帧格式： cmd(u32 LE) | len(u32 LE) | payload      （已由嗅探确认）
 * 流程：  HELLO -> HELLO_RESP -> 循环服务 READ_DATA -> DONE_REQ -> RESET_REQ
 */
#define CMD_HELLO          0x01
#define CMD_HELLO_RESP     0x02
#define CMD_READ_DATA      0x03
#define CMD_END_IMAGE_TX   0x04
#define CMD_DONE_REQ       0x05
#define CMD_DONE_RESP      0x06
#define CMD_RESET_REQ      0x07
#define CMD_RESET_RESP     0x08
#define CMD_MEMORY_DEBUG   0x09
#define CMD_MEMORY_READ    0x0A
#define CMD_READ_DATA_64   0x0B
#define CMD_MEM_DEBUG_64   0x0C
#define CMD_MEM_READ_64    0x0D
#define CMD_CMD_READY      0x0E
#define CMD_SWITCH_MODE    0x0F
#define CMD_EXEC           0x10
#define CMD_EXEC_RESP      0x11
#define CMD_EXEC_DATA      0x12

static long sc4(long n,long a,long b,long c,long d){
    register long x8 __asm__("x8")=n; register long x0 __asm__("x0")=a;
    register long x1 __asm__("x1")=b; register long x2 __asm__("x2")=c; register long x3 __asm__("x3")=d;
    __asm__ volatile("svc #0":"+r"(x0):"r"(x8),"r"(x1),"r"(x2),"r"(x3):"memory");
    return x0;
}
static long sc5(long n,long a,long b,long c,long d,long e){
    register long x8 __asm__("x8")=n; register long x0 __asm__("x0")=a;
    register long x1 __asm__("x1")=b; register long x2 __asm__("x2")=c; register long x3 __asm__("x3")=d;
    register long x4 __asm__("x4")=e;
    __asm__ volatile("svc #0":"+r"(x0):"r"(x8),"r"(x1),"r"(x2),"r"(x3),"r"(x4):"memory");
    return x0;
}
static long slen(const char*s){ long n=0; while(s[n]) n++; return n; }
static long L=-1;
static long esoc_fd=-1;
static void outs(const char*s){ if(L>=0&&s) sc4(64,L,(long)s,slen(s),0); }
static void outn(long v){ char b[24]; int i=23; if(v==0){outs("0");return;} if(v<0){outs("-");v=-v;} while(v>0){b[--i]='0'+(v%10);v/=10;} sc4(64,L,(long)(b+i),23-i,0); }
static void outh(unsigned long v,int digits){ const char*H="0123456789abcdef"; char b[20]; for(int i=digits-1;i>=0;i--){ b[i]=H[v&15]; v>>=4; } sc4(64,L,(long)b,digits,0); }
static void put(const char*tag,const char*s){ outs(tag); outs(s?s:"(null)"); outs("\n"); }
static void puth(const char*tag,unsigned long v,int d){ outs(tag); outh(v,d); outs("\n"); }
static void delay(long loops){ for(volatile long k=0;k<loops;k++); }

/* ---- 全局缓冲 ---- */
static unsigned char rx[64*1024];
static unsigned char tx[64*1024];

static long write_full(long fd,const unsigned char*buf,long n);   /* fwd */
static long TXLOG(long fd,const unsigned char*b,long n){
    outs("  TX"); for(long i=0;i<n && i<28;i++){ outs(" "); outh(b[i],2); } outs("\n");
    return write_full(fd,b,n);
}
/* 文件表 */
struct img { long id; const char *path; };
static struct img IMGS[] = {
    {21,"/firmware/image/sbl1.mbn"}, {25,"/firmware/image/tz.mbn"},
    {30,"/firmware/image/sdi.mbn"},  {23,"/firmware/image/rpm.mbn"},
    {31,"/firmware/image/mba.mbn"},  {8, "/firmware/image/qdsp6sw.mbn"},
    {28,"/firmware/image/dsp2.mbn"}, {6, "/firmware/image/apps.mbn"},
    {16,"/dev/block/modem/m9kefs1"},{17,"/dev/block/modem/m9kefs2"},
    {20,"/dev/block/modem/m9kefs3"},{29,"/firmware/image/acdb.mbn"},
};
#define NIMG (sizeof(IMGS)/sizeof(IMGS[0]))

static const char* find_img(long id){ for(unsigned i=0;i<NIMG;i++) if(IMGS[i].id==id) return IMGS[i].path; return 0; }

/* 带超时等可读（ppoll=73），避免驱动 read 阻塞把主循环卡死 */
static int wait_readable(long fd,long ms){
    struct { int fd; short ev; short rv; } pfd;
    struct { long s; long ns; } ts;
    pfd.fd=(int)fd; pfd.ev=0x001; pfd.rv=0;
    ts.s=ms/1000; ts.ns=(ms%1000)*1000000;
    return (int)sc5(73,(long)&pfd,1,(long)&ts,0,0);
}
/* 非阻塞读：返回 >0 数据；-11 = 暂无数据；<0 其它错误 */
static long read_nb(long fd,unsigned char*buf,long n){ return sc4(63,fd,(long)buf,n,0); }
/* 单调时钟毫秒（arm64 syscall 113 = clock_gettime，CLOCK_MONOTONIC=1） */
static long now_ms(void){ struct { long s; long ns; } ts; sc4(113,1,(long)&ts,0,0); return ts.s*1000 + ts.ns/1000000; }
static void nsleep(long ms){
    struct { long s; long ns; } ts; ts.s=ms/1000; ts.ns=(ms%1000)*1000000;
    sc4(101,(long)&ts,0,0,0);
}
/* 读满 n 字节（处理分包） */
static long read_full(long fd,unsigned char*buf,long n){
    long got=0;
    while(got<n){
        long r=sc4(63,fd,(long)(buf+got),n-got,0);
        if(r<=0) return r<0?r:got;
        got+=r;
    }
    return got;
}
/* 写满（处理短写；v17：非阻塞模式下 EAGAIN 重试并带超时诊断） */
static long write_full(long fd,const unsigned char*buf,long n){
    long sent=0; int idle=0;
    while(sent<n){
        long w=sc4(64,fd,(long)(buf+sent),n-sent,0);
        if(w==-11){                       /* EAGAIN：modem 暂时不取数据 */
            nsleep(20);
            if(++idle>250){               /* ~5s 未排空 */
                outs("!! modem stopped draining (sent "); outn(sent); outs("/"); outn(n); outs(") — abort chunk\n");
                return sent>0?sent:-1;
            }
            continue;
        }
        if(w<=0){ outs("!! write err "); outn(w); outs("\n"); return w<0?w:sent; }
        idle=0; sent+=w;
    }
    return sent;
}

void real_start(unsigned long *sp){
    (void)sp;
    L=sc4(56,-100,(long)"/data/local/tmp/diag/sahara_own.log",0x0241,0644);
    outs("=== own sahara loader v40 [no self-reset] ===\n");

    /* ★ v33：自己注册为 ESOC req engine —— 这样 mdm_subsys_powerup 不会因"等 req engine"而阻塞，
     *   从而**不需要 mdm_helper**（它会在失败时请求 SSR 重启、把 modem 打进 dump 模式）。 */
    {
        long efd = sc4(56,-100,(long)"/dev/esoc-0",0x802,0);
        esoc_fd = efd;
        if(efd >= 0){
            long r = sc4(29, efd, 0xCC07, 0, 0);      /* ioctl(ESOC_REG_REQ_ENG) */
            outs("  [esoc] req_eng register -> "); outn(r); outs("\n");
        } else {
            outs("  [esoc] open /dev/esoc-0 failed: "); outn(efd); outs("\n");
        }
    }

    static unsigned char acc[128*1024];
    long acc_len=0, served=0, hello_sent=0, done_sent=0;
    static unsigned char streamed[NIMG];
    static long off_state[NIMG];
    static long img_fd[NIMG];        /* ★ v20：缓存镜像 fd，避免每请求 open/close */
    long t0=now_ms(), served_bytes=0, last_id=-1;
    int reset_sent=0;
    int crash_req=0;
    long raw_expect=0, raw_total=0;
    long dfd=-1;
    /* ★ v21：ramdump —— 先读表，再按表逐区读内存 */
    static unsigned char mtab[8192]; static long mtab_len=0;
    int dump_state=0;              /* 0=空闲 1=正在读表 2=正在读内存区 */
    unsigned long reg_addr=0, reg_len=0, reg_got=0;
    char reg_file[80]; long reg_fd=-1; unsigned long reg_off=0, reg_next=0;
    long drained=0;
    long fd=-1;
    for(int iter=0; iter<200000; iter++){
        if(fd<0){
            fd=sc4(56,-100,(long)"/dev/ks_hsic_bridge",0x802 /*O_RDWR|O_NONBLOCK*/,0);
            if(fd<0){ nsleep(500); continue; }
            outs("port opened\n");
        }
        /* ★ v22：若正在 dump 且当前空闲，就从表里取下一个内存区发 MEMORY_READ */
        if(dump_state==2 && reg_len==0 && raw_expect==0 && reg_fd<0){
            while(reg_off + 52 <= (unsigned long)mtab_len){
                /* 条目=52字节：u32 type, u32 addr, u32 len, char name[20], char file[20] */
                unsigned long eaddr = mtab[reg_off+4]|((unsigned long)mtab[reg_off+5]<<8)|((unsigned long)mtab[reg_off+6]<<16)|((unsigned long)mtab[reg_off+7]<<24);
                unsigned long elen  = mtab[reg_off+8]|((unsigned long)mtab[reg_off+9]<<8)|((unsigned long)mtab[reg_off+10]<<16)|((unsigned long)mtab[reg_off+11]<<24);
                unsigned long ent = reg_off + 52;
                int k=0;
                for(int i=0;i<20 && mtab[reg_off+12+i]; i++){
                    unsigned char c=mtab[reg_off+12+i];
                    if(c>32 && c<127 && k<40) reg_file[k++]=(char)c;
                }
                if(k==0){ reg_file[k++]='R'; reg_file[k++]='E'; reg_file[k++]='G'; }
                reg_file[k]=0;
                if(eaddr==0 || elen==0 || elen > 16UL*1024*1024){ reg_off = ent; continue; }
                char path[128]; int q=0; const char*pre="/data/local/tmp/diag/cpdump/";
                for(int i=0;pre[i];i++) path[q++]=pre[i];
                for(int i=0;i<k;i++) path[q++]=reg_file[i];
                for(const char*suf=".BIN";*suf;suf++) path[q++]=*suf;
                path[q]=0;
                reg_addr=eaddr; reg_len=elen; reg_got=0; reg_next=ent;
                reg_fd=sc4(56,-100,(long)path,0x0241,0644);
                outs("  [dump] "); outs(reg_file); outs(" addr="); outh(reg_addr,8); outs(" len="); outn((long)reg_len); outs("\n");
                tx[0]=CMD_MEMORY_READ; tx[1]=0;tx[2]=0;tx[3]=0; tx[4]=16;tx[5]=0;tx[6]=0;tx[7]=0;
                tx[8]=reg_addr&0xff; tx[9]=(reg_addr>>8)&0xff; tx[10]=(reg_addr>>16)&0xff; tx[11]=(reg_addr>>24)&0xff;
                tx[12]=reg_len&0xff; tx[13]=(reg_len>>8)&0xff; tx[14]=(reg_len>>16)&0xff; tx[15]=(reg_len>>24)&0xff;
                TXLOG(fd,tx,16);
                raw_expect=(long)reg_len;
                reg_len=0;                    /* 标记：已发出，等数据 */
                break;
            }
            if(reg_off >= (unsigned long)mtab_len && reg_len==0 && raw_expect==0 && reg_fd<0 && dump_state==2){
                outs("  [dump] 全部内存区 dump 完成 ✓\n");
                dump_state=3;
                /* ★ v29：按 Sahara 规范，dump 结束后发 DONE_REQ，等 modem 的 DONE_RESP */
                tx[0]=CMD_DONE_REQ; tx[1]=0;tx[2]=0;tx[3]=0; tx[4]=8;tx[5]=0;tx[6]=0;tx[7]=0;
                TXLOG(fd,tx,8);
                outs("  -> dump 后发 DONE_REQ，等待 modem 继续\n");
            }
        }

        unsigned char tmp[16384];
        long n = (wait_readable(fd,40)>0) ? read_nb(fd,tmp,sizeof(tmp)) : -11;
        if(n==-11){                      /* EAGAIN：无数据 */
            nsleep(30);
            /* 每 ~2s 重开一次，兼容 EHCI 重绑导致的端口新实例 */
            static int idle=0;
            if(++idle>60){ idle=0; sc4(57,fd,0,0,0); fd=-1; }
            continue;
        }
        if(n<=0){ outs("port read err, reopen\n"); sc4(57,fd,0,0,0); fd=-1; nsleep(300); continue; }
        /* 期望原始数据时，直接把收到的字节写成内存转储 */
        if(raw_expect>0){
            long take = (n<raw_expect)?n:raw_expect;
            if(dump_state==1){                        /* 正在收"表" */
                if(mtab_len+take <= (long)sizeof(mtab)){ for(long i=0;i<take;i++) mtab[mtab_len++]=tmp[i]; }
            } else if(dump_state==2 && reg_fd>=0){    /* 正在收某个内存区 */
                sc4(64,reg_fd,(long)tmp,take,0); reg_got+=take;
            } else if(dfd>=0) sc4(64,dfd,(long)tmp,take,0);
            raw_total+=take; raw_expect-=take;
            if(raw_expect==0){
                if(dump_state==1){
                    outs("  [dump] 表已收到 "); outn(mtab_len); outs(" 字节，开始逐区 dump\n");
                    dump_state=2; reg_len=0; reg_got=0; reg_off=0; reg_addr=0;
                } else if(dump_state==2){
                    sc4(57,reg_fd,0,0,0); reg_fd=-1;
                    outs("  [dump] "); outs(reg_file); outs(" 完成 "); outn(reg_got); outs(" 字节\n");
                    reg_off = reg_next;              /* ★ v24：推进到下一条目 */
                }
            }
            if(n<=take) continue;                     /* 整块都是原始数据 */
            for(long i=take;i<n;i++) acc[acc_len++]=tmp[i];   /* 余下按包解析 */
            continue;
        }
        if(acc_len+n > (long)sizeof(acc)) acc_len=0;      /* 防御 */
        for(long i=0;i<n;i++) acc[acc_len++]=tmp[i];

        /* 解析所有完整包 */
        while(acc_len>=8){
            unsigned long cmd=(unsigned long)acc[0]|((unsigned long)acc[1]<<8)|((unsigned long)acc[2]<<16)|((unsigned long)acc[3]<<24);
            unsigned long len=(unsigned long)acc[4]|((unsigned long)acc[5]<<8)|((unsigned long)acc[6]<<16)|((unsigned long)acc[7]<<24);
            if(len<8 || len>64*1024){
                outs("bad len="); outn(len); outs(" head:");
                for(long z=0; z<acc_len && z<16; z++){ outs(" "); outh(acc[z],2); }
                outs("  -> drop 1 byte, resync\n");
                for(long z=1; z<acc_len; z++) acc[z-1]=acc[z];
                acc_len--; continue;                      /* 逐字节重同步 */
            }
            if(acc_len<(long)len) break;
            outs("RX cmd="); outn(cmd); outs(" len="); outn(len); outs(" hex:");
            for(unsigned long i=0;i<len && i<28;i++){ outs(" "); outh(acc[i],2); }
            outs("\n");
            if(cmd==CMD_HELLO){
                unsigned long mode=(unsigned long)acc[20]|((unsigned long)acc[21]<<8)|((unsigned long)acc[22]<<16)|((unsigned long)acc[23]<<24);
                outs("  HELLO mode="); outn(mode); outs(" raw:");
                for(unsigned long i=0;i<len && i<24;i++){ outs(" "); outh(acc[i],2); }
                outs("\n");
                /* ★ 按原厂 ks 的实测字节构造（不是回显 HELLO！）：
                 *   cmd=2, len=48, version=2, version_min=2, max_cmd_len=0, 其余 0 */
                for(unsigned long i=0;i<48;i++) tx[i]=0;
                tx[0]=CMD_HELLO_RESP; tx[1]=0; tx[2]=0; tx[3]=0;
                tx[4]=48; tx[5]=0; tx[6]=0; tx[7]=0;
                tx[8]=2; tx[9]=0; tx[10]=0; tx[11]=0;      /* version = 2 */
                tx[12]=2; tx[13]=0; tx[14]=0; tx[15]=0;    /* version_min = 2 (ks 实测) */
                /* max_cmd_len (16..19) = 0 (ks 实测) */
                tx[20]=(unsigned char)(mode&0xff); tx[21]=(unsigned char)((mode>>8)&0xff);
                tx[22]=(unsigned char)((mode>>16)&0xff); tx[23]=(unsigned char)((mode>>24)&0xff);
                outs("  (HELLO mode echoed = "); outn((long)mode); outs(")\n");
                if(TXLOG(fd,tx,48)==48){ hello_sent++; outs("  HELLO_RESP sent #"); outn(hello_sent); outs("\n"); }
                else outs("  TX HELLO_RESP FAILED\n");
            }
            else if(cmd==CMD_READ_DATA || cmd==CMD_READ_DATA_64){
                long id=(long)acc[8]|((long)acc[9]<<8)|((long)acc[10]<<16)|((long)acc[11]<<24);
                long off=(long)acc[12]|((long)acc[13]<<8)|((long)acc[14]<<16)|((long)acc[15]<<24);
                long reqlen=(long)acc[16]|((long)acc[17]<<8)|((long)acc[18]<<16)|((long)acc[19]<<24);
                const char*p=find_img(id);
                if(!p){ outs("  !! no mapping id="); outn(id); outs("\n"); acc_len=0; break; }
                if(reqlen<=0 || reqlen>64*1024*1024){ outs("  !! bad reqlen "); outn(reqlen); outs("\n"); acc_len=0; break; }
                unsigned iidx=0; for(unsigned i=0;i<NIMG;i++) if(IMGS[i].id==id) iidx=i;
                long f=img_fd[iidx];
                if(f<=0){ f=sc4(56,-100,(long)p,0,0); if(f<0){ outs("  open failed\n"); acc_len=0; break; } img_fd[iidx]=f; }
                /* ★ v15：请求可能远大于缓冲 —— 循环发完整个 reqlen */
                long sent_total=0, o=off, remain=reqlen; int werr=0;
                while(remain>0){
                    long n = remain > (long)sizeof(tx) ? (long)sizeof(tx) : remain;
                    long got=sc4(67,f,(long)tx,n,o);
                    if(got<=0) break;
                    if(write_full(fd,tx,got)!=got){ werr=1; break; }
                    o+=got; sent_total+=got; remain-=got;
                }
                served++; served_bytes+=sent_total;
                if((served<8)||((served%64)==0)||sent_total!=reqlen){
                    outs("  READ id="); outn(id); outs(" off="); outn(off); outs(" req="); outn(reqlen);
                    outs(" sent="); outn(sent_total); outs(werr?" (WRITE ERR)":(sent_total!=reqlen?" (SHORT!)":"")); outs("\n");
                }
                if((served%64)==0){
                    long el=now_ms()-t0;
                    outs("    [stats] chunks="); outn(served); outs(" bytes="); outn(served_bytes);
                    outs(" elapsed="); outn(el); outs("ms rate="); outn(served_bytes/(el?el:1)); outs(" KB/s\n");
                }
                if(werr || sent_total!=reqlen){ acc_len=0; break; }
            }            else if(cmd==CMD_END_IMAGE_TX){
                unsigned long id=(unsigned long)acc[8]|((unsigned long)acc[9]<<8)|((unsigned long)acc[10]<<16)|((unsigned long)acc[11]<<24);
                unsigned long st=(unsigned long)acc[12]|((unsigned long)acc[13]<<8)|((unsigned long)acc[14]<<16)|((unsigned long)acc[15]<<24);
                outs("  END_IMAGE_TX id="); outn(id); outs(" status="); outn(st);
                last_id=(long)id;
                /* ★ v31：按 modem 自己的状态机回复 ——
                 *   modem: "Send END_IMAGE_TX" → "Received a response about END_IMAGE_TX
                 *           from host" → "send DONE_RESP to host"
                 *   所以主机要回一个 **END_IMAGE_TX**（同样的命令，status=0）作为应答。 */
                /* ★ v32：DONE_REQ 才是 modem 认的应答（实测它会回 DONE_RESP）；
                 *   END_IMAGE_TX 应答会被它回 status=1 并陷入循环 ✗ */
                /* ★ v34：每次 END_IMAGE_TX 都回 DONE_REQ（v29 用这个流程时 12 个镜像全过 ✓） */
                tx[0]=CMD_DONE_REQ; tx[1]=0;tx[2]=0;tx[3]=0; tx[4]=8;tx[5]=0;tx[6]=0;tx[7]=0;
                TXLOG(fd,tx,8);
                outs("  -> sent DONE_REQ (v34)\n");
            }
            else if(cmd==CMD_DONE_RESP){
                outs("  DONE_RESP  last_id="); outn(last_id); outs("\n");
                /* ★ v35：按 Sahara 规范，全部镜像喂完后发 RESET_REQ，
                 *   让 modem 执行已加载固件（此前那次有 mdm_helper/SSR 干扰，结论不可靠）。 */
                if(last_id==6 && !crash_req){
                    /* ★★ v40：**移除** v39 的自我破坏逻辑 ★★
                     *
                     * 实测证据（本机 dmesg，2026-10-03）：
                     *   [53.75] Signaling request engine for images → 12 镜像全 status=0
                     *   [65.22] esoc_dev_ioctl, ESOC_SET_CRASH_OCCURRENCE, status: 1
                     *           subsys-restart: ... Restart sequence requested for esoc0
                     *   [65.23] status = 1: mdm is now ready        ← modem 其实起来了
                     *   [76.42] unexpected reset external modem     ← 11.2s 后被这条 SSR 复位
                     *
                     * 即 v39 在最后一个镜像的 DONE_RESP 后立刻调用
                     *   ESOC_SET_CRASH + 写 "restart" 到 msm_subsys/esoc0
                     * → 我们自己把刚启动的 modem 复位了。modem 从未有机会常驻。
                     *
                     * 现在 boot_done 窗口已是 900s（boot-AD/V），fed 完约 20s，
                     * 完全不需要"先 dump 再继续"这套 workaround。故此处只记录、不动作。
                     */
                    outs("  -> [v40] 全部镜像完成；不触发 SSR/CRASH（避免自我复位）\n");
                    crash_req=1;
                }
                done_sent=1;
            }
            else if(cmd==CMD_RESET_RESP){ outs("  RESET_RESP — 完成\n"); sc4(93,0,0,0,0); for(;;); }
            else if(cmd==CMD_CMD_READY){
                outs("  CMD_READY (modem 进入命令模式)\n");
                tx[0]=CMD_SWITCH_MODE; tx[1]=0;tx[2]=0;tx[3]=0; tx[4]=12;tx[5]=0;tx[6]=0;tx[7]=0;
                tx[8]=0;tx[9]=0;tx[10]=0;tx[11]=0;   /* mode=0: 请求回到图像传输模式 */
                TXLOG(fd,tx,12);
            }
            else if(cmd==CMD_SWITCH_MODE||cmd==CMD_EXEC||cmd==CMD_EXEC_RESP||cmd==CMD_EXEC_DATA){
                outs("  <command-mode packet>\n");
            }
            else if(cmd==CMD_MEMORY_DEBUG||cmd==CMD_MEM_DEBUG_64){
                /* 载荷是 (地址,长度) 表；主机发 MEMORY_READ 取数据（原始字节流回复） */
                unsigned long addr=(unsigned long)acc[8]|((unsigned long)acc[9]<<8)|((unsigned long)acc[10]<<16)|((unsigned long)acc[11]<<24);
                unsigned long mlen=(unsigned long)acc[12]|((unsigned long)acc[13]<<8)|((unsigned long)acc[14]<<16)|((unsigned long)acc[15]<<24);
                outs("  MEMORY_DEBUG addr="); outh(addr,8); outs(" len="); outn((long)mlen); outs("\n");
                if(mlen>0 && mlen<=4*1024*1024){
                    mtab_len=0; dump_state=1;
                    tx[0]=CMD_MEMORY_READ; tx[1]=0;tx[2]=0;tx[3]=0; tx[4]=16;tx[5]=0;tx[6]=0;tx[7]=0;
                    tx[8]=addr&0xff; tx[9]=(addr>>8)&0xff; tx[10]=(addr>>16)&0xff; tx[11]=(addr>>24)&0xff;
                    tx[12]=mlen&0xff; tx[13]=(mlen>>8)&0xff; tx[14]=(mlen>>16)&0xff; tx[15]=(mlen>>24)&0xff;
                    TXLOG(fd,tx,16);
                    raw_expect=(long)mlen;
                    outs("  -> MEMORY_READ issued, expecting "); outn(raw_expect); outs(" raw bytes\n");
                }
            }
            else outs("  <unhandled>\n");
            /* 消费该包 */
            long rest=acc_len-(long)len;
            for(long i=0;i<rest;i++) acc[i]=acc[(long)len+i];
            acc_len=rest;
        }
    }
    outs("=== loader exit, served="); outn(served); outs(" ===\n");
    sc4(93,0,0,0,0); for(;;);
}
void _start(void){ __asm__ volatile("mov x0, sp\n bl real_start\n"); }
