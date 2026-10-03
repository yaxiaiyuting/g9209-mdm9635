/* mdmdump.c — 通过 Sahara MEMORY_DEBUG/MEMORY_READ 读 modem 内存/寄存器
 * 静态 ARM64，纯 syscall。
 *
 * 用途：modem 每次失败后会回到 PBL（05c6:9008）并响应 Sahara。
 * 此时我们可以用 Sahara 的 memory-debug 模式直接读它的**活寄存器**，
 * 从而判定它上次为什么复位（看门狗 vs PMIC vs 软复位）。
 *
 * 用法：mdmdump <addr_hex> <len_hex> [outfile]
 *   例：mdmdump 0xfc4b0000 0x100 /data/local/tmp/diag/wdog.bin
 */
#define CMD_HELLO        0x01
#define CMD_HELLO_RESP   0x02
#define CMD_READ_DATA    0x03
#define CMD_END_IMAGE_TX 0x04
#define CMD_DONE_REQ     0x05
#define CMD_DONE_RESP    0x06
#define CMD_MEMORY_DEBUG 0x09
#define CMD_MEMORY_READ  0x0A

static long sc4(long n,long a,long b,long c,long d){
    register long x8 __asm__("x8")=n; register long x0 __asm__("x0")=a;
    register long x1 __asm__("x1")=b; register long x2 __asm__("x2")=c; register long x3 __asm__("x3")=d;
    __asm__ volatile("svc #0":"+r"(x0):"r"(x8),"r"(x1),"r"(x2),"r"(x3):"memory");
    return x0;
}
static long slen(const char*s){ long n=0; while(s[n]) n++; return n; }
static long L=-1;
static void outs(const char*s){ if(L>=0&&s) sc4(64,L,(long)s,slen(s),0); }
static void outs2(const char*s){ sc4(64,1,(long)s,slen(s),0); }
static void outn(long v){ char b[24]; int i=23; if(v==0){outs("0");return;} if(v<0){outs("-");v=-v;} while(v>0){b[--i]='0'+(v%10);v/=10;} sc4(64,L,(long)(b+i),23-i,0); }
static void outh(unsigned long v,int d){ const char*H="0123456789abcdef"; char b[20]; for(int i=d-1;i>=0;i--){ b[i]=H[v&15]; v>>=4; } sc4(64,L,(long)b,d,0); }
static void ts(void){ struct { long s; long ns; } t; sc4(113,1,(long)&t,0,0);
    outs("["); outn(t.s); outs("."); long ms=t.ns/1000000; if(ms<100) outs("0"); if(ms<10) outs("0"); outn(ms); outs("] "); }
static void msg(const char*s){ ts(); outs(s); outs("\n"); }
static void msgn(const char*a,long v){ ts(); outs(a); outn(v); outs("\n"); }
static void sleep_ms(long ms){ struct { long s; long ns; } t; t.s=ms/1000; t.ns=(ms%1000)*1000000; sc4(101,(long)&t,0,0,0); }

static unsigned char buf[512*1024];

/* 解析十六进制字符串 */
static unsigned long parse_hex(const char*s){
    unsigned long v=0; int n=0;
    if(s[0]=='0'&&(s[1]=='x'||s[1]=='X')) s+=2;
    while(*s && n<16){
        char c=*s++;
        unsigned long d;
        if(c>='0'&&c<='9') d=c-'0';
        else if(c>='a'&&c<='f') d=c-'a'+10;
        else if(c>='A'&&c<='F') d=c-'A'+10;
        else break;
        v=(v<<4)|d; n++;
    }
    return v;
}

void real_start(unsigned long *sp){
    long argc=(long)sp[0];
    char **argv=(char**)(sp+1);
    (void)argc;

    L=sc4(56,-100,(long)"/data/local/tmp/diag/mdmdump.log",0x0241,0644);

    if(argc<3){
        outs2("usage: mdmdump <addr_hex> <len_hex> [outfile]\n");
        sc4(93,2,0,0,0); for(;;);
    }
    unsigned long addr=parse_hex(argv[1]);
    unsigned long len =parse_hex(argv[2]);
    const char *outf = (argc>=4)?argv[3]:"/data/local/tmp/diag/memread.bin";
    if(len==0 || len>sizeof(buf)) len=sizeof(buf);

    ts(); outs("=== mdmdump addr=0x"); outh(addr,8); outs(" len="); outn((long)len); outs(" ===\n");

    long fd=sc4(56,-100,(long)"/dev/ks_hsic_bridge",0x802,0);
    if(fd<0){ msgn("open bridge failed ",fd); sc4(93,1,0,0,0); for(;;); }
    msgn("bridge fd=",fd);

    /* 等 HELLO（最多 20s） */
    long got=0, n;
    for(int i=0;i<200 && !got;i++){
        n=sc4(63,fd,(long)buf,sizeof(buf),0);
        if(n>=8 && buf[0]==CMD_HELLO){
            got=1;
            ts(); outs("RX HELLO len="); outn(buf[4]|(buf[5]<<8)|(buf[6]<<16)|((long)buf[7]<<24)); outs("\n");
        } else if(n>0){
            ts(); outs("RX cmd="); outn(buf[0]); outs(" (非 HELLO，忽略)\n");
        } else sleep_ms(100);
    }
    if(!got){ msg("!! 未收到 HELLO，modem 可能不在 Sahara"); sc4(93,1,0,0,0); for(;;); }

    /* 回 HELLO_RESP */
    for(int i=0;i<48;i++) buf[i]=0;
    buf[0]=CMD_HELLO_RESP; buf[4]=48; buf[8]=2; buf[12]=2;
    long w=sc4(64,fd,(long)buf,48,0);
    msgn("TX HELLO_RESP -> ",w);
    sleep_ms(200);

    /* 发 MEMORY_READ(addr,len) */
    buf[0]=CMD_MEMORY_READ; buf[4]=16;
    buf[8]=addr&0xff; buf[9]=(addr>>8)&0xff; buf[10]=(addr>>16)&0xff; buf[11]=(addr>>24)&0xff;
    buf[12]=len&0xff; buf[13]=(len>>8)&0xff; buf[14]=(len>>16)&0xff; buf[15]=(len>>24)&0xff;
    w=sc4(64,fd,(long)buf,16,0);
    msgn("TX MEMORY_READ -> ",w);

    /* 收原始数据 */
    long ofd=sc4(56,-100,(long)outf,0x0241,0644);
    long total=0;
    for(int i=0;i<400 && (unsigned long)total<len;i++){
        n=sc4(63,fd,(long)buf,sizeof(buf),0);
        if(n>0){
            long take=n; if((unsigned long)(total+take)>len) take=(long)len-total;
            if(ofd>=0) sc4(64,ofd,(long)buf,take,0);
            total+=take;
        } else sleep_ms(50);
    }
    if(ofd>=0) sc4(57,ofd,0,0,0);
    msgn("收到字节 ",total);

    if(total>0){
        /* hexdump 前 256 字节 */
        ts(); outs("hexdump:\n");
        for(long r=0;r<total && r<256;r+=16){
            outs("  "); outh((unsigned long)(addr+r),8); outs(":");
            for(long c=0;c<16 && r+c<total;c++){ outs(" "); outh(buf[r+c],2); }
            outs("\n");
        }
    }
    ts(); outs("=== mdmdump done ===\n");
    sc4(93,0,0,0,0); for(;;);
}
void _start(void){ __asm__ volatile("mov x0, sp\n bl real_start\n"); }
