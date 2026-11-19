/*
 * MIT License with Commons Clause
 *
 * Copyright (c) 2026 Jeff Curless
 *
 * Required Notice: Copyright (c) 2026 Jeff Curless.
 *
 * This software is licensed under the MIT License, subject to the Commons Clause
 * License Condition v1.0. You may use, copy, modify, and distribute this software,
 * but you may not sell the software itself, offer it as a paid service, or use it
 * in a product or service whose value derives substantially from the software
 * without prior written permission from the copyright holder.
 */

/*
 * hwtest.c — device side of the hardware test suite (hwtest/ on the host)
 *
 * The host drives picoOS through the USB shell, one command at a time.  A
 * round trip costs milliseconds, so anything that must happen inside a
 * narrower window than that cannot be timed from the host.  This app is
 * where such sequences live.  It has two kinds of mode:
 *
 *   self-checking   run the whole sequence on the device and print
 *                   "[hwtest] <mode>: RESULT: PASS|FAIL".  The host only
 *                   starts it and reads the verdict.
 *   holder          set up a state and hold it for a long, fixed time (or
 *                   forever), printing what the host needs (PID, TID).  The
 *                   host then acts on it at its own pace: the window is
 *                   seconds wide, so host timing does not matter.
 *
 *   run hwtest                 list the modes
 *   run hwtest kill-mutex      self-checking: a kill waits for kmutex_unlock (#26)
 *   run hwtest kill-file       self-checking: a killed process's files are closed (#26)
 *   run hwtest fs-longname     self-checking: an over-long file name is refused (#7)
 *   run hwtest fs-fds          self-checking: readers during a write, deleted-file fds (#31, #8)
 *   run hwtest holdmutex       holder: lock a mutex for HOLD_MS, then unlock
 *   run hwtest holdfile        holder: keep hwt_hold.txt open for writing, forever
 *   run hwtest leakfile        holder: write hwt_leak.txt and exit without closing
 */

#include "hwtest.h"
#include "../kernel/sync.h"
#include "../kernel/syscall.h"
#include "../kernel/task.h"
#include "../kernel/mem.h"
#include "../kernel/vfs.h"
#include "../kernel/fs.h"
#include "../shell/shell.h"

#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#define HOLD_MS  3000u   /* holdmutex: wide enough for a host to kill it */

/* -------------------------------------------------------------------------
 * Check helpers — every self-checking mode reports through these
 * ------------------------------------------------------------------------- */
static const char *s_mode;
static int         s_fails;

static void check(bool ok, const char *what)
{
    shell_print("[hwtest]   %s  %s\r\n", ok ? "ok  " : "FAIL", what);
    if (!ok) {
        s_fails++;
    }
}

static void result(void)
{
    shell_print("[hwtest] %s: RESULT: %s\r\n", s_mode, s_fails == 0 ? "PASS" : "FAIL");
}

/* Wait up to ms for *flag, polling every 10 ms. */
static bool wait_flag(volatile bool *flag, uint32_t ms)
{
    for (uint32_t t = 0u; t < ms && !*flag; t += 10u) {
        sys_sleep(10);
    }
    return *flag;
}

static bool thread_gone(uint32_t tid)
{
    return task_find_thread(tid) == NULL;
}

/* =========================================================================
 * kill-mutex — a thread killed while it holds a kmutex keeps running until
 * its last kmutex_unlock(), dies there, and leaves the mutex free.
 * ========================================================================= */
static kmutex_t       km_mutex;
static volatile bool  km_locked;
static volatile bool  km_after_unlock;   /* must never be set once killed */
static volatile bool  km_free_running;

static void km_holder(void *arg)
{
    (void)arg;
    kmutex_lock(&km_mutex);
    km_locked = true;
    sys_sleep(500);                  /* killed during this sleep */
    kmutex_unlock(&km_mutex);        /* ...and dies in here */
    km_after_unlock = true;
}

static void km_free_thread(void *arg)
{
    (void)arg;
    km_free_running = true;
    for (;;) {
        sys_sleep(100);
    }
}

static void mode_kill_mutex(void)
{
    pcb_t *proc = task_find_process((uint32_t)sys_getpid());

    /* Control: a thread holding nothing is freed at once. */
    km_free_running = false;
    tcb_t *f = task_create_thread(proc, "hwt-free", km_free_thread, NULL, 4u,
                                  DEFAULT_STACK_SIZE);
    check(f != NULL, "create control thread");
    if (f != NULL) {
        uint32_t ftid = f->tid;
        wait_flag(&km_free_running, 1000u);
        syscall_dispatch(SYS_KILL, ftid, 0, 0, 0);
        check(thread_gone(ftid), "thread holding no mutex is freed at once");
    }

    /* The case under test. */
    kmutex_init(&km_mutex);
    km_locked       = false;
    km_after_unlock = false;
    tcb_t *h = task_create_thread(proc, "hwt-holder", km_holder, NULL, 4u,
                                  DEFAULT_STACK_SIZE);
    check(h != NULL, "create holder thread");
    if (h == NULL) {
        return;
    }
    uint32_t tid = h->tid;
    check(wait_flag(&km_locked, 1000u), "holder took the mutex");

    syscall_dispatch(SYS_KILL, tid, 0, 0, 0);
    tcb_t *t = task_find_thread(tid);
    check(t != NULL, "holder still exists right after the kill");
    check(t != NULL && t->kill_pending, "holder is marked kill_pending");
    check(t != NULL && t->mutex_depth == 1u, "holder's mutex_depth is 1");

    /* It sleeps 500 ms, then unlocks and should die in kmutex_unlock(). */
    bool gone = false;
    for (int i = 0; i < 150 && !gone; i++) {
        sys_sleep(10);
        gone = thread_gone(tid);
    }
    check(gone, "holder is gone after it unlocked");
    check(!km_after_unlock, "holder did not run past kmutex_unlock()");
    check(km_mutex.owner_tid == -1, "mutex is free");

    kmutex_lock(&km_mutex);          /* would hang if it had leaked */
    check(km_mutex.owner_tid == sys_gettid(), "mutex can be locked again");
    kmutex_unlock(&km_mutex);
}

/* =========================================================================
 * kill-file — a process killed with a file open for writing has that file
 * closed for it: the data is committed and the write buffer is free.
 * ========================================================================= */
#define KF_NAME  "hwt_kill.txt"
#define KF_DATA  "written-before-kill\n"

static volatile bool     kf_ready;
static volatile uint32_t kf_tid;

static void kf_writer(void *arg)
{
    (void)arg;
    kf_tid = (uint32_t)sys_gettid();
    int fd = vfs_open(KF_NAME, VFS_O_WRONLY | VFS_O_CREAT | VFS_O_TRUNC);
    if (fd >= 0) {
        vfs_write(fd, (const uint8_t *)KF_DATA, (uint32_t)strlen(KF_DATA));
        kf_ready = true;
    }
    for (;;) {
        sys_sleep(1000);   /* never closes fd */
    }
}

static void mode_kill_file(void)
{
    kf_ready = false;
    kf_tid   = 0u;
    int pid = (int)syscall_dispatch(SYS_SPAWN, (uint32_t)(uintptr_t)"hwt-writer",
                                    (uint32_t)(uintptr_t)kf_writer, 0u, 0u);
    check(pid > 0, "spawn writer process");
    if (pid <= 0) {
        return;
    }
    check(wait_flag(&kf_ready, 2000u), "writer opened and wrote " KF_NAME);
    if (!kf_ready) {
        return;
    }

    int busy = vfs_open(KF_NAME, VFS_O_WRONLY);
    check(busy < 0, "a second writer is refused while the writer lives");
    if (busy >= 0) {
        vfs_close(busy);
    }

    syscall_dispatch(SYS_KILL, kf_tid, 0, 0, 0);   /* reaps its files too */
    check(task_find_process((uint32_t)pid) == NULL, "writer process is gone");

    char buf[64];
    int  n  = -1;
    int  fd = vfs_open(KF_NAME, VFS_O_RDONLY);
    if (fd >= 0) {
        n = vfs_read(fd, (uint8_t *)buf, sizeof(buf) - 1u);
        vfs_close(fd);
    }
    check(n == (int)strlen(KF_DATA) && memcmp(buf, KF_DATA, (size_t)n) == 0,
          "the killed writer's data was committed");

    fd = vfs_open(KF_NAME, VFS_O_WRONLY | VFS_O_TRUNC);
    check(fd >= 0, "the write buffer is free again");
    if (fd >= 0) {
        vfs_write(fd, (const uint8_t *)"ok\n", 3u);
        check(vfs_close(fd) == 0, "a new write commits");
    }
}

/* -------------------------------------------------------------------------
 * fs-longname (#7) — straight through the VFS, past the shell's own length
 * check.  A name of FS_NAME_MAX characters used to be stored cut to 15, so
 * every create made another entry under the short name.
 * ------------------------------------------------------------------------- */
#define LONG_NAME   "hwt_longname_16c"   /* FS_NAME_MAX (16) chars */
#define SHORT_NAME  "hwt_longname_16"    /* its first 15 */

_Static_assert(sizeof(LONG_NAME) - 1u == FS_NAME_MAX, "LONG_NAME must be FS_NAME_MAX chars");

static void mode_fs_longname(void)
{
    fs_delete(SHORT_NAME);   /* leftovers from an older build */

    for (int i = 0; i < 2; i++) {
        int fd = vfs_open(LONG_NAME, VFS_O_WRONLY | VFS_O_CREAT | VFS_O_TRUNC);
        check(fd < 0, "create with a 16-char name is refused");
        if (fd >= 0) { vfs_close(fd); }
    }
    int fd = vfs_open(SHORT_NAME, VFS_O_RDONLY);
    check(fd < 0, "no entry was made under the 15-char prefix");
    if (fd >= 0) { vfs_close(fd); }

    fd = vfs_open(SHORT_NAME, VFS_O_WRONLY | VFS_O_CREAT | VFS_O_TRUNC);
    check(fd >= 0, "the 15-char prefix is a valid name");
    if (fd >= 0) { vfs_close(fd); }
    fd = vfs_open(LONG_NAME, VFS_O_RDONLY);
    check(fd < 0, "the 16-char name does not open the 15-char file");
    if (fd >= 0) { vfs_close(fd); }

    check(fs_delete(LONG_NAME) == -1, "delete with the 16-char name finds nothing");
    check(fs_delete(SHORT_NAME) == 0, "the 15-char file deletes");
}

/* -------------------------------------------------------------------------
 * fs-fds (#31, #8) — what other descriptors see.
 *   #31  While a write is pending, another fd reads the committed file: it
 *        used to get the writer's new size, and 0xFF from flash past the
 *        old end.
 *   #8   An fd left open on a deleted file used to point at a freed fs
 *        slot; the next file opened got that slot, and the old fd read it.
 * ------------------------------------------------------------------------- */
#define FDS_A  "hwt_fds_a.txt"
#define FDS_B  "hwt_fds_b.txt"

static void mode_fs_fds(void)
{
    uint8_t buf[32];
    fs_delete(FDS_A);
    fs_delete(FDS_B);

    int w = vfs_open(FDS_A, VFS_O_WRONLY | VFS_O_CREAT | VFS_O_TRUNC);
    check(w >= 0 && vfs_write(w, (const uint8_t *)"old", 3u) == 3, "write 3 bytes");
    check(vfs_close(w) == 0, "commit them");

    /* #31 */
    w = vfs_open(FDS_A, VFS_O_WRONLY | VFS_O_APPEND);
    check(w >= 0 && vfs_write(w, (const uint8_t *)"-and-more", 9u) == 9, "pending append of 9");
    int r = vfs_open(FDS_A, VFS_O_RDONLY);
    memset(buf, 0, sizeof(buf));
    int n = vfs_read(r, buf, sizeof(buf));
    check(n == 3 && memcmp(buf, "old", 3) == 0, "another fd reads the committed 3 bytes");
    vfs_close(r);
    check(vfs_close(w) == 0, "commit the append");
    r = vfs_open(FDS_A, VFS_O_RDONLY);
    memset(buf, 0, sizeof(buf));
    n = vfs_read(r, buf, sizeof(buf));
    check(n == 12 && memcmp(buf, "old-and-more", 12) == 0, "then all 12 bytes");

    /* #8: r stays open across the delete */
    check(fs_delete(FDS_A) == 0, "delete the file while an fd is open on it");
    int b = vfs_open(FDS_B, VFS_O_WRONLY | VFS_O_CREAT | VFS_O_TRUNC);
    check(b >= 0 && vfs_write(b, (const uint8_t *)"bbbb", 4u) == 4, "write another file");
    check(vfs_close(b) == 0, "commit it");
    b = vfs_open(FDS_B, VFS_O_RDONLY);
    check(vfs_read(r, buf, sizeof(buf)) == -1, "the old fd fails instead of reading the new file");
    memset(buf, 0, sizeof(buf));
    check(vfs_read(b, buf, sizeof(buf)) == 4 && memcmp(buf, "bbbb", 4) == 0,
          "the new file reads back intact");
    check(vfs_close(r) == 0, "the stale fd closes");
    vfs_close(b);
    check(fs_delete(FDS_B) == 0, "clean up");
}

/* =========================================================================
 * Holder modes — the host acts while these hold a state
 * ========================================================================= */
static void mode_holdmutex(void)
{
    static kmutex_t m;
    kmutex_init(&m);
    kmutex_lock(&m);
    shell_print("[hwtest] holdmutex: pid %d tid %d holding for %u ms\r\n",
                sys_getpid(), sys_gettid(), (unsigned)HOLD_MS);
    sys_sleep(HOLD_MS);
    shell_print("[hwtest] holdmutex: releasing\r\n");
    kmutex_unlock(&m);   /* a killed holder dies in here */
    shell_print("[hwtest] holdmutex: released (not killed)\r\n");
}

static void mode_holdfile(void)
{
    int fd = vfs_open("hwt_hold.txt", VFS_O_WRONLY | VFS_O_CREAT | VFS_O_TRUNC);
    if (fd < 0) {
        shell_print("[hwtest] holdfile: open failed\r\n");
        return;
    }
    vfs_write(fd, (const uint8_t *)"held-by-holdfile\n", 17u);
    shell_print("[hwtest] holdfile: pid %d tid %d holding fd %d\r\n",
                sys_getpid(), sys_gettid(), fd);
    for (;;) {
        sys_sleep(1000);
    }
}

static void mode_leakfile(void)
{
    int fd = vfs_open("hwt_leak.txt", VFS_O_WRONLY | VFS_O_CREAT | VFS_O_TRUNC);
    if (fd < 0) {
        shell_print("[hwtest] leakfile: open failed\r\n");
        return;
    }
    vfs_write(fd, (const uint8_t *)"leaked-by-leakfile\n", 19u);
    shell_print("[hwtest] leakfile: pid %d exiting with fd %d open\r\n",
                sys_getpid(), fd);
    /* return without vfs_close(fd) */
}

/* =========================================================================
 * Entry point
 * ========================================================================= */
typedef struct {
    const char *name;
    void      (*fn)(void);
    bool        self_checking;
    const char *help;
} hwtest_mode_t;

static const hwtest_mode_t modes[] = {
    { "kill-mutex", mode_kill_mutex, true,  "kill waits for kmutex_unlock (#26)" },
    { "kill-file",  mode_kill_file,  true,  "killed process's files are closed (#26)" },
    { "fs-longname", mode_fs_longname, true, "over-long file name is refused (#7)" },
    { "fs-fds",     mode_fs_fds,     true,  "readers during a write, deleted-file fds (#31, #8)" },
    { "holdmutex",  mode_holdmutex,  false, "hold a mutex for 3 s" },
    { "holdfile",   mode_holdfile,   false, "keep hwt_hold.txt open for writing" },
    { "leakfile",   mode_leakfile,   false, "exit with hwt_leak.txt still open" },
};

void hwtest(void *arg)
{
    char name[16] = "";
    if (arg != NULL) {
        strncpy(name, (const char *)arg, sizeof(name) - 1u);
        kfree(arg);   /* run's argument copy belongs to us */
    }

    for (size_t i = 0u; i < sizeof(modes) / sizeof(modes[0]); i++) {
        if (strcmp(name, modes[i].name) != 0) {
            continue;
        }
        if (modes[i].self_checking) {
            s_mode  = modes[i].name;
            s_fails = 0;
            shell_print("[hwtest] %s: start\r\n", s_mode);
            modes[i].fn();
            result();
        } else {
            modes[i].fn();
        }
        return;
    }

    shell_print("[hwtest] usage: run hwtest <mode>\r\n");
    for (size_t i = 0u; i < sizeof(modes) / sizeof(modes[0]); i++) {
        shell_print("[hwtest]   %-11s %s\r\n", modes[i].name, modes[i].help);
    }
}
