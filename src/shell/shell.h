#ifndef SHELL_H
#define SHELL_H

#include <stdint.h>
#include <stdarg.h>

/* -------------------------------------------------------------------------
 * Shell constants
 * ------------------------------------------------------------------------- */
#define SHELL_LINE_MAX   128u
#define SHELL_ARGC_MAX   8u
#define SHELL_MAX_CMDS   32u

/* -------------------------------------------------------------------------
 * Command descriptor
 * ------------------------------------------------------------------------- */
typedef struct {
    const char *name;
    const char *help;
    int (*handler)(int argc, char **argv);
} shell_cmd_t;

/* -------------------------------------------------------------------------
 * Shell API
 * ------------------------------------------------------------------------- */

/*
 * shell_init — register all built-in commands.
 *              Must be called once before shell_run().
 */
void shell_init(void);

/*
 * shell_run — main interactive shell loop.
 *             Reads lines from stdin (USB CDC), tokenises them, and
 *             dispatches to the matching command handler.
 *             This function never returns.
 */
void shell_run(void);

/*
 * shell_register_cmd — add a command to the command table.
 *                      Returns 0 on success, -1 if the table is full.
 */
int shell_register_cmd(const shell_cmd_t *cmd);

/*
 * shell_print — printf-style formatted output to the USB console.
 */
void shell_print(const char *fmt, ...);

/*
 * shell_println — print a string followed by CRLF.
 */
void shell_println(const char *s);

#endif /* SHELL_H */
