// SPDX-License-Identifier: GPL-2.0
/*
 * /dev/i2s for the Logitech Harmony Hub (AR9330) on Linux 6.x: the IR blaster (transmit) and
 * the IR receiver (read), through the SoC stereo (I2S) block and its MBOX DMA. It keeps the
 * interface of the stock 2.6.31 module ath_i2s.ko, which payload/source/ir_i2s.c uses:
 *
 *   ioctl _IOW('N', 0x20) VOLUME, 0x21 FREQ (stereo clock divider), 0x22 DSIZE (8/16/24/32),
 *         0x23 MODE (2 = receive), 0x30 START (bits 16-18: IR outputs to enable), 0x31 DRAIN
 *   write  16-bit samples, queued in 768-byte DMA buffers; START sends them
 *   read   whole 768-byte receive buffers
 *
 * Register use and values follow ath_i2s.ko (build/kernel6/drivers/ATH_I2S.md).
 */
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/fs.h>
#include <linux/gpio/consumer.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/reset.h>
#include <linux/uaccess.h>
#include <linux/wait.h>

/* MBOX DMA (0x180a0000) */
#define MBOX_DMA_POLICY		0x10
#define MBOX_TX_DESC		0x18	/* memory -> stereo (transmit) */
#define MBOX_TX_CTRL		0x1c
#define MBOX_RX_DESC		0x20	/* stereo -> memory (receive) */
#define MBOX_RX_CTRL		0x24
#define MBOX_INT_STATUS		0x44
#define MBOX_INT_ENABLE		0x4c
#define MBOX_FIFO_RESET		0x58
#define DMA_START		2
#define DMA_PAUSE		1
#define DMA_RESUME		4
#define INT_TX			0x400
#define INT_RX			0x40

/* stereo block (0x180b0000) */
#define ST_CONFIG		0x00
#define ST_VOLUME		0x04
#define ST_CLK_DIV		0x1c
#define ST_CONFIG_RESET		0x80000
#define ST_CONFIG_SPKR		0x4000	/* cleared in receive mode */
#define ST_CONFIG_16BIT		0xa21302
#define ST_CLK_DIV_DEFAULT	0x215999

#define GPIO_FUNC1_I2S		0x24000000

#define I2S_VOLUME		_IOW('N', 0x20, int)
#define I2S_FREQ		_IOW('N', 0x21, int)
#define I2S_DSIZE		_IOW('N', 0x22, int)
#define I2S_MODE		_IOW('N', 0x23, int)
#define I2S_START		_IOW('N', 0x30, int)
#define I2S_DRAIN		_IOW('N', 0x31, int)

#define BUF_SIZE		768
#define TX_DESCS		512
#define RX_DESCS		128
#define DESC_OWN		BIT(31)	/* set: the DMA owns the buffer */
#define DESC_EOM		BIT(30)
#define NR_PORTS		3

struct mbox_desc {
	u32 ctrl;	/* 0-11 size, 12-23 length, 30 EOM, 31 own */
	u32 buf;
	u32 next;
};

struct ring {
	struct mbox_desc *desc;
	dma_addr_t desc_dma;
	u8 *buf;
	dma_addr_t buf_dma;
	int n;
	int head;	/* next descriptor to fill (tx) or to read (rx) */
	bool open;
	bool started;	/* descriptor base programmed and DMA started */
};

struct hi2s {
	struct device *dev;
	void __iomem *mbox;
	void __iomem *stereo;
	void __iomem *gpio_func1;
	struct reset_control *rst;
	struct gpio_descs *ports;
	struct mutex lock;
	spinlock_t irq_lock;
	wait_queue_head_t tx_wait;
	wait_queue_head_t rx_wait;
	struct ring tx;
	struct ring rx;
	struct miscdevice misc;
};

static void stereo_reset(struct hi2s *s)
{
	writel(0xff, s->mbox + MBOX_FIFO_RESET);
	udelay(500);
	writel(readl(s->stereo + ST_CONFIG) | ST_CONFIG_RESET, s->stereo + ST_CONFIG);
	udelay(100);
}

static int ring_alloc(struct hi2s *s, struct ring *r, int n, bool rx)
{
	int i;

	r->n = n;
	r->head = 0;
	r->started = false;
	r->desc = dma_alloc_coherent(s->dev, n * sizeof(*r->desc), &r->desc_dma, GFP_KERNEL);
	r->buf = dma_alloc_coherent(s->dev, n * BUF_SIZE, &r->buf_dma, GFP_KERNEL);
	if (!r->desc || !r->buf)
		goto fail;
	for (i = 0; i < n; i++) {
		r->desc[i].ctrl = BUF_SIZE | (BUF_SIZE << 12) | (rx ? DESC_OWN : 0);
		r->desc[i].buf = r->buf_dma + i * BUF_SIZE;
		r->desc[i].next = r->desc_dma + ((i + 1) % n) * sizeof(*r->desc);
	}
	wmb();
	return 0;
fail:
	if (r->desc)
		dma_free_coherent(s->dev, n * sizeof(*r->desc), r->desc, r->desc_dma);
	if (r->buf)
		dma_free_coherent(s->dev, n * BUF_SIZE, r->buf, r->buf_dma);
	r->desc = NULL;
	r->buf = NULL;
	return -ENOMEM;
}

static void ring_free(struct hi2s *s, struct ring *r)
{
	dma_free_coherent(s->dev, r->n * sizeof(*r->desc), r->desc, r->desc_dma);
	dma_free_coherent(s->dev, r->n * BUF_SIZE, r->buf, r->buf_dma);
	r->desc = NULL;
	r->buf = NULL;
}

static bool tx_idle(struct hi2s *s)
{
	int i;

	for (i = 0; i < s->tx.n; i++)
		if (READ_ONCE(s->tx.desc[i].ctrl) & DESC_OWN)
			return false;
	return true;
}

static irqreturn_t hi2s_irq(int irq, void *data)
{
	struct hi2s *s = data;
	u32 st = readl(s->mbox + MBOX_INT_STATUS);

	writel(st, s->mbox + MBOX_INT_STATUS);
	if (st & INT_TX) {
		wake_up_interruptible(&s->tx_wait);
		if (s->tx.open && !tx_idle(s))
			writel(DMA_RESUME, s->mbox + MBOX_TX_CTRL);
	}
	if (st & INT_RX) {
		wake_up_interruptible(&s->rx_wait);
		if (s->rx.open)
			writel(DMA_RESUME, s->mbox + MBOX_RX_CTRL);
	}
	return st & (INT_TX | INT_RX) ? IRQ_HANDLED : IRQ_NONE;
}

static struct hi2s *to_hi2s(struct file *f)
{
	return container_of(f->private_data, struct hi2s, misc);
}

static int hi2s_open(struct inode *inode, struct file *f)
{
	struct hi2s *s = to_hi2s(f);
	bool first;
	int ret = 0;

	mutex_lock(&s->lock);
	if (((f->f_mode & FMODE_READ) && s->rx.open) || ((f->f_mode & FMODE_WRITE) && s->tx.open)) {
		ret = -EBUSY;
		goto out;
	}
	first = !s->rx.open && !s->tx.open;
	if (first)
		stereo_reset(s);
	if (f->f_mode & FMODE_READ) {
		ret = ring_alloc(s, &s->rx, RX_DESCS, true);
		if (ret)
			goto out;
		s->rx.open = true;
	} else {
		ret = ring_alloc(s, &s->tx, TX_DESCS, false);
		if (ret)
			goto out;
		s->tx.open = true;
	}
	if (first)
		writel(98, s->mbox + MBOX_DMA_POLICY);
out:
	mutex_unlock(&s->lock);
	return ret;
}

static int hi2s_release(struct inode *inode, struct file *f)
{
	struct hi2s *s = to_hi2s(f);
	struct ring *r = (f->f_mode & FMODE_READ) ? &s->rx : &s->tx;
	unsigned long flags;

	mutex_lock(&s->lock);
	spin_lock_irqsave(&s->irq_lock, flags);
	writel(DMA_PAUSE, s->mbox + ((f->f_mode & FMODE_READ) ? MBOX_RX_CTRL : MBOX_TX_CTRL));
	writel(readl(s->mbox + MBOX_INT_ENABLE) & ~((f->f_mode & FMODE_READ) ? INT_RX : INT_TX),
	       s->mbox + MBOX_INT_ENABLE);
	r->open = false;
	spin_unlock_irqrestore(&s->irq_lock, flags);
	if (!(f->f_mode & FMODE_READ))
		gpiod_set_array_value(s->ports->ndescs, s->ports->desc, s->ports->info,
				      (unsigned long[]){ 0 });
	ring_free(s, r);
	mutex_unlock(&s->lock);
	return 0;
}

static ssize_t hi2s_write(struct file *f, const char __user *ubuf, size_t len, loff_t *off)
{
	struct hi2s *s = to_hi2s(f);
	struct ring *r = &s->tx;
	size_t done = 0;
	int ret;

	writel(readl(s->mbox + MBOX_INT_ENABLE) | INT_TX, s->mbox + MBOX_INT_ENABLE);
	while (done < len) {
		struct mbox_desc *d = &r->desc[r->head];
		size_t n = min_t(size_t, len - done, BUF_SIZE);

		ret = wait_event_interruptible(s->tx_wait, !(READ_ONCE(d->ctrl) & DESC_OWN));
		if (ret)
			return done ? done : ret;
		if (copy_from_user(r->buf + r->head * BUF_SIZE, ubuf + done, n))
			return done ? done : -EFAULT;
		wmb();
		WRITE_ONCE(d->ctrl, n | (BUF_SIZE << 12) | DESC_EOM | DESC_OWN);
		r->head = (r->head + 1) % r->n;
		done += n;
	}
	return done;
}

static ssize_t hi2s_read(struct file *f, char __user *ubuf, size_t len, loff_t *off)
{
	struct hi2s *s = to_hi2s(f);
	struct ring *r = &s->rx;
	struct mbox_desc *d;
	int ret;

	if (len < BUF_SIZE)
		return -EINVAL;
	writel(readl(s->mbox + MBOX_INT_ENABLE) | INT_RX, s->mbox + MBOX_INT_ENABLE);
	if (!r->started) {
		writel(r->desc_dma, s->mbox + MBOX_RX_DESC);
		writel(DMA_START, s->mbox + MBOX_RX_CTRL);
		r->started = true;
	}
	d = &r->desc[r->head];
	ret = wait_event_interruptible(s->rx_wait, !(READ_ONCE(d->ctrl) & DESC_OWN));
	if (ret)
		return ret;
	rmb();
	if (copy_to_user(ubuf, r->buf + r->head * BUF_SIZE, BUF_SIZE))
		return -EFAULT;
	WRITE_ONCE(d->ctrl, BUF_SIZE | (BUF_SIZE << 12) | DESC_OWN);
	r->head = (r->head + 1) % r->n;
	writel(DMA_RESUME, s->mbox + MBOX_RX_CTRL);
	return BUF_SIZE;
}

static u32 dsize_config(int bits)
{
	switch (bits) {
	case 8:  return 0xa00302;
	case 16: return 0xa21302;
	case 24: return 0xa22b02;
	case 32: return 0xa23b02;
	}
	return 0;
}

static long hi2s_ioctl(struct file *f, unsigned int cmd, unsigned long arg)
{
	struct hi2s *s = to_hi2s(f);
	unsigned long ports;
	u32 v;
	int i;

	switch (cmd) {
	case I2S_VOLUME: {
		int vol = (int)arg;

		if (vol < 15)
			v = vol < 0 ? 0x1f : ((~vol & 0xf) | 0x10);
		else
			v = vol < 23 ? vol - 15 : 7;
		writel(v | (v << 8), s->stereo + ST_VOLUME);
		return 0;
	}
	case I2S_FREQ:
		if (readl(s->stereo + ST_CLK_DIV) == (u32)arg)
			return 0;
		if (s->tx.started)
			writel(DMA_PAUSE, s->mbox + MBOX_TX_CTRL);
		if (s->rx.started)
			writel(DMA_PAUSE, s->mbox + MBOX_RX_CTRL);
		writel((u32)arg, s->stereo + ST_CLK_DIV);
		stereo_reset(s);
		if (s->tx.started)
			writel(DMA_RESUME, s->mbox + MBOX_TX_CTRL);
		if (s->rx.started)
			writel(DMA_RESUME, s->mbox + MBOX_RX_CTRL);
		return 0;
	case I2S_DSIZE:
		v = dsize_config((int)arg);
		if (!v)
			return -ENOTSUPP;
		writel(v, s->stereo + ST_CONFIG);
		stereo_reset(s);
		return 0;
	case I2S_MODE:
		v = readl(s->stereo + ST_CONFIG);
		v = arg == 2 ? v & ~ST_CONFIG_SPKR : v | ST_CONFIG_SPKR;
		writel(v, s->stereo + ST_CONFIG);
		return 0;
	case I2S_START:
		ports = 0;
		for (i = 0; i < NR_PORTS; i++)
			if (arg & BIT(16 + i))
				ports |= BIT(i);
		gpiod_set_array_value(s->ports->ndescs, s->ports->desc, s->ports->info, &ports);
		if (!s->tx.open)
			return 0;
		if (!s->tx.started) {
			writel(s->tx.desc_dma, s->mbox + MBOX_TX_DESC);
			writel(DMA_START, s->mbox + MBOX_TX_CTRL);
			s->tx.started = true;
		} else {
			writel(DMA_RESUME, s->mbox + MBOX_TX_CTRL);
		}
		return 0;
	case I2S_DRAIN:
		if (!s->tx.open)
			return 0;
		return wait_event_interruptible_timeout(s->tx_wait, tx_idle(s), 10 * HZ) > 0 ? 0 : -ETIMEDOUT;
	}
	return -ENOTTY;
}

static const struct file_operations hi2s_fops = {
	.owner		= THIS_MODULE,
	.open		= hi2s_open,
	.release	= hi2s_release,
	.read		= hi2s_read,
	.write		= hi2s_write,
	.unlocked_ioctl	= hi2s_ioctl,
	.llseek		= noop_llseek,
};

static int hi2s_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct resource *res;
	struct hi2s *s;
	int irq, ret;

	s = devm_kzalloc(dev, sizeof(*s), GFP_KERNEL);
	if (!s)
		return -ENOMEM;
	s->dev = dev;
	mutex_init(&s->lock);
	spin_lock_init(&s->irq_lock);
	init_waitqueue_head(&s->tx_wait);
	init_waitqueue_head(&s->rx_wait);

	s->mbox = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(s->mbox))
		return PTR_ERR(s->mbox);
	s->stereo = devm_platform_ioremap_resource(pdev, 1);
	if (IS_ERR(s->stereo))
		return PTR_ERR(s->stereo);
	/* GPIO_FUNCTION_1 sits in the GPIO controller's range: map it without claiming it */
	res = platform_get_resource(pdev, IORESOURCE_MEM, 2);
	if (!res)
		return -EINVAL;
	s->gpio_func1 = devm_ioremap(dev, res->start, resource_size(res));
	if (!s->gpio_func1)
		return -ENOMEM;

	s->ports = devm_gpiod_get_array(dev, "ir", GPIOD_OUT_LOW);
	if (IS_ERR(s->ports))
		return dev_err_probe(dev, PTR_ERR(s->ports), "ir-gpios\n");
	if (s->ports->ndescs != NR_PORTS)
		return dev_err_probe(dev, -EINVAL, "need %d ir-gpios\n", NR_PORTS);
	s->rst = devm_reset_control_get_exclusive(dev, NULL);
	if (IS_ERR(s->rst))
		return dev_err_probe(dev, PTR_ERR(s->rst), "reset\n");

	ret = dma_set_mask_and_coherent(dev, DMA_BIT_MASK(32));
	if (ret)
		return ret;
	irq = platform_get_irq(pdev, 0);
	if (irq < 0)
		return irq;
	ret = devm_request_irq(dev, irq, hi2s_irq, 0, "harmony-i2s", s);
	if (ret)
		return ret;

	/* as ath_i2s.ko init_module: pin functions, reset pulse, clock and 16-bit config */
	writel(readl(s->gpio_func1) | GPIO_FUNC1_I2S, s->gpio_func1);
	reset_control_assert(s->rst);
	udelay(10);
	reset_control_deassert(s->rst);
	mdelay(1);
	writel(ST_CLK_DIV_DEFAULT, s->stereo + ST_CLK_DIV);
	writel(ST_CONFIG_16BIT, s->stereo + ST_CONFIG);
	stereo_reset(s);

	s->misc.minor = MISC_DYNAMIC_MINOR;
	s->misc.name = "i2s";
	s->misc.fops = &hi2s_fops;
	s->misc.mode = 0666;
	ret = misc_register(&s->misc);
	if (ret)
		return ret;
	platform_set_drvdata(pdev, s);
	dev_info(dev, "/dev/i2s ready (irq %d)\n", irq);
	return 0;
}

static void hi2s_remove(struct platform_device *pdev)
{
	struct hi2s *s = platform_get_drvdata(pdev);

	misc_deregister(&s->misc);
}

static const struct of_device_id hi2s_of_match[] = {
	{ .compatible = "logitech,harmony-hub-i2s" },
	{}
};
MODULE_DEVICE_TABLE(of, hi2s_of_match);

static struct platform_driver hi2s_driver = {
	.probe	= hi2s_probe,
	.remove	= hi2s_remove,
	.driver	= {
		.name = "harmony-i2s",
		.of_match_table = hi2s_of_match,
	},
};
module_platform_driver(hi2s_driver);

MODULE_DESCRIPTION("Harmony Hub IR blaster and receiver (/dev/i2s, ath_i2s.ko interface)");
MODULE_LICENSE("GPL");
