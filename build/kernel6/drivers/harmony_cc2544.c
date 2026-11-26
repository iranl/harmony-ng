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
#include <linux/wait.h>

#define MAX_FRAME	64
#define QUEUE_LEN	32	/* frames waiting for read */
#define READY_WAIT_MS	1000

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

static irqreturn_t cc2544_irq_thread(int irq, void *data)
{
	struct cc2544 *c = data;

	cc2544_xfer(c, NULL, 0);
	return IRQ_HANDLED;
}

static struct cc2544 *to_cc(struct file *f)
{
	return container_of(f->private_data, struct cc2544, misc);
}

static int cc2544_open(struct inode *inode, struct file *f)
{
	atomic_inc(&to_cc(f)->users);
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
	spi->mode = SPI_MODE_0;
	spi->bits_per_word = 8;
	ret = spi_setup(spi);
	if (ret)
		return ret;
	if (!spi->irq)
		return dev_err_probe(dev, -EINVAL, "no interrupt\n");
	ret = devm_request_threaded_irq(dev, spi->irq, NULL, cc2544_irq_thread, IRQF_ONESHOT,
					"cc2544", c);
	if (ret)
		return ret;

	c->misc.minor = MISC_DYNAMIC_MINOR;
	c->misc.name = "rfspi";
	c->misc.fops = &cc2544_fops;
	c->misc.mode = 0600;
	ret = misc_register(&c->misc);
	if (ret)
		return ret;
	spi_set_drvdata(spi, c);
	dev_info(dev, "/dev/rfspi ready (irq %d)\n", spi->irq);
	return 0;
}

static void cc2544_remove(struct spi_device *spi)
{
	struct cc2544 *c = spi_get_drvdata(spi);

	misc_deregister(&c->misc);
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
