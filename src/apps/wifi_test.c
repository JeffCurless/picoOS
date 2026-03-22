#ifdef PICOOS_WIFI_ENABLE

#include "kernel/wifi.h"
#include "kernel/vfs.h"
#include "shell/shell.h"
#include <string.h>
#include <stddef.h>

#define CONFIG_FILE  "config.txt"
#define CONFIG_BUFSZ 256
#define CRED_MAXLEN   64

/* ---- parse_config --------------------------------------------------------
 *
 * Scans a NUL-terminated text buffer line-by-line looking for:
 *   SSID=<value>
 *   PASSWORD=<value>
 *
 * Values are written into ssid/password (each sized CRED_MAXLEN).
 * Trailing CR/LF is stripped from values.
 * ------------------------------------------------------------------------- */
static void parse_config(const char *buf,
                          char *ssid,     size_t ssid_sz,
                          char *password, size_t pass_sz)
{
    const char *p = buf;
    while (*p) {
        const char *eol = p;
        while (*eol && *eol != '\n' && *eol != '\r') eol++;

        if (strncmp(p, "SSID=", 5) == 0) {
            size_t len = (size_t)(eol - (p + 5));
            if (len >= ssid_sz) len = ssid_sz - 1;
            memcpy(ssid, p + 5, len);
            ssid[len] = '\0';
        } else if (strncmp(p, "PASSWORD=", 9) == 0) {
            size_t len = (size_t)(eol - (p + 9));
            if (len >= pass_sz) len = pass_sz - 1;
            memcpy(password, p + 9, len);
            password[len] = '\0';
        }

        p = eol;
        while (*p == '\n' || *p == '\r') p++;
    }
}

/* ---- wifi_test -----------------------------------------------------------
 *
 * Entry point registered in app_table[].  Run via: run wifi-test
 *
 * 1. Opens config.txt from the filesystem.
 * 2. Parses SSID= and PASSWORD= lines.
 * 3. Connects to the network (up to 10 s).
 * 4. Prints the assigned IP address on success.
 * ------------------------------------------------------------------------- */
void wifi_test(void *arg)
{
    (void)arg;

    /* ---- 1. Open config.txt ---- */
    int fd = vfs_open(CONFIG_FILE, VFS_O_RDONLY);
    if (fd < 0) {
        shell_print("[wifi-test] ERROR: config.txt not found\r\n");
        shell_print("[wifi-test] Create it with: write config.txt\r\n");
        shell_print("[wifi-test]   SSID=<network>\r\n");
        shell_print("[wifi-test]   PASSWORD=<password>\r\n");
        return;
    }

    /* ---- 2. Read and parse ---- */
    char buf[CONFIG_BUFSZ];
    int n = vfs_read(fd, (uint8_t *)buf, sizeof(buf) - 1);
    vfs_close(fd);

    if (n <= 0) {
        shell_print("[wifi-test] ERROR: config.txt is empty or unreadable\r\n");
        return;
    }
    buf[n] = '\0';

    char ssid[CRED_MAXLEN]     = {0};
    char password[CRED_MAXLEN] = {0};
    parse_config(buf, ssid, sizeof(ssid), password, sizeof(password));

    if (ssid[0] == '\0') {
        shell_print("[wifi-test] ERROR: SSID= not found in config.txt\r\n");
        return;
    }

    shell_print("[wifi-test] SSID     : %s\r\n", ssid);
    shell_print("[wifi-test] Password : %s\r\n",
                password[0] ? "(set)" : "(none — open network)");

    /* ---- 3. Connect ---- */
    shell_print("[wifi-test] Connecting...\r\n");
    int rc = wifi_connect(ssid, password);
    if (rc != 0) {
        shell_print("[wifi-test] Connection failed (err %d)\r\n", rc);
        return;
    }

    /* ---- 4. Report IP ---- */
    shell_print("[wifi-test] Connected\r\n");
    shell_print("[wifi-test] IP address: %s\r\n", wifi_get_ip_str());
}

#endif /* PICOOS_WIFI_ENABLE */
