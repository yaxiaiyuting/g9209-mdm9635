/* 直接 syscall，不依赖 libc/头文件：openat + ioctl(SUBSYS_POWERUP=0xCD01) */
static long sc3(long n, long a, long b, long c) {
    register long x8 __asm__("x8") = n;
    register long x0 __asm__("x0") = a;
    register long x1 __asm__("x1") = b;
    register long x2 __asm__("x2") = c;
    __asm__ volatile("svc #0" : "+r"(x0) : "r"(x8), "r"(x1), "r"(x2) : "memory");
    return x0;
}
void _start(void) {
    const char *p = "/dev/subsys_esoc0";
    long fd = sc3(56 /*openat*/, -100 /*AT_FDCWD*/, (long)p, 2 /*O_RDWR*/);
    if (fd < 0) { sc3(93 /*exit*/, 1, 0, 0); for(;;); }
    long r = sc3(29 /*ioctl*/, fd, 0xCD01, 0);
    sc3(93, r < 0 ? 2 : 0, 0, 0);
    for(;;);
}
