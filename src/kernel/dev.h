#ifndef KERNEL_DEV_H
#define KERNEL_DEV_H

#include <stdint.h>

/* -------------------------------------------------------------------------
 * Device IDs
 * ------------------------------------------------------------------------- */
typedef enum {
    DEV_CONSOLE = 0,
    DEV_TIMER,
    DEV_FLASH,
    DEV_GPIO,
    DEV_COUNT           /* sentinel — always last */
} dev_id_t;

/* -------------------------------------------------------------------------
 * ioctl command codes
 * ------------------------------------------------------------------------- */
#define IOCTL_TIMER_GET_TICK    0x0100u   /* arg: uint32_t * — fills tick_count  */
#define IOCTL_TIMER_GET_US      0x0101u   /* arg: uint64_t * — fills time_us_64  */
#define IOCTL_GPIO_SET_DIR      0x0200u   /* arg: uint32_t pin | (dir << 16)     */
#define IOCTL_GPIO_SET_VAL      0x0201u   /* arg: uint32_t pin | (val << 16)     */
#define IOCTL_GPIO_GET_VAL      0x0202u   /* arg: uint32_t * — fills pin value   */

/* -------------------------------------------------------------------------
 * Device descriptor
 * ------------------------------------------------------------------------- */
typedef struct {
    dev_id_t    id;
    const char *name;
    int       (*open)(void);
    int       (*read)(uint8_t *buf, uint32_t len);
    int       (*write)(const uint8_t *buf, uint32_t len);
    int       (*ioctl)(uint32_t cmd, void *arg);
    void      (*close)(void);
} device_t;

/* -------------------------------------------------------------------------
 * Device manager API
 * ------------------------------------------------------------------------- */

void      dev_init(void);
device_t *dev_get(dev_id_t id);
int       dev_open(dev_id_t id);
int       dev_read(dev_id_t id, uint8_t *buf, uint32_t len);
int       dev_write(dev_id_t id, const uint8_t *buf, uint32_t len);
int       dev_ioctl(dev_id_t id, uint32_t cmd, void *arg);
void      dev_close(dev_id_t id);

#endif /* KERNEL_DEV_H */
