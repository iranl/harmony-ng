// SPDX-License-Identifier: GPL-2.0
/*
 * /dev/rfspi for the Logitech Harmony Hub (AR9330) on Linux 6.x: the CC2544 RF transceiver of
 * the Harmony remotes, on the SPI bus it shares with the NOR flash (chip select GPIO 8).
 * It keeps the interface of the stock 2.6.31 module cc2544.ko that hub_rf uses
 * (payload/source/rf_spi.c):
 *
 *   write  one frame to the CC2544 (full duplex: a frame the CC2544 sends at the same time is
 *          queued for read); returns 0, as the stock driver did
 *   read   blocks until a frame is queued, returns it whole (its length from the first byte)
 *
 * Transfer, as cc2544.ko cc2544_spi_readwrite: wait until the ready line (GPIO 6) is low, chip
 * select low, 20 us, then bytes in SPI mode 0, 5 us apart. The first byte the CC2544 sends gives
 * the frame length (table below); max(write length, frame length) bytes are clocked. The CC2544
 * raises its interrupt line (GPIO 14, level high) when it has a frame: the threaded handler
 * clocks it out with zeros. The SPI bus is held for the whole frame, so no flash access can come
 * between its bytes.
 *
 * /dev/rffw, the CC2544's own flash (stock cc2544_fw), over the TI 8051 debug interface: the
 * ready line (GPIO 6) is debug data, the interrupt line (GPIO 14) debug clock, GPIO 15 the
 * CC2544's reset (the stock module held it high). The line also resets the CSR8811 Bluetooth
 * chip: after a /dev/rffw session (a read too) Bluetooth stays silent until the Hub restarts
 * (seen on a Hub: bccmd gets no answer at any rate, hub_btstack never reaches WORKING).
 * As the stock driver:
 *   open read-only   reads the 28 KB flash (28 pages of 1 KB) for read
 *   open write-only  write the image (at most 32 KB); at close every page is erased, then
 *                    written and read back, page 0 (the reset vector) last; an image that starts
 *                    with "erase" erases the whole chip instead
 * Not while /dev/rfspi is open (hub_rf must be stopped). The page erase and write run on the
 * CC2544 itself: two small 8051 routines (from cc2544.ko) loaded into its RAM.
 */
#include <linux/delay.h>
#include <linux/fs.h>
#include <linux/gpio/consumer.h>
#include <linux/interrupt.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/slab.h>
#include <linux/spi/spi.h>
#include <linux/uaccess.h>
#include <linux/vmalloc.h>
#include <linux/wait.h>

#define MAX_FRAME	64
#define QUEUE_LEN	32	/* frames waiting for read */
#define READY_WAIT_MS	1000

#define FW_BUF		0x8000	/* /dev/rffw image buffer (stock size) */
#define FW_PAGES	28	/* flash pages the stock driver erases, writes and reads */
#define FW_PAGE		1024

struct frame {
	u8 len;
	u8 data[MAX_FRAME];
};

struct cc2544 {
	struct spi_device *spi;
	struct gpio_desc *ready;
	struct mutex xfer_lock;		/* one transfer at a time */
	spinlock_t q_lock;
	wait_queue_head_t rx_wait;
	struct frame q[QUEUE_LEN];
	unsigned int q_head, q_tail;	/* q_head == q_tail: empty */
	atomic_t users;
	struct miscdevice misc;
	/* /dev/rffw */
	struct miscdevice fw_misc;
	struct gpio_desc *dc;		/* debug clock: the interrupt line */
	struct gpio_desc *reset;	/* active low */
	atomic_t fw_open;
	u8 *fw_buf;			/* FW_BUF bytes while /dev/rffw is open */
	bool fw_write;
	u8 fw_page[FW_PAGE];
	/* under xfer_lock; too big for the kernel stack */
	struct spi_transfer t[MAX_FRAME];
	u8 txb[MAX_FRAME];
	u8 rxb[MAX_FRAME];
};

/* frame length announced by the first byte the CC2544 sends (cc2544.ko) */
static int frame_len(u8 id)
{
	switch (id) {
	case 0x01: case 0x02: case 0x10: return 7;
	case 0x03: return 4;
	case 0x04: case 0x08: return 1;
	case 0x11: return 20;
	case 0x12: return 30;
	case 0x20: return 32;
	}
	return 0;
}

static void queue_frame(struct cc2544 *c, const u8 *data, int len)
{
	unsigned long flags;
	unsigned int next;

	spin_lock_irqsave(&c->q_lock, flags);
	next = (c->q_head + 1) % QUEUE_LEN;
	if (next == c->q_tail)		/* full: drop the oldest */
		c->q_tail = (c->q_tail + 1) % QUEUE_LEN;
	c->q[c->q_head].len = len;
	memcpy(c->q[c->q_head].data, data, len);
	c->q_head = next;
	spin_unlock_irqrestore(&c->q_lock, flags);
	wake_up_interruptible(&c->rx_wait);
}

/* One frame exchange. tx may be NULL (read on interrupt). Returns the received frame length
 * (0: none) or a negative error. */
static int cc2544_xfer(struct cc2544 *c, const u8 *tx, int tx_len)
{
	struct spi_controller *ctlr = c->spi->controller;
	struct spi_transfer *t = c->t;
	u8 *txb = c->txb, *rxb = c->rxb;
	struct spi_message m;
	unsigned long end;
	int rx_len, n, i, ret;

	if (tx_len > MAX_FRAME)
		return -EINVAL;
	mutex_lock(&c->xfer_lock);
	memset(txb, 0, MAX_FRAME);
	if (tx)
		memcpy(txb, tx, tx_len);

	end = jiffies + msecs_to_jiffies(READY_WAIT_MS);
	while (gpiod_get_value_cansleep(c->ready)) {
		if (time_after(jiffies, end)) {
			ret = -EIO;
			goto out;
		}
		usleep_range(500, 1000);
	}

	spi_bus_lock(ctlr);
	/* first byte: chip select stays low after it (cs_change on the last transfer) */
	memset(t, 0, sizeof(c->t));
	t[0].tx_buf = txb;
	t[0].rx_buf = rxb;
	t[0].len = 1;
	t[0].delay.value = 5;
	t[0].delay.unit = SPI_DELAY_UNIT_USECS;
	t[0].cs_change = 1;
	spi_message_init_with_transfers(&m, t, 1);
	ret = spi_sync_locked(c->spi, &m);
	if (ret)
		goto unlock;

	rx_len = frame_len(rxb[0]);
	if (!rx_len && rxb[0])
		dev_warn_ratelimited(&c->spi->dev, "frame length 0x%02x unknown\n", rxb[0]);
	n = max(rx_len, tx_len);
	if (n > 1) {
		memset(t, 0, sizeof(c->t));
		for (i = 1; i < n; i++) {
			t[i - 1].tx_buf = &txb[i];
			t[i - 1].rx_buf = &rxb[i];
			t[i - 1].len = 1;
			t[i - 1].delay.value = 5;
			t[i - 1].delay.unit = SPI_DELAY_UNIT_USECS;
		}
		spi_message_init_with_transfers(&m, t, n - 1);
		ret = spi_sync_locked(c->spi, &m);
	} else {
		/* nothing more: a zero-length message ends the chip select */
		memset(t, 0, sizeof(c->t));
		spi_message_init_with_transfers(&m, t, 1);
		ret = spi_sync_locked(c->spi, &m);
	}
unlock:
	spi_bus_unlock(ctlr);
	if (!ret && rx_len && atomic_read(&c->users))
		queue_frame(c, rxb, rx_len);
	if (!ret)
		ret = rx_len;
out:
	mutex_unlock(&c->xfer_lock);
	return ret;
}

/* gpio-ath79 runs its interrupts with handle_simple_irq, which does not mask a level interrupt
 * while the thread runs (IRQF_ONESHOT masks only with handle_level_irq): the line, high until
 * the frame is read, fired thousands of times per frame, each counted as unhandled, and the
 * kernel disabled interrupt 14 ("nobody cared") within a minute of boot. The interrupt is
 * masked here and unmasked once the thread has read the frame. */
static irqreturn_t cc2544_irq(int irq, void *data)
{
	disable_irq_nosync(irq);
	return IRQ_WAKE_THREAD;
}

static irqreturn_t cc2544_irq_thread(int irq, void *data)
{
	struct cc2544 *c = data;

	/* a resent interrupt (pending while masked) for a frame already read: line low */
	if (gpiod_get_value_cansleep(c->dc))
		cc2544_xfer(c, NULL, 0);
	enable_irq(irq);
	return IRQ_HANDLED;
}

static struct cc2544 *to_cc(struct file *f)
{
	return container_of(f->private_data, struct cc2544, misc);
}

static int cc2544_open(struct inode *inode, struct file *f)
{
	struct cc2544 *c = to_cc(f);

	if (atomic_read(&c->fw_open))
		return -EBUSY;
	atomic_inc(&c->users);
	return 0;
}

static int cc2544_release(struct inode *inode, struct file *f)
{
	atomic_dec(&to_cc(f)->users);
	return 0;
}

static ssize_t cc2544_write(struct file *f, const char __user *ubuf, size_t len, loff_t *off)
{
	struct cc2544 *c = to_cc(f);
	u8 buf[MAX_FRAME];
	int ret;

	if (len > MAX_FRAME)
		return -EINVAL;
	if (copy_from_user(buf, ubuf, len))
		return -EFAULT;
	ret = cc2544_xfer(c, buf, len);
	return ret < 0 ? ret : 0;
}

static ssize_t cc2544_read(struct file *f, char __user *ubuf, size_t len, loff_t *off)
{
	struct cc2544 *c = to_cc(f);
	struct frame fr;
	unsigned long flags;
	int ret;

	for (;;) {
		spin_lock_irqsave(&c->q_lock, flags);
		if (c->q_head != c->q_tail) {
			fr = c->q[c->q_tail];
			c->q_tail = (c->q_tail + 1) % QUEUE_LEN;
			spin_unlock_irqrestore(&c->q_lock, flags);
			break;
		}
		spin_unlock_irqrestore(&c->q_lock, flags);
		if (f->f_flags & O_NONBLOCK)
			return -EAGAIN;
		ret = wait_event_interruptible(c->rx_wait, READ_ONCE(c->q_head) != READ_ONCE(c->q_tail));
		if (ret)
			return ret;
	}
	if (len > fr.len)
		len = fr.len;
	if (copy_to_user(ubuf, fr.data, len))
		return -EFAULT;
	return len;
}

static const struct file_operations cc2544_fops = {
	.owner		= THIS_MODULE,
	.open		= cc2544_open,
	.release	= cc2544_release,
	.read		= cc2544_read,
	.write		= cc2544_write,
	.llseek		= noop_llseek,
};

/* ---- /dev/rffw: the 8051 debug interface (timing as cc2544.ko) ---- */

/* 8051 routines of cc2544.ko, run from CC2544 RAM (XDATA 0x0000 = code 0x8000). Byte 7 is the
 * flash page (FADDRH = byte address >> 10). Erase: FADDR = page, FCTL.ERASE, wait while
 * FCTL.BUSY. Write: FADDR = page, FCTL.WRITE, 1024 bytes from XDATA 0x0000 to FWDATA, four at a
 * time, waiting while FCTL.FULL. Both end in 0xA5 (halt, debug breakpoint). */
static const u8 fw_erase_code[29] = {
	0x74, 0x00, 0x90, 0x62, 0x71, 0xF0, 0x74, 0x00, 0x90, 0x62, 0x72, 0xF0, 0x90, 0x62, 0x70,
	0xE0, 0xD2, 0xE0, 0xF0, 0x00, 0x90, 0x62, 0x70, 0xE0, 0xA2, 0xE7, 0x40, 0xF8, 0xA5,
};
static const u8 fw_write_code[116] = {
	0x74, 0x00, 0x90, 0x62, 0x71, 0xF0, 0x74, 0x00, 0x90, 0x62, 0x72, 0xF0, 0x90, 0x62, 0x70,
	0xE0, 0xD2, 0xE1, 0xF0, 0x78, 0x00, 0x79, 0x00, 0xC3, 0xE8, 0x94, 0x00, 0xE9, 0x94, 0x04,
	0xA2, 0xD2, 0x65, 0xD0, 0x33, 0x50, 0x4E, 0x74, 0x00, 0x28, 0xF5, 0x82, 0x74, 0x00, 0x39,
	0xF5, 0x83, 0xE0, 0x90, 0x62, 0x73, 0xF0, 0x74, 0x01, 0x28, 0xF5, 0x82, 0x74, 0x00, 0x39,
	0xF5, 0x83, 0xE0, 0x90, 0x62, 0x73, 0xF0, 0x74, 0x02, 0x28, 0xF5, 0x82, 0x74, 0x00, 0x39,
	0xF5, 0x83, 0xE0, 0x90, 0x62, 0x73, 0xF0, 0x74, 0x03, 0x28, 0xF5, 0x82, 0x74, 0x00, 0x39,
	0xF5, 0x83, 0xE0, 0x90, 0x62, 0x73, 0xF0, 0x90, 0x62, 0x70, 0xE0, 0xA2, 0xE6, 0x40, 0xF8,
	0xE8, 0x24, 0x04, 0xF8, 0xE9, 0x34, 0x00, 0xF9, 0x80, 0xA4, 0xA5,
};
#define FW_CODE_PAGE	7	/* index of the page byte in both routines */

#define DBG_CHIP_ERASE	0x14
#define DBG_GET_CHIP_ID	0x68
#define DBG_READ_STATUS	0x34
#define DBG_RESUME	0x4C
#define DBG_INSTR1	0x55
#define DBG_INSTR2	0x56
#define DBG_INSTR3	0x57
#define STATUS_HALTED	0x20
#define CHIP_ID_CC2544	0x44

static void dbg_clock(struct cc2544 *c, int v)
{
	gpiod_set_value(c->dc, v);
	ndelay(20);
}

static void dbg_out(struct cc2544 *c, u8 b)
{
	udelay(2);
	gpiod_direction_output(c->ready, 0);
	for (int i = 7; i >= 0; i--) {
		gpiod_set_value(c->dc, 1);
		ndelay(20);
		gpiod_set_value(c->ready, (b >> i) & 1);
		dbg_clock(c, 0);
	}
}

static u8 dbg_in(struct cc2544 *c)
{
	u8 b = 0;

	for (int i = 0; i < 8; i++) {
		dbg_clock(c, 1);
		b = (b << 1) | !!gpiod_get_value(c->ready);
		dbg_clock(c, 0);
	}
	return b;
}

/* the CC2544 pulls the data line low when its answer is ready; high: one byte to skip */
static void dbg_wait(struct cc2544 *c)
{
	gpiod_direction_input(c->ready);
	udelay(2);
	if (gpiod_get_value(c->ready)) {
		dbg_in(c);
		udelay(2);
	}
}

static u8 dbg_cmd(struct cc2544 *c, const u8 *b, int n)
{
	for (int i = 0; i < n; i++)
		dbg_out(c, b[i]);
	dbg_wait(c);
	return dbg_in(c);
}

static u8 dbg_instr1(struct cc2544 *c, u8 op)
{
	return dbg_cmd(c, (u8[]){ DBG_INSTR1, op }, 2);
}

static u8 dbg_instr3(struct cc2544 *c, u8 op, u8 a, u8 b)
{
	return dbg_cmd(c, (u8[]){ DBG_INSTR3, op, a, b }, 4);
}

/* reset low with two clock pulses: the CC2544 starts in debug mode */
static void dbg_enter(struct cc2544 *c)
{
	gpiod_direction_output(c->dc, 0);
	gpiod_direction_output(c->ready, 0);
	gpiod_set_value(c->reset, 0);
	msleep(20);
	gpiod_set_value(c->reset, 1);
	msleep(10);
	dbg_clock(c, 1);
	dbg_clock(c, 0);
	dbg_clock(c, 1);
	dbg_clock(c, 0);
	msleep(10);
	gpiod_set_value(c->reset, 0);
}

/* back to the RF firmware: a plain reset, lines to inputs */
static void dbg_leave(struct cc2544 *c)
{
	gpiod_direction_input(c->ready);
	gpiod_direction_input(c->dc);
	gpiod_set_value(c->reset, 1);
	msleep(20);
	gpiod_set_value(c->reset, 0);
}

static void dbg_read_page(struct cc2544 *c, unsigned int addr, u8 *buf)
{
	dbg_instr3(c, 0x75, 0xC7, 0);			/* MOV MEMCTR,#0 */
	dbg_instr3(c, 0x90, addr >> 8, addr & 0xff);	/* MOV DPTR,#addr */
	for (int i = 0; i < FW_PAGE; i++) {
		dbg_instr1(c, 0xE4);			/* CLR A */
		buf[i] = dbg_instr1(c, 0x93);		/* MOVC A,@A+DPTR */
		dbg_instr1(c, 0xA3);			/* INC DPTR */
		if (!(i & 63))
			cond_resched();
	}
}

static void dbg_write_xdata(struct cc2544 *c, unsigned int addr, const u8 *buf, int len)
{
	dbg_instr3(c, 0x90, addr >> 8, addr & 0xff);	/* MOV DPTR,#addr */
	for (int i = 0; i < len; i++) {
		dbg_cmd(c, (u8[]){ DBG_INSTR2, 0x74, buf[i] }, 3);	/* MOV A,#b */
		dbg_instr1(c, 0xF0);			/* MOVX @DPTR,A */
		dbg_instr1(c, 0xA3);			/* INC DPTR */
	}
}

/* run an 8051 routine loaded at XDATA xaddr until it halts (0xA5) */
static int dbg_run(struct cc2544 *c, unsigned int xaddr, unsigned int wait_us)
{
	int i;

	dbg_instr3(c, 0x02, (0x8000 + xaddr) >> 8, (0x8000 + xaddr) & 0xff);	/* LJMP: PC */
	dbg_instr3(c, 0x75, 0xC7, 8);			/* MEMCTR: RAM in code space */
	dbg_cmd(c, (u8[]){ DBG_RESUME }, 1);
	for (i = 0; i < 100; i++) {
		if (dbg_cmd(c, (u8[]){ DBG_READ_STATUS }, 1) & STATUS_HALTED)
			break;
		usleep_range(wait_us, wait_us + 50);
	}
	dbg_instr3(c, 0x75, 0xC7, 0);
	return i < 100 ? 0 : -ETIMEDOUT;
}

static int fw_chip_id(struct cc2544 *c)
{
	u8 id;

	dbg_out(c, DBG_GET_CHIP_ID);
	dbg_wait(c);
	id = dbg_in(c);
	dbg_in(c);	/* revision */
	return id;
}

static int fw_program(struct cc2544 *c)
{
	struct device *dev = &c->spi->dev;
	u8 code[sizeof(fw_write_code)];
	unsigned int page, i, n;
	const u8 *src;
	int id;

	id = fw_chip_id(c);
	if (id != CHIP_ID_CC2544) {
		dev_err(dev, "rffw: chip id 0x%02x, not a CC2544\n", id);
		return -ENODEV;
	}
	if (!memcmp(c->fw_buf, "erase", 5)) {
		dbg_cmd(c, (u8[]){ DBG_CHIP_ERASE }, 1);
		dev_info(dev, "rffw: chip erased\n");
		return 0;
	}
	memcpy(code, fw_erase_code, sizeof(fw_erase_code));
	for (page = 0; page < FW_PAGES; page++) {
		code[FW_CODE_PAGE] = page;
		dbg_write_xdata(c, 0, code, sizeof(fw_erase_code));
		if (dbg_run(c, 0, 1000)) {
			dev_err(dev, "rffw: erase of page %u timed out\n", page);
			return -ETIMEDOUT;
		}
	}
	/* page 0 (reset vector) last: a cut write leaves no half image that starts */
	for (n = 1; n <= FW_PAGES; n++) {
		page = n % FW_PAGES;
		src = c->fw_buf + page * FW_PAGE;

		for (i = 0; i < FW_PAGE && src[i] == 0xff; i++)
			;
		if (i == FW_PAGE)
			continue;			/* erased already */
		dbg_write_xdata(c, 0, src, FW_PAGE);
		memcpy(code, fw_write_code, sizeof(code));
		code[FW_CODE_PAGE] = page;
		dbg_write_xdata(c, FW_PAGE, code, sizeof(code));
		if (dbg_run(c, FW_PAGE, 100)) {
			dev_err(dev, "rffw: write of page %u timed out\n", page);
			return -ETIMEDOUT;
		}
		dbg_read_page(c, page * FW_PAGE, c->fw_page);
		if (memcmp(c->fw_page, src, FW_PAGE)) {
			dev_err(dev, "rffw: page %u does not read back\n", page);
			return -EIO;
		}
	}
	dev_info(dev, "rffw: flash programmed\n");
	return 0;
}

static struct cc2544 *fw_cc(struct file *f)
{
	return container_of(f->private_data, struct cc2544, fw_misc);
}

/* the session ends: program (write-only open), start the RF firmware again, interrupt back */
static void fw_end(struct cc2544 *c)
{
	mutex_lock(&c->xfer_lock);
	if (c->fw_write)
		fw_program(c);
	dbg_leave(c);
	if (request_threaded_irq(c->spi->irq, cc2544_irq, cc2544_irq_thread, 0, "cc2544", c))
		dev_err(&c->spi->dev, "rffw: interrupt %d not requested again\n", c->spi->irq);
	mutex_unlock(&c->xfer_lock);
	vfree(c->fw_buf);
	c->fw_buf = NULL;
	atomic_set(&c->fw_open, 0);
}

static int cc2544_fw_open(struct inode *inode, struct file *f)
{
	struct cc2544 *c = fw_cc(f);
	int mode = f->f_flags & O_ACCMODE, ret = 0;

	if (mode == O_RDWR)
		return -EINVAL;
	if (atomic_cmpxchg(&c->fw_open, 0, 1))
		return -EBUSY;
	if (atomic_read(&c->users)) {
		atomic_set(&c->fw_open, 0);
		return -EBUSY;			/* hub_rf has /dev/rfspi open */
	}
	c->fw_buf = vmalloc(FW_BUF);
	if (!c->fw_buf) {
		atomic_set(&c->fw_open, 0);
		return -ENOMEM;
	}
	memset(c->fw_buf, 0xff, FW_BUF);
	c->fw_write = false;
	/* the interrupt line becomes the debug clock: no interrupt meanwhile. Freed before taking
	 * xfer_lock: free_irq waits for the threaded handler, which takes that lock itself. Disabled
	 * first: a handler running now ends with enable_irq(), which must not come after free_irq */
	disable_irq(c->spi->irq);
	free_irq(c->spi->irq, c);
	mutex_lock(&c->xfer_lock);
	dbg_enter(c);
	if (mode == O_RDONLY) {
		if (fw_chip_id(c) != CHIP_ID_CC2544)
			ret = -ENODEV;
		for (int p = 0; !ret && p < FW_PAGES; p++)
			dbg_read_page(c, p * FW_PAGE, c->fw_buf + p * FW_PAGE);
	}
	mutex_unlock(&c->xfer_lock);
	if (ret) {
		fw_end(c);
		return ret;
	}
	c->fw_write = mode == O_WRONLY;
	return 0;
}

static int cc2544_fw_release(struct inode *inode, struct file *f)
{
	fw_end(fw_cc(f));	/* close() cannot report the result: the kernel log has it */
	return 0;
}

static ssize_t cc2544_fw_write(struct file *f, const char __user *ubuf, size_t len, loff_t *off)
{
	struct cc2544 *c = fw_cc(f);

	if (*off >= FW_BUF)
		return -ENOSPC;
	len = min_t(size_t, len, FW_BUF - *off);
	if (copy_from_user(c->fw_buf + *off, ubuf, len))
		return -EFAULT;
	*off += len;
	return len;
}

static ssize_t cc2544_fw_read(struct file *f, char __user *ubuf, size_t len, loff_t *off)
{
	struct cc2544 *c = fw_cc(f);

	if (*off >= FW_BUF)
		return 0;
	len = min_t(size_t, len, FW_BUF - *off);
	if (copy_to_user(ubuf, c->fw_buf + *off, len))
		return -EFAULT;
	*off += len;
	return len;
}

static const struct file_operations cc2544_fw_fops = {
	.owner		= THIS_MODULE,
	.open		= cc2544_fw_open,
	.release	= cc2544_fw_release,
	.read		= cc2544_fw_read,
	.write		= cc2544_fw_write,
	.llseek		= default_llseek,
};

static int cc2544_probe(struct spi_device *spi)
{
	struct device *dev = &spi->dev;
	struct cc2544 *c;
	int ret;

	c = devm_kzalloc(dev, sizeof(*c), GFP_KERNEL);
	if (!c)
		return -ENOMEM;
	c->spi = spi;
	mutex_init(&c->xfer_lock);
	spin_lock_init(&c->q_lock);
	init_waitqueue_head(&c->rx_wait);

	c->ready = devm_gpiod_get(dev, "ready", GPIOD_IN);
	if (IS_ERR(c->ready))
		return dev_err_probe(dev, PTR_ERR(c->ready), "ready-gpios\n");
	/* out of reset now (the CSR8811 Bluetooth needs the line high as well) */
	c->reset = devm_gpiod_get(dev, "reset", GPIOD_OUT_LOW);
	if (IS_ERR(c->reset))
		return dev_err_probe(dev, PTR_ERR(c->reset), "reset-gpios\n");
	/* the interrupt line, driven only as the debug clock of /dev/rffw */
	c->dc = devm_gpiod_get(dev, "dc", GPIOD_ASIS);
	if (IS_ERR(c->dc))
		return dev_err_probe(dev, PTR_ERR(c->dc), "dc-gpios\n");
	spi->mode = SPI_MODE_0;
	spi->bits_per_word = 8;
	ret = spi_setup(spi);
	if (ret)
		return ret;
	if (!spi->irq)
		return dev_err_probe(dev, -EINVAL, "no interrupt\n");
	/* not devm: /dev/rffw frees it while it drives the line */
	ret = request_threaded_irq(spi->irq, cc2544_irq, cc2544_irq_thread, 0, "cc2544", c);
	if (ret)
		return ret;

	c->misc.minor = MISC_DYNAMIC_MINOR;
	c->misc.name = "rfspi";
	c->misc.fops = &cc2544_fops;
	c->misc.mode = 0600;
	ret = misc_register(&c->misc);
	if (ret)
		goto err_irq;
	c->fw_misc.minor = MISC_DYNAMIC_MINOR;
	c->fw_misc.name = "rffw";
	c->fw_misc.fops = &cc2544_fw_fops;
	c->fw_misc.mode = 0600;
	ret = misc_register(&c->fw_misc);
	if (ret)
		goto err_misc;
	spi_set_drvdata(spi, c);
	dev_info(dev, "/dev/rfspi and /dev/rffw ready (irq %d)\n", spi->irq);
	return 0;

err_misc:
	misc_deregister(&c->misc);
err_irq:
	free_irq(spi->irq, c);
	return ret;
}

static void cc2544_remove(struct spi_device *spi)
{
	struct cc2544 *c = spi_get_drvdata(spi);

	misc_deregister(&c->fw_misc);
	misc_deregister(&c->misc);
	free_irq(spi->irq, c);
}

static const struct of_device_id cc2544_of_match[] = {
	{ .compatible = "logitech,harmony-hub-cc2544" },
	{}
};
MODULE_DEVICE_TABLE(of, cc2544_of_match);

static const struct spi_device_id cc2544_ids[] = {
	{ "harmony-hub-cc2544" },
	{}
};
MODULE_DEVICE_TABLE(spi, cc2544_ids);

static struct spi_driver cc2544_driver = {
	.probe		= cc2544_probe,
	.remove		= cc2544_remove,
	.id_table	= cc2544_ids,
	.driver		= {
		.name = "harmony-cc2544",
		.of_match_table = cc2544_of_match,
	},
};
module_spi_driver(cc2544_driver);

MODULE_DESCRIPTION("Harmony Hub CC2544 RF transceiver (/dev/rfspi, cc2544.ko interface)");
MODULE_LICENSE("GPL");
