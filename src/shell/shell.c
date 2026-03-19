#include "shell.h"
#include "../kernel/task.h"
#include "../kernel/sched.h"
#include "../kernel/mem.h"
#include "../kernel/vfs.h"
#include "../kernel/fs.h"
#include "../kernel/syscall.h"
#include "../apps/demo.h"

#include "../kernel/arch.h"   /* SDK wrappers + host stubs */

#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <stdlib.h>   /* strtol */

/* -------------------------------------------------------------------------
 * Version — defined by CMakeLists.txt via target_compile_definitions
 * ------------------------------------------------------------------------- */
#ifdef PICOOS_DISPLAY_ENABLE
#include "../drivers/display.h"
#endif

/* -------------------------------------------------------------------------
 * trace_enabled — defined in main.c
 * ------------------------------------------------------------------------- */
extern volatile bool trace_enabled;

/* -------------------------------------------------------------------------
 * Command table
 * ------------------------------------------------------------------------- */
static shell_cmd_t cmd_table[SHELL_MAX_CMDS];
static int         cmd_count = 0;

/* -------------------------------------------------------------------------
 * shell_print / shell_println
 * ------------------------------------------------------------------------- */
void shell_print(const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    vprintf(fmt, args);
    va_end(args);
}

void shell_println(const char *s)
{
    printf("%s\r\n", s);
}

/* -------------------------------------------------------------------------
 * shell_register_cmd
 * ------------------------------------------------------------------------- */
int shell_register_cmd(const shell_cmd_t *cmd)
{
    if (cmd == NULL || cmd_count >= (int)SHELL_MAX_CMDS) {
        return -1;
    }
    cmd_table[cmd_count++] = *cmd;
    return 0;
}

/* =========================================================================
 * Built-in command handlers
 * ========================================================================= */

/* --- help ---------------------------------------------------------------- */
static int cmd_help(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    shell_println("Available commands:");
    for (int i = 0; i < cmd_count; i++) {
        shell_print("  %-12s  %s\r\n", cmd_table[i].name, cmd_table[i].help);
    }
    return 0;
}

/* --- ps ------------------------------------------------------------------ */
static int cmd_ps(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    shell_println("PID  NAME             THREADS  ALIVE");
    shell_println("---  ---------------  -------  -----");
    for (int i = 0; i < MAX_PROCESSES; i++) {
        pcb_t *p = task_get_process_slot(i);
        if (p == NULL) { continue; }
        shell_print("%-4u %-16s %-8u %s\r\n",
                    p->pid, p->name, p->thread_count,
                    p->alive ? "yes" : "no");
    }
    return 0;
}

/* --- threads ------------------------------------------------------------- */
/* state_name — return a short human-readable string for a thread state.
 * Used by cmd_threads to format the 'threads' command output table. */
static const char *state_name(thread_state_t s)
{
    switch (s) {
    case THREAD_NEW:      return "NEW";
    case THREAD_READY:    return "READY";
    case THREAD_RUNNING:  return "RUNNING";
    case THREAD_BLOCKED:  return "BLOCKED";
    case THREAD_SLEEPING: return "SLEEPING";
    case THREAD_ZOMBIE:   return "ZOMBIE";
    default:              return "UNKNOWN";
    }
}

static int cmd_threads(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    shell_println("TID  PID  PRI  STATE     CPU-ms  NAME");
    shell_println("---  ---  ---  --------  ------  ---------------");
    for (int i = 0; i < MAX_THREADS; i++) {
        tcb_t *t = task_get_thread_slot(i);
        if (t == NULL) { continue; }
        shell_print("%-4u %-4u %-4u %-9s %-7llu %s\r\n",
                    t->tid, t->pid, (unsigned)t->priority,
                    state_name(t->state),
                    (unsigned long long)(t->cpu_time_us / 1000u),
                    t->name);
    }
    return 0;
}

/* --- kill --------------------------------------------------------------- */
static int cmd_kill(int argc, char **argv)
{
    if (argc < 2) {
        shell_println("Usage: kill <tid>");
        return -1;
    }
    uint32_t tid = (uint32_t)strtol(argv[1], NULL, 10);
    tcb_t *t = task_find_thread(tid);
    if (t == NULL) {
        shell_print("kill: thread %u not found\r\n", tid);
        return -1;
    }
    t->state = THREAD_ZOMBIE;
    shell_print("kill: thread %u (%s) killed\r\n", tid, t->name);
    return 0;
}

/* --- mem ----------------------------------------------------------------- */
static int cmd_mem(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    uint32_t used, free_bytes, largest;
    mem_stats(&used, &free_bytes, &largest);

    shell_print("Kernel heap:\r\n");
    shell_print("  Used        : %u bytes\r\n",  used);
    shell_print("  Free        : %u bytes\r\n",  free_bytes);
    shell_print("  Largest free: %u bytes\r\n",  largest);

    /* Stack canary check for all threads. */
    shell_println("Thread stack canaries:");
    for (int i = 0; i < MAX_THREADS; i++) {
        tcb_t *t = task_get_thread_slot(i);
        if (t == NULL) { continue; }
        uint32_t *canary_ptr = (uint32_t *)t->stack_base;
        bool ok = (*canary_ptr == STACK_CANARY);
        shell_print("  TID %-3u %-14s  canary=%08X  %s\r\n",
                    t->tid, t->name,
                    *canary_ptr,
                    ok ? "OK" : "*** OVERFLOWED ***");
    }
    return 0;
}

/* --- ls ------------------------------------------------------------------ */
static int ls_callback(const fs_entry_t *entry)
{
    shell_print("  %-16s  %u bytes\r\n", entry->name, entry->size);
    return 0;
}

static int cmd_ls(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    shell_println("Name              Size");
    shell_println("----------------  ----");
    fs_list(ls_callback);
    return 0;
}

/* --- cat ----------------------------------------------------------------- */
static int cmd_cat(int argc, char **argv)
{
    if (argc < 2) {
        shell_println("Usage: cat <file>");
        return -1;
    }

    int fd = fs_open(argv[1], VFS_O_RDONLY);
    if (fd < 0) {
        shell_print("cat: cannot open '%s'\r\n", argv[1]);
        return -1;
    }

    uint8_t buf[128];
    int n;
    while ((n = fs_read(fd, buf, sizeof(buf) - 1u)) > 0) {
        buf[n] = '\0';
        shell_print("%s", (char *)buf);
    }
    shell_println("");   /* newline after file content */
    fs_close(fd);
    return 0;
}

/* --- write --------------------------------------------------------------- */
static int cmd_write(int argc, char **argv)
{
    if (argc < 3) {
        shell_println("Usage: write <file> <data>");
        return -1;
    }

    /* Open (create/truncate) the file. */
    int fd = fs_open(argv[1], VFS_O_WRONLY | VFS_O_CREAT | VFS_O_TRUNC);
    if (fd < 0) {
        shell_print("write: cannot open '%s'\r\n", argv[1]);
        return -1;
    }

    /* Join remaining argv tokens into a single string with spaces. */
    char data_buf[SHELL_LINE_MAX];
    data_buf[0] = '\0';
    for (int i = 2; i < argc; i++) {
        if (i > 2) {
            strncat(data_buf, " ", sizeof(data_buf) - strlen(data_buf) - 1u);
        }
        strncat(data_buf, argv[i], sizeof(data_buf) - strlen(data_buf) - 1u);
    }

    uint32_t len = (uint32_t)strlen(data_buf);
    int written = fs_write(fd, (const uint8_t *)data_buf, len);
    fs_close(fd);

    if (written < 0) {
        shell_print("write: failed to write to '%s'\r\n", argv[1]);
        return -1;
    }
    shell_print("write: wrote %d bytes to '%s'\r\n", written, argv[1]);
    return 0;
}

/* --- format -------------------------------------------------------------- */
static int cmd_format(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    shell_println("Formatting filesystem — all files will be erased...");
    fs_format();
    shell_println("Format complete.");
    return 0;
}

/* --- rm ------------------------------------------------------------------ */
static int cmd_rm(int argc, char **argv)
{
    if (argc < 2) {
        shell_println("Usage: rm <file>");
        return -1;
    }
    if (fs_delete(argv[1]) < 0) {
        shell_print("rm: '%s' not found\r\n", argv[1]);
        return -1;
    }
    shell_print("rm: deleted '%s'\r\n", argv[1]);
    return 0;
}

/* --- reboot -------------------------------------------------------------- */
static int cmd_reboot(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    shell_println("Rebooting...");
    stdio_flush();
    watchdog_reboot(0u, 0u, 0u);
    /* Not reached. */
    for (;;) {}
    return 0;
}

/* --- update -------------------------------------------------------------- */
static int cmd_update(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    shell_println("Rebooting into USB BOOTSEL mode...");
    stdio_flush();
    reset_usb_boot(0u, 0u);
    /* Not reached. */
    for (;;) {}
    return 0;
}

/* --- trace --------------------------------------------------------------- */
static int cmd_trace(int argc, char **argv)
{
    if (argc < 2) {
        shell_print("trace: %s\r\n", trace_enabled ? "on" : "off");
        return 0;
    }
    if (strcmp(argv[1], "on") == 0) {
        trace_enabled = true;
        shell_println("Tracing enabled.");
    } else if (strcmp(argv[1], "off") == 0) {
        trace_enabled = false;
        shell_println("Tracing disabled.");
    } else {
        shell_println("Usage: trace <on|off>");
        return -1;
    }
    return 0;
}

/* --- run ----------------------------------------------------------------- */
static int cmd_run(int argc, char **argv)
{
    if (argc < 2) {
        shell_println("Usage: run <appname>");
        shell_println("Available apps:");
        for (int i = 0; i < app_table_size; i++) {
            shell_print("  %s\r\n", app_table[i].name);
        }
        return 0;
    }

    const char *appname = argv[1];
    for (int i = 0; i < app_table_size; i++) {
        if (strcmp(app_table[i].name, appname) == 0) {
            /* Assign a new PID from the 100+ range for user apps. */
            static uint32_t user_pid = 100u;
            uint32_t pid = user_pid++;

            pcb_t *proc = task_create_process(appname, pid);
            if (proc == NULL) {
                shell_println("run: cannot create process (pool full)");
                return -1;
            }
            tcb_t *t = task_create_thread(proc, appname,
                                          app_table[i].entry, NULL,
                                          app_table[i].priority,
                                          DEFAULT_STACK_SIZE);
            if (t == NULL) {
                shell_println("run: cannot create thread (pool full)");
                task_free_process(proc);
                return -1;
            }
            shell_print("run: started '%s' as PID %u TID %u\r\n",
                        appname, pid, t->tid);
            return 0;
        }
    }

    shell_print("run: app '%s' not found\r\n", appname);
    return -1;
}

/* =========================================================================
 * Built-in command table
 * ========================================================================= */
/* --- info ----------------------------------------------------------------- */
static int cmd_info(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    shell_print("\r\bpicoOS v%u.%u.%u\r\n\n",
                PICOOS_VERSION_MAJOR, PICOOS_VERSION_MINOR, PICOOS_VERSION_EDIT);
    shell_print("Build     : %s %s\r\n", __DATE__, __TIME__);
    shell_print("Compiler  : %s\r\n",    __VERSION__);
    shell_print("Platform  : RP2040, dual ARM Cortex-M0+ (133 MHz max)\r\n");
    shell_print("SRAM      : 264 KB\r\n");
    shell_print("Flash     : 2 MB QSPI (XIP)\r\n");
#ifdef PICOOS_DISPLAY_ENABLE
    shell_print("Display   : %ux%u RGB332 ST7789\r\n", DISP_WIDTH, DISP_HEIGHT);
#endif
    return 0;
}

/* builtin_cmds — compile-time command table for all built-in shell commands.
 * Registered into cmd_table[] by shell_init() via shell_register_cmd().
 * Additional commands can be added at runtime from any module by calling
 * shell_register_cmd() directly — no changes to this table are required. */
static const shell_cmd_t builtin_cmds[] = {
    { "info",    "Show system information and version",         cmd_info    },
    { "help",    "List all commands",                          cmd_help    },
    { "ps",      "Show processes",                             cmd_ps      },
    { "threads", "Show all threads with state and CPU time",   cmd_threads },
    { "kill",    "kill <tid>  — kill a thread",                cmd_kill    },
    { "mem",     "Show heap usage and stack canary status",    cmd_mem     },
    { "ls",      "List filesystem contents",                   cmd_ls      },
    { "cat",     "cat <file>  — print file contents",          cmd_cat     },
    { "write",   "write <file> <data>  — write data to file",  cmd_write   },
    { "rm",      "rm <file>  — delete a file",                 cmd_rm      },
    { "format",  "Erase all files and reset the filesystem",   cmd_format  },
    { "reboot",  "Reboot the system",                          cmd_reboot  },
    { "update",  "Reboot into USB BOOTSEL mode",               cmd_update  },
    { "trace",   "trace <on|off>  — toggle scheduler tracing", cmd_trace   },
    { "run",     "run <appname>  — spawn an app thread",       cmd_run     },
};

#define BUILTIN_CMD_COUNT ((int)(sizeof(builtin_cmds) / sizeof(builtin_cmds[0])))

/* =========================================================================
 * shell_init
 * ========================================================================= */
void shell_init(void)
{
    for (int i = 0; i < BUILTIN_CMD_COUNT; i++) {
        shell_register_cmd(&builtin_cmds[i]);
    }
}

/* =========================================================================
 * shell_run — main interactive loop
 * ========================================================================= */

/* Tokenise a NUL-terminated line in-place.
 * Writes NUL bytes at whitespace boundaries.
 * argv[] is populated with pointers to tokens.
 * Returns the number of tokens found.                            */
/* tokenise — split a NUL-terminated line into whitespace-delimited tokens.
 *
 * Modifies line in-place by replacing the first whitespace byte after each
 * token with '\0'.  argv[] is populated with pointers into line; no heap
 * allocation is performed.
 *
 * Parameters:
 *   line      — the input string; modified in-place.
 *   argv      — output array; must hold at least max_argc pointers.
 *   max_argc  — maximum number of tokens to extract.
 *
 * Returns the number of tokens found (0 if line is empty or all whitespace).
 * Tokens beyond max_argc are silently ignored. */
static int tokenise(char *line, char **argv, int max_argc)
{
    int argc = 0;
    char *p = line;

    while (*p != '\0' && argc < max_argc) {
        /* Skip leading spaces. */
        while (*p == ' ' || *p == '\t') {
            p++;
        }
        if (*p == '\0') {
            break;
        }
        /* Start of a token. */
        argv[argc++] = p;
        /* Advance to end of token. */
        while (*p != '\0' && *p != ' ' && *p != '\t') {
            p++;
        }
        if (*p != '\0') {
            *p = '\0';
            p++;
        }
    }
    return argc;
}

void shell_run(void)
{
    char     line[SHELL_LINE_MAX];
    char    *argv[SHELL_ARGC_MAX];
    uint32_t line_len = 0u;

    shell_print("\r\npicoOS shell ready.  Type 'help' for commands.\r\n");

    for (;;) {
        /* Print prompt. */
        shell_print("pico> ");

        /* Read a line character by character. */
        line_len = 0u;
        bool line_done = false;

        while (!line_done) {
            /* Non-blocking poll: sleep 1 ms between attempts so the shell
             * thread is removed from the ready queue while idle, giving
             * lower-priority threads (e.g. sensor, demo apps) a chance
             * to run.  getchar() busy-polls and would starve them. */
            int ch = PICO_ERROR_TIMEOUT;
            while (ch == PICO_ERROR_TIMEOUT) {
                ch = getchar_timeout_us(0);
                if (ch == PICO_ERROR_TIMEOUT) {
                    sys_sleep(1);
                }
            }

            if (ch == '\r' || ch == '\n') {
                /* Enter pressed — process the line. */
                shell_print("\r\n");
                line[line_len] = '\0';
                line_done = true;
            } else if ((ch == '\b' || ch == 0x7F) && line_len > 0u) {
                /* Backspace: erase last character on the terminal. */
                line_len--;
                shell_print("\b \b");
            } else if (ch >= 0x20 && ch < 0x7F && line_len < SHELL_LINE_MAX - 1u) {
                /* Printable character. */
                line[line_len++] = (char)ch;
                putchar_raw(ch);   /* local echo */
            }
            /* Ignore all other control characters. */
        }

        /* Skip empty lines. */
        if (line_len == 0u) {
            continue;
        }

        /* Tokenise. */
        int argc = tokenise(line, argv, (int)SHELL_ARGC_MAX);
        if (argc == 0) {
            continue;
        }

        /* Find and invoke the matching command. */
        bool found = false;
        for (int i = 0; i < cmd_count; i++) {
            if (strcmp(cmd_table[i].name, argv[0]) == 0) {
                int rc = cmd_table[i].handler(argc, argv);
                if (rc != 0) {
                    shell_print("Command returned error %d\r\n", rc);
                }
                found = true;
                break;
            }
        }

        if (!found) {
            shell_print("Unknown command: '%s'  (type 'help')\r\n", argv[0]);
        }
    }
}
