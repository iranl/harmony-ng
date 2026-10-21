/*
 * cc2544.c - TI CC2544 RF transceiver driver for Logitech Harmony Hub (Atheros AR7240/AR9331)
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; version 2 of the License.
 *
 * Features:
 *  - /dev/rffw  : Firmware flash programming interface (TI CC debug protocol)
 *  - /dev/rfspi : SPI communication interface (fixed write return value + relaxed RX packet filter)
 *  - ioctl support for hardware reset and GPIO status inquiry
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/fs.h>
#include <linux/miscdevice.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/delay.h>
#include <linux/interrupt.h>
#include <linux/workqueue.h>
#include <linux/wait.h>
#include <linux/sched.h>
#include <linux/list.h>
#include <linux/jiffies.h>
#include <linux/ioctl.h>

#define DRV_NAME "cc2544"

/* Hardware Register Definitions - Atheros AR7240 / AR9331 */
#define AR7240_GPIO_OE_REG      (*(volatile u32 *)0xb8040000)
#define AR7240_GPIO_IN_REG      (*(volatile u32 *)0xb8040004)
#define AR7240_GPIO_OUT_REG     (*(volatile u32 *)0xb8040008)

#define AR7240_SPI_CTRL_REG     (*(volatile u32 *)0xbf000000)
#define AR7240_SPI_CLK_REG      (*(volatile u32 *)0xbf000004)
#define AR7240_SPI_DATA_OUT_REG (*(volatile u32 *)0xbf000008)
#define AR7240_SPI_DATA_IN_REG  (*(volatile u32 *)0xbf00000c)

/* GPIO Pin Assignments */
#define GPIO_CC2544_RESET       15  /* Output: Active Low Reset */
#define GPIO_CC2544_DC_BUSY      6  /* In Run mode: Busy input. In Debug mode: Debug Clock */
#define GPIO_CC2544_DD_IRQ      14  /* In Run mode: IRQ 46 input. In Debug mode: Debug Data */
#define GPIO_CC2544_SPI_CS       8  /* Output: Active Low SPI Chip Select */

#define CC2544_IRQ_NUM          46

/* ioctl commands for /dev/rfspi */
#define CC2544_IOC_MAGIC        'C'
#define CC2544_IOC_RESET        _IO(CC2544_IOC_MAGIC, 1)
#define CC2544_IOC_STATUS       _IOR(CC2544_IOC_MAGIC, 2, int)

/* External platform functions for SPI Flash locking */
extern void ar7240_flash_spi_down(void);
extern void ar7240_flash_spi_up(void);
extern void ar7240_gpio_config_int(int gpio, int type, int pol);

/* Queue entry for incoming SPI packets */
struct cc2544_packet {
    u8 data[64];
    int len;
    struct list_head list;
};

static LIST_HEAD(rf_data_head);
static wait_queue_head_t cc2544_spi_read_wq;
static struct work_struct cc2544_spi_work;
static int rfspi_open = 0;
static u32 saved_spi_clock_reg = 0;

/* ========================================================================= */
/* Atheros GPIO helpers                                                      */
/* ========================================================================= */

static void ar7240_gpio_config_output(int gpio)
{
    AR7240_GPIO_OE_REG |= (1U << gpio);
    (void)AR7240_GPIO_OE_REG;
}

static void ar7240_gpio_config_input(int gpio)
{
    AR7240_GPIO_OE_REG &= ~(1U << gpio);
    (void)AR7240_GPIO_OE_REG;
}

static void ar7240_gpio_out_val(int gpio, int val)
{
    if (val & 1)
        AR7240_GPIO_OUT_REG |= (1U << gpio);
    else
        AR7240_GPIO_OUT_REG &= ~(1U << gpio);
    (void)AR7240_GPIO_OUT_REG;
}

static int ar7240_gpio_in_val(int gpio)
{
    return (AR7240_GPIO_IN_REG & (1U << gpio)) ? 1 : 0;
}

/* ========================================================================= */
/* TI CC-series 2-Wire Debug Interface (flashing & reset)                    */
/* ========================================================================= */

int cc2544_reset(void)
{
    int i;
    ar7240_gpio_out_val(GPIO_CC2544_RESET, 0);
    for (i = 0; i < 20; i++)
        udelay(1000);
    ar7240_gpio_out_val(GPIO_CC2544_RESET, 1);
    return 0;
}

static int cc2544_debug_init(void)
{
    int i;
    ar7240_gpio_out_val(GPIO_CC2544_RESET, 1);
    for (i = 0; i < 20; i++)
        udelay(1000);
    ar7240_gpio_out_val(GPIO_CC2544_RESET, 0);
    for (i = 0; i < 10; i++)
        udelay(1000);

    /* Clock pulse 1 on DD/DC */
    ar7240_gpio_out_val(GPIO_CC2544_DD_IRQ, 1);
    ndelay(20);
    ar7240_gpio_out_val(GPIO_CC2544_DD_IRQ, 0);
    ndelay(20);

    /* Clock pulse 2 on DD/DC */
    ar7240_gpio_out_val(GPIO_CC2544_DD_IRQ, 1);
    ndelay(20);
    ar7240_gpio_out_val(GPIO_CC2544_DD_IRQ, 0);
    ndelay(20);

    for (i = 0; i < 10; i++)
        udelay(1000);
    ar7240_gpio_out_val(GPIO_CC2544_RESET, 1);
    return 0;
}

static void cc2544_debug_out(int byte)
{
    int bit;
    udelay(2);
    ar7240_gpio_config_output(GPIO_CC2544_DC_BUSY);
    for (bit = 7; bit >= 0; bit--) {
        ar7240_gpio_out_val(GPIO_CC2544_DD_IRQ, 1);
        ndelay(20);
        ar7240_gpio_out_val(GPIO_CC2544_DC_BUSY, (byte >> bit) & 1);
        ar7240_gpio_out_val(GPIO_CC2544_DD_IRQ, 0);
        ndelay(20);
    }
}

static u32 cc2544_debug_in(void)
{
    int bit;
    u32 val = 0;
    for (bit = 0; bit < 8; bit++) {
        ar7240_gpio_out_val(GPIO_CC2544_DD_IRQ, 1);
        ndelay(20);
        val = (val << 1) | (ar7240_gpio_in_val(GPIO_CC2544_DC_BUSY) != 0);
        ar7240_gpio_out_val(GPIO_CC2544_DD_IRQ, 0);
        ndelay(20);
    }
    return val;
}

static void cc2544_wait_response(void)
{
    ar7240_gpio_config_input(GPIO_CC2544_DC_BUSY);
    udelay(2);
    if (ar7240_gpio_in_val(GPIO_CC2544_DC_BUSY)) {
        cc2544_debug_in();
        udelay(2);
    }
}

static u32 cc2544_debug_instr1(u32 instr)
{
    cc2544_debug_out(0x55);
    cc2544_debug_out(instr);
    cc2544_wait_response();
    return cc2544_debug_in();
}

static u32 cc2544_resume(void)
{
    cc2544_debug_out(0x4c);
    cc2544_wait_response();
    return cc2544_debug_in();
}

static u32 cc2544_read_status(void)
{
    cc2544_debug_out(0x34);
    cc2544_wait_response();
    return cc2544_debug_in();
}

static u32 cc2544_debug_instr3(u32 b1, u32 b2, u32 b3)
{
    cc2544_debug_out(0x57);
    cc2544_debug_out(b1);
    cc2544_debug_out(b2);
    cc2544_debug_out(b3);
    cc2544_wait_response();
    return cc2544_debug_in();
}

static u32 cc2544_set_pc(u32 addr)
{
    /* LJMP addr (0x02 <addr_hi> <addr_lo>) */
    return cc2544_debug_instr3(0x02, (addr >> 8) & 0xff, addr & 0xff);
}

static int cc2544_read_flash_page(u32 addr, u8 *buf)
{
    int i;
    cc2544_debug_instr3(0x75, 199, 0); /* MOV MEMCTR, #0 */
    cc2544_debug_instr3(0x90, (addr >> 8) & 0xff, addr & 0xff); /* MOV DPTR, #addr */
    for (i = 0; i < 0x400; i++) {
        cc2544_debug_instr1(0xe4); /* CLR A */
        buf[i] = (u8)cc2544_debug_instr1(0x93); /* MOVC A, @A+DPTR */
        cc2544_debug_instr1(0xa3); /* INC DPTR */
    }
    return 0;
}

static void cc2544_write_xdata_memory(u32 addr, u32 len, const u8 *buf)
{
    u32 i;
    cc2544_debug_instr3(0x90, (addr >> 8) & 0xff, addr & 0xff); /* MOV DPTR, #addr */
    for (i = 0; i < len; i++) {
        cc2544_debug_out(0x56);
        cc2544_debug_out(0x74);
        cc2544_debug_out(buf[i]);
        cc2544_wait_response();
        cc2544_debug_in();
        cc2544_debug_instr1(0xf0); /* MOVX @DPTR, A */
        cc2544_debug_instr1(0xa3); /* INC DPTR */
    }
}

static int cc2544_erase_flash_page(u32 addr)
{
    u8 stub[29];
    int timeout = 0;

    memset(stub, 0, sizeof(stub));
    stub[0] = 0x74; stub[1] = 0x00; stub[2] = 0x90; stub[3] = 0x62; stub[4] = 0x71; stub[5] = 0xf0;
    stub[6] = 0x74; stub[7] = (u8)(addr >> 10); stub[8] = 0x90; stub[9] = 0x62; stub[10] = 0x72; stub[11] = 0xf0;
    stub[12] = 0x90; stub[13] = 0x62; stub[14] = 0x70; stub[15] = 0xe0; stub[16] = 0xd2; stub[17] = 0x00;
    stub[18] = 0xe0; stub[19] = 0xf0; stub[20] = 0x90; stub[21] = 0x62; stub[22] = 0x70; stub[23] = 0xe0;
    stub[24] = 0xa2; stub[25] = 0xe7; stub[26] = 0x40; stub[27] = 0xf8; stub[28] = 0xa5;

    cc2544_write_xdata_memory(0, sizeof(stub), stub);
    cc2544_set_pc(0x8000);
    cc2544_debug_instr3(0x75, 199, 8); /* MEMCTR = 8 */
    cc2544_resume();

    while (++timeout <= 100) {
        u32 status = cc2544_read_status();
        if (status & 0x20) {
            cc2544_debug_instr3(0x75, 199, 0);
            return 0;
        }
        udelay(1000);
    }
    printk(KERN_ERR "cc2544_erase_flash_page timeout %d\n", timeout);
    cc2544_debug_instr3(0x75, 199, 0);
    return 0;
}

static int cc2544_program_page(u32 page_base, u32 offset, u8 *scratch)
{
    u8 *src_data = (u8 *)(page_base + offset);
    int timeout = 0;
    u8 prog_stub[116];

    memset(scratch, 0xff, 0x400);
    if (memcmp(scratch, src_data, 0x400) == 0)
        return 0; /* All 0xFF, skip writing */

    /* 8051 Flash programming routine */
    memset(prog_stub, 0, sizeof(prog_stub));
    prog_stub[0]  = 0x74; prog_stub[1]  = 0x00; prog_stub[2]  = 0x90; prog_stub[3]  = 0x62; prog_stub[4]  = 0x71; prog_stub[5]  = 0xf0;
    prog_stub[6]  = 0x74; prog_stub[7]  = (u8)(offset >> 10); prog_stub[8] = 0x90; prog_stub[9] = 0x62; prog_stub[10] = 0x72; prog_stub[11] = 0xf0;
    prog_stub[12] = 0x90; prog_stub[13] = 0x62; prog_stub[14] = 0x70; prog_stub[15] = 0xe0; prog_stub[16] = 0xd2; prog_stub[17] = 0xe1;
    prog_stub[18] = 0x78; prog_stub[19] = 0x00; prog_stub[20] = 0x79; prog_stub[21] = 0x00; prog_stub[22] = 0xc3; prog_stub[23] = 0xe8;
    prog_stub[24] = 0x94; prog_stub[25] = 0x00; prog_stub[26] = 0xe9; prog_stub[27] = 0x94; prog_stub[28] = 0x04; prog_stub[29] = 0xa2;
    prog_stub[30] = 0xd2; prog_stub[31] = 0x65; prog_stub[32] = 0xd0; prog_stub[33] = 0x33; prog_stub[34] = 0x50; prog_stub[35] = 0x4e;
    prog_stub[36] = 0x74; prog_stub[37] = 0x00; prog_stub[38] = 0x28; prog_stub[39] = 0xf5; prog_stub[40] = 0x82; prog_stub[41] = 0x74;
    prog_stub[42] = 0x00; prog_stub[43] = 0x39; prog_stub[44] = 0xf5; prog_stub[45] = 0x83; prog_stub[46] = 0xe0; prog_stub[47] = 0x73;
    prog_stub[48] = 0x90; prog_stub[49] = 0x62; prog_stub[50] = 0x73; prog_stub[51] = 0xf0; prog_stub[52] = 0x74; prog_stub[53] = 0x01;
    prog_stub[54] = 0x28; prog_stub[55] = 0xf5; prog_stub[56] = 0x82; prog_stub[57] = 0x74; prog_stub[58] = 0x00; prog_stub[59] = 0x39;
    prog_stub[60] = 0xf5; prog_stub[61] = 0x83; prog_stub[62] = 0xe0; prog_stub[63] = 0x73; prog_stub[64] = 0x90; prog_stub[65] = 0x62;
    prog_stub[66] = 0x73; prog_stub[67] = 0xf0; prog_stub[68] = 0x74; prog_stub[69] = 0x02; prog_stub[70] = 0x28; prog_stub[71] = 0xf5;
    prog_stub[72] = 0x82; prog_stub[73] = 0x74; prog_stub[74] = 0x00; prog_stub[75] = 0x39; prog_stub[76] = 0xf5; prog_stub[77] = 0x83;
    prog_stub[78] = 0xe0; prog_stub[79] = 0x73; prog_stub[80] = 0x90; prog_stub[81] = 0x62; prog_stub[82] = 0x73; prog_stub[83] = 0xf0;
    prog_stub[84] = 0x74; prog_stub[85] = 0x03; prog_stub[86] = 0x28; prog_stub[87] = 0xf5; prog_stub[88] = 0x82; prog_stub[89] = 0x74;
    prog_stub[90] = 0x00; prog_stub[91] = 0x39; prog_stub[92] = 0xf5; prog_stub[93] = 0x83; prog_stub[94] = 0xe0; prog_stub[95] = 0x73;
    prog_stub[96] = 0x90; prog_stub[97] = 0x62; prog_stub[98] = 0x73; prog_stub[99] = 0xf0; prog_stub[100] = 0x90; prog_stub[101] = 0x62;
    prog_stub[102] = 0x70; prog_stub[103] = 0xe0; prog_stub[104] = 0xa2; prog_stub[105] = 0xe6; prog_stub[106] = 0x40; prog_stub[107] = 0xf8;
    prog_stub[108] = 0xe8; prog_stub[109] = 0x24; prog_stub[110] = 0x04; prog_stub[111] = 0xf8; prog_stub[112] = 0xe9; prog_stub[113] = 0x34;
    prog_stub[114] = 0x00; prog_stub[115] = 0xa5;

    cc2544_write_xdata_memory(0, 0x400, src_data);
    cc2544_write_xdata_memory(0x400, sizeof(prog_stub), prog_stub);
    cc2544_set_pc(0x8400);
    cc2544_debug_instr3(0x75, 199, 8); /* MEMCTR = 8 */
    cc2544_resume();

    while (++timeout <= 100) {
        u32 status = cc2544_read_status();
        if (status & 0x20)
            goto done;
        udelay(100);
    }
    printk(KERN_ERR "cc2544_write_flash_page timeout\n");

done:
    cc2544_debug_instr3(0x75, 199, 0);
    cc2544_read_flash_page(offset, scratch);
    if (memcmp(scratch, src_data, 0x400) != 0) {
        printk(KERN_ERR "verification error, page %08x\n", offset);
        return -1;
    }
    return 0;
}

/* ========================================================================= */
/* /dev/rffw File Operations (firmware programming device)                   */
/* ========================================================================= */

static int cc2544_fw_open(struct inode *inode, struct file *file)
{
    u8 *buf = kmalloc(0x8000, GFP_KERNEL);
    if (!buf)
        return -ENOMEM;
    file->private_data = buf;
    memset(buf, 0xff, 0x8000);

    /* If opened read-only, read current CC2544 flash contents into buffer */
    if ((file->f_flags & O_ACCMODE) == O_RDONLY) {
        int page;
        cc2544_debug_init();
        for (page = 0; page < 0x7000; page += 0x400) {
            cc2544_read_flash_page(page, buf + page);
            schedule();
        }
    }
    return 0;
}

static ssize_t cc2544_fw_write(struct file *file, const char __user *buf, size_t count, loff_t *ppos)
{
    u8 *fw_buf = file->private_data;
    if (*ppos + count > 0x8000)
        count = 0x8000 - *ppos;

    if (copy_from_user(fw_buf + *ppos, buf, count))
        return -EFAULT;

    *ppos += count;
    return count;
}

static ssize_t cc2544_fw_read(struct file *file, char __user *buf, size_t count, loff_t *ppos)
{
    u8 *fw_buf = file->private_data;
    if (*ppos + count > 0x8000)
        count = 0x8000 - *ppos;

    if (copy_to_user(buf, fw_buf + *ppos, count))
        return -EFAULT;

    *ppos += count;
    return count;
}

static int cc2544_fw_release(struct inode *inode, struct file *file)
{
    u8 *fw_buf = file->private_data;

    ar7240_gpio_config_output(GPIO_CC2544_DD_IRQ);
    ar7240_gpio_config_output(GPIO_CC2544_DC_BUSY);
    cc2544_debug_init();

    if ((file->f_flags & O_ACCMODE) == O_WRONLY) {
        if (strncmp((char *)fw_buf, "erase", 5) == 0) {
            printk(KERN_INFO "cc2544 chip erase\n");
            cc2544_debug_out(0x14);
            cc2544_wait_response();
            cc2544_debug_in();
        } else {
            u8 *scratch = kmalloc(0x400, GFP_KERNEL);
            int page;

            ar7240_gpio_config_output(GPIO_CC2544_RESET);
            disable_irq(CC2544_IRQ_NUM);
            cc2544_debug_init();

            for (page = 0; page < 0x7000; page += 0x400)
                cc2544_erase_flash_page(page);

            for (page = 0x400; page < 0x7000; page += 0x400) {
                cc2544_program_page((u32)fw_buf, page, scratch);
                schedule();
            }
            cc2544_program_page((u32)fw_buf, 0, scratch);

            kfree(scratch);
            enable_irq(CC2544_IRQ_NUM);
            printk(KERN_INFO "cc2544: flash programmed\n");
        }
    }

    cc2544_reset();
    ar7240_gpio_config_input(GPIO_CC2544_DD_IRQ);
    ar7240_gpio_config_input(GPIO_CC2544_DC_BUSY);
    kfree(fw_buf);
    return 0;
}

static const struct file_operations cc2544_fw_fops = {
    .owner   = THIS_MODULE,
    .open    = cc2544_fw_open,
    .read    = cc2544_fw_read,
    .write   = cc2544_fw_write,
    .release = cc2544_fw_release,
};

static struct miscdevice cc2544_fw_miscdevice = {
    .minor = MISC_DYNAMIC_MINOR,
    .name  = "cc2544_fw",
    .fops  = &cc2544_fw_fops,
};

/* ========================================================================= */
/* Atheros AR7240 Hardware SPI Driver for CC2544                             */
/* ========================================================================= */

static u8 cc2544_spi_raw_output_u8(u8 byte)
{
    u32 cur_data = AR7240_SPI_DATA_OUT_REG & ~0x101;
    int bit;

    for (bit = 7; bit >= 0; bit--) {
        u32 out_val = cur_data | ((byte >> bit) & 1);
        AR7240_SPI_DATA_OUT_REG = out_val;
        AR7240_SPI_DATA_OUT_REG = out_val | 0x100; /* clock pulse */
    }
    return (u8)(AR7240_SPI_DATA_IN_REG & 0xff);
}

/*
 * cc2544_spi_readwrite:
 * Performs full-duplex SPI transfer with the CC2544.
 * Returns: number of received bytes enqueued (or negative on error).
 */
static int cc2544_spi_readwrite(const u8 *tx_buf, int tx_len)
{
    unsigned long timeout = jiffies + 250;
    struct cc2544_packet *pkt;
    u8 first_rx;
    int rx_len = 0;
    int transfer_len;
    int i;

    /* Wait for CC2544 Busy line to go Low (ready) */
    while (ar7240_gpio_in_val(GPIO_CC2544_DC_BUSY) != 0 && time_before(jiffies, timeout))
        schedule();

    if (ar7240_gpio_in_val(GPIO_CC2544_DC_BUSY) != 0)
        return -EIO;

    pkt = kmalloc(sizeof(*pkt), GFP_ATOMIC);
    if (!pkt)
        return -ENOMEM;

    /* Acquire Flash SPI lock and set SPI clock divider */
    ar7240_flash_spi_down();
    AR7240_SPI_CTRL_REG = 1;
    saved_spi_clock_reg = AR7240_SPI_CLK_REG;
    AR7240_SPI_CLK_REG = 99;

    /* Assert SPI CS (GPIO 8 = Low) */
    ar7240_gpio_out_val(GPIO_CC2544_SPI_CS, 0);
    udelay(20);

    /* Clock byte 0 */
    first_rx = cc2544_spi_raw_output_u8(tx_buf ? tx_buf[0] : 0);

    /* Determine incoming packet length from first byte */
    switch (first_rx) {
    case 0x00:
        rx_len = 0;
        break;
    case 0x01:
    case 0x02:
        rx_len = 7;
        break;
    case 0x03:
        rx_len = 4;
        break;
    case 0x04:
    case 0x08:
        rx_len = 1;
        break;
    case 0x10:
        rx_len = 7;   /* HID++ 1.0 Short Report */
        break;
    case 0x11:
        rx_len = 20;  /* HID++ 2.0 Long Report */
        break;
    case 0x12:
        rx_len = 30;
        break;
    case 0x20:
        rx_len = 32;  /* HID++ Very Long / eQuad Report */
        break;
    default:
        /*
         * FIXED: Instead of dropping unknown packet headers,
         * default to 32 bytes to allow arbitrary RF packets!
         */
        rx_len = 32;
        break;
    }

    transfer_len = max(rx_len, tx_len);

    if (rfspi_open && rx_len > 0) {
        pkt->len = rx_len;
        pkt->data[0] = first_rx;
    }

    /* Clock remaining bytes */
    for (i = 1; i < transfer_len; i++) {
        u8 tx_byte = (i < tx_len && tx_buf) ? tx_buf[i] : 0;
        u8 rx_byte;

        udelay(5);
        rx_byte = cc2544_spi_raw_output_u8(tx_byte);

        if (i < rx_len && rfspi_open)
            pkt->data[i] = rx_byte;
    }

    udelay(5);
    /* De-assert SPI CS (GPIO 8 = High) */
    ar7240_gpio_out_val(GPIO_CC2544_SPI_CS, 1);
    AR7240_SPI_CLK_REG = saved_spi_clock_reg;
    AR7240_SPI_CTRL_REG = 0;
    ar7240_flash_spi_up();

    /* If valid RX packet clocked in, enqueue to read waitqueue */
    if (rx_len > 0 && rfspi_open) {
        list_add_tail(&pkt->list, &rf_data_head);
        wake_up_interruptible(&cc2544_spi_read_wq);
    } else {
        kfree(pkt);
    }

    return rx_len;
}

/* ========================================================================= */
/* /dev/rfspi File Operations                                                */
/* ========================================================================= */

static int cc2544_spi_open(struct inode *inode, struct file *file)
{
    rfspi_open++;
    return 0;
}

static int cc2544_spi_close(struct inode *inode, struct file *file)
{
    rfspi_open--;
    return 0;
}

/*
 * cc2544_spi_write:
 * FIXED: Returns `count` on success instead of `rx_len`.
 */
static ssize_t cc2544_spi_write(struct file *file, const char __user *buf, size_t count, loff_t *ppos)
{
    void *kbuf;
    int ret;

    kbuf = kmalloc(count, GFP_KERNEL);
    if (!kbuf) {
        printk(KERN_ERR "cc2544_spi_write : -ENOMEM\n");
        return -ENOMEM;
    }

    if (copy_from_user(kbuf, buf, count)) {
        printk(KERN_ERR "cc2544_spi_write : copy_from_user\n");
        kfree(kbuf);
        return -EFAULT;
    }

    ret = cc2544_spi_readwrite(kbuf, count);
    kfree(kbuf);

    if (ret < 0)
        return ret;

    /* Posix write contract: return number of bytes written! */
    return count;
}

static ssize_t cc2544_spi_read(struct file *file, char __user *buf, size_t count, loff_t *ppos)
{
    struct cc2544_packet *pkt;
    int to_copy;

    /* Wait until a packet is queued */
    while (list_empty(&rf_data_head)) {
        if (file->f_flags & O_NONBLOCK)
            return -EAGAIN;
        if (wait_event_interruptible(cc2544_spi_read_wq, !list_empty(&rf_data_head)))
            return -ERESTARTSYS;
    }

    pkt = list_first_entry(&rf_data_head, struct cc2544_packet, list);
    to_copy = min_t(int, count, pkt->len);

    if (copy_to_user(buf, pkt->data, to_copy)) {
        printk(KERN_ERR "cc2544_spi_read : copy_to_user\n");
        return -EFAULT;
    }

    list_del(&pkt->list);
    kfree(pkt);
    return to_copy;
}

static long cc2544_spi_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
    switch (cmd) {
    case CC2544_IOC_RESET:
        cc2544_reset();
        return 0;

    case CC2544_IOC_STATUS: {
        int status = 0;
        if (ar7240_gpio_in_val(GPIO_CC2544_DC_BUSY)) status |= 0x01;
        if (ar7240_gpio_in_val(GPIO_CC2544_DD_IRQ))  status |= 0x02;
        if (copy_to_user((int __user *)arg, &status, sizeof(status)))
            return -EFAULT;
        return 0;
    }

    default:
        return -ENOTTY;
    }
}

static const struct file_operations cc2544_spi_fops = {
    .owner          = THIS_MODULE,
    .open           = cc2544_spi_open,
    .release        = cc2544_spi_close,
    .write          = cc2544_spi_write,
    .read           = cc2544_spi_read,
    .unlocked_ioctl = cc2544_spi_ioctl,
};

static struct miscdevice cc2544_spi_dev = {
    .minor = MISC_DYNAMIC_MINOR,
    .name  = "rfspi",
    .fops  = &cc2544_spi_fops,
};

/* ========================================================================= */
/* Interrupt Handler & Workqueue                                             */
/* ========================================================================= */

static void cc2544_spi_worker(struct work_struct *work)
{
    /* Clock out 0 to read any pending packet from CC2544 */
    cc2544_spi_readwrite(NULL, 0);
    enable_irq(CC2544_IRQ_NUM);
}

static irqreturn_t cc2544_spi_notif_interrupt(int irq, void *dev_id)
{
    disable_irq(CC2544_IRQ_NUM);
    schedule_work(&cc2544_spi_work);
    return IRQ_HANDLED;
}

/* ========================================================================= */
/* Module Init & Exit                                                        */
/* ========================================================================= */

static int cc2544_spi_init(void)
{
    int ret;

    ar7240_gpio_config_output(GPIO_CC2544_SPI_CS);
    ar7240_gpio_config_input(GPIO_CC2544_DC_BUSY);
    ar7240_gpio_config_input(GPIO_CC2544_DD_IRQ);
    ar7240_gpio_out_val(GPIO_CC2544_SPI_CS, 1);

    init_waitqueue_head(&cc2544_spi_read_wq);
    INIT_WORK(&cc2544_spi_work, cc2544_spi_worker);
    INIT_LIST_HEAD(&rf_data_head);

    ret = misc_register(&cc2544_spi_dev);
    if (ret) {
        printk(KERN_ERR "cc2544_spi : can't register misc_register\n");
        return ret;
    }

    ar7240_gpio_config_int(GPIO_CC2544_DD_IRQ, 1, 1);
    ret = request_threaded_irq(CC2544_IRQ_NUM, cc2544_spi_notif_interrupt, NULL, 0, "rfspi", NULL);
    if (ret < 0) {
        printk(KERN_ERR "cc2544_spi : Can't allocate irq %d\n", CC2544_IRQ_NUM);
        misc_deregister(&cc2544_spi_dev);
        return ret;
    }

    return 0;
}

static void cc2544_spi_exit(void)
{
    free_irq(CC2544_IRQ_NUM, NULL);
    cancel_work_sync(&cc2544_spi_work);
    misc_deregister(&cc2544_spi_dev);
}

int init_module(void)
{
    int ret;
    u32 chipid;

    ar7240_gpio_config_output(GPIO_CC2544_RESET);
    ar7240_gpio_config_output(GPIO_CC2544_DD_IRQ);
    ar7240_gpio_config_output(GPIO_CC2544_DC_BUSY);
    ar7240_gpio_out_val(GPIO_CC2544_RESET, 1);
    ar7240_gpio_out_val(GPIO_CC2544_DD_IRQ, 0);

    /* Read CC2544 Chip ID via Debug Command 0x68 (GET_CHIP_ID) */
    cc2544_debug_init();
    cc2544_debug_out(0x68);
    cc2544_wait_response();
    chipid = (cc2544_debug_in() << 8) | cc2544_debug_in();

    if ((chipid >> 8) == 0x44) {
        printk(KERN_INFO "cc2544 chipid 0x%04x\n", chipid);
        cc2544_reset();
        ar7240_gpio_config_input(GPIO_CC2544_DD_IRQ);
        ar7240_gpio_config_input(GPIO_CC2544_DC_BUSY);

        ret = misc_register(&cc2544_fw_miscdevice);
        if (ret)
            return ret;

        ret = cc2544_spi_init();
        if (ret < 0) {
            misc_deregister(&cc2544_fw_miscdevice);
            return ret;
        }
        return 0;
    }

    printk(KERN_ERR "cc2544 chipid unknown 0x%04x\n", chipid);
    return -EINVAL;
}

void cleanup_module(void)
{
    misc_deregister(&cc2544_fw_miscdevice);
    cc2544_spi_exit();
}

MODULE_AUTHOR("Logitech / Re-engineered");
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("TI CC2544 RF transceiver driver with arbitrary packet support");
