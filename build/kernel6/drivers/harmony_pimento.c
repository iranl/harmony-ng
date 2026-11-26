// SPDX-License-Identifier: GPL-2.0
/*
 * Pair button and front light of the Logitech Harmony Hub on Linux 6.x, with the interface of
 * the stock 2.6.31 kernel's built-in "Pimento" driver that HarmonyNG uses:
 *
 *   input device "Pimento" (input0, /dev/input/event0): the Pair button as key 30
 *   /sys/class/input/input0/led: red mode in bits 0-1, green in bits 2-3 (payload/source/hub_led.h)
 *       mode 0 off, 1 solid, 2 fast flash, 3 slow flash. Both flashing at one rate: out of phase
 *       (red and green alternate). Two different modes, neither off: fast is off, slow is solid.
 *   /sys/class/input/input0/tde: 0 (the stock factory test mode flag)
 *
 * GPIOs read from a stock Hub: button GPIO 11 (low when pressed), red GPIO 1 and green GPIO 17
 * (both lit when low).
 *
 * The probe also disables JTAG (GPIO_FUNCTION_1 bit 0, as the stock kernel did): the JTAG pins
 * are GPIO 6, 7, 8 and 11, which are the CC2544 ready line and chip select and the Pair button.
 * Load this module before harmony_cc2544.
 */
#include <linux/gpio/consumer.h>
#include <linux/input.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/timer.h>

#define PIMENTO_KEY	30
#define FAST_MS		125
#define SLOW_MS		500
#define FUNC1_JTAG_DISABLE	BIT(0)

enum { UNLIT, SOLID, FAST, SLOW };

struct pimento {
	struct input_dev *input;
	struct gpio_desc *button, *red, *green;
	struct timer_list timer;
	spinlock_t lock;
	int value;		/* last written led value */
	int red_mode, green_mode;
	unsigned int tick;	/* FAST_MS steps since the pattern started */
};

static void pimento_apply(struct pimento *p)
{
	unsigned int fast_phase = p->tick & 1;		/* toggles every FAST_MS */
	unsigned int slow_phase = (p->tick / (SLOW_MS / FAST_MS)) & 1;
	bool same_rate = p->red_mode == p->green_mode;
	int lit[2], modes[2] = { p->red_mode, p->green_mode };
	int i;

	for (i = 0; i < 2; i++) {
		int m = modes[i];

		/* two different modes, neither off: fast reads as off, slow as solid */
		if (modes[0] != modes[1] && modes[0] != UNLIT && modes[1] != UNLIT)
			m = m == FAST ? UNLIT : m == SLOW ? SOLID : m;
		switch (m) {
		case SOLID: lit[i] = 1; break;
		case FAST:  lit[i] = fast_phase ^ (same_rate && i); break;
		case SLOW:  lit[i] = slow_phase ^ (same_rate && i); break;
		default:    lit[i] = 0;
		}
	}
	gpiod_set_value(p->red, lit[0]);
	gpiod_set_value(p->green, lit[1]);
}

static bool pimento_flashing(struct pimento *p)
{
	return p->red_mode >= FAST || p->green_mode >= FAST;
}

static void pimento_timer(struct timer_list *t)
{
	struct pimento *p = timer_container_of(p, t, timer);
	unsigned long flags;

	spin_lock_irqsave(&p->lock, flags);
	p->tick++;
	pimento_apply(p);
	if (pimento_flashing(p))
		mod_timer(&p->timer, jiffies + msecs_to_jiffies(FAST_MS));
	spin_unlock_irqrestore(&p->lock, flags);
}

static ssize_t led_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct pimento *p = input_get_drvdata(to_input_dev(dev));

	return sysfs_emit(buf, "%d\n", p->value);
}

static ssize_t led_store(struct device *dev, struct device_attribute *attr, const char *buf,
			 size_t count)
{
	struct pimento *p = input_get_drvdata(to_input_dev(dev));
	unsigned long flags;
	int v, ret;

	ret = kstrtoint(buf, 0, &v);
	if (ret)
		return ret;
	if (v < 0 || v > 15)
		return -EINVAL;
	spin_lock_irqsave(&p->lock, flags);
	p->value = v;
	p->red_mode = v & 3;
	p->green_mode = (v >> 2) & 3;
	p->tick = 0;
	pimento_apply(p);
	if (pimento_flashing(p))
		mod_timer(&p->timer, jiffies + msecs_to_jiffies(FAST_MS));
	spin_unlock_irqrestore(&p->lock, flags);
	return count;
}
static DEVICE_ATTR_RW(led);

static ssize_t tde_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "0\n");
}
static DEVICE_ATTR_RO(tde);

static struct attribute *pimento_attrs[] = {
	&dev_attr_led.attr,
	&dev_attr_tde.attr,
	NULL
};
ATTRIBUTE_GROUPS(pimento);

static irqreturn_t pimento_button_irq(int irq, void *data)
{
	struct pimento *p = data;

	input_report_key(p->input, PIMENTO_KEY, gpiod_get_value_cansleep(p->button));
	input_sync(p->input);
	return IRQ_HANDLED;
}

static int pimento_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct resource *res;
	void __iomem *func1;
	struct pimento *p;
	int irq, ret;

	/* GPIO_FUNCTION_1 is in the GPIO controller's range: map it without claiming it */
	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res)
		return -EINVAL;
	func1 = devm_ioremap(dev, res->start, resource_size(res));
	if (!func1)
		return -ENOMEM;
	writel(readl(func1) | FUNC1_JTAG_DISABLE, func1);

	p = devm_kzalloc(dev, sizeof(*p), GFP_KERNEL);
	if (!p)
		return -ENOMEM;
	spin_lock_init(&p->lock);
	timer_setup(&p->timer, pimento_timer, 0);

	p->button = devm_gpiod_get(dev, "button", GPIOD_IN);
	if (IS_ERR(p->button))
		return dev_err_probe(dev, PTR_ERR(p->button), "button-gpios\n");
	p->red = devm_gpiod_get(dev, "red", GPIOD_OUT_LOW);
	if (IS_ERR(p->red))
		return dev_err_probe(dev, PTR_ERR(p->red), "red-gpios\n");
	p->green = devm_gpiod_get(dev, "green", GPIOD_OUT_LOW);
	if (IS_ERR(p->green))
		return dev_err_probe(dev, PTR_ERR(p->green), "green-gpios\n");

	p->input = devm_input_allocate_device(dev);
	if (!p->input)
		return -ENOMEM;
	p->input->name = "Pimento";
	p->input->dev.groups = pimento_groups;
	input_set_capability(p->input, EV_KEY, PIMENTO_KEY);
	input_set_drvdata(p->input, p);
	ret = input_register_device(p->input);
	if (ret)
		return ret;

	irq = gpiod_to_irq(p->button);
	if (irq < 0)
		return irq;
	ret = devm_request_threaded_irq(dev, irq, NULL, pimento_button_irq,
					IRQF_ONESHOT | IRQF_TRIGGER_RISING | IRQF_TRIGGER_FALLING,
					"button", p);
	if (ret)
		return ret;
	platform_set_drvdata(pdev, p);
	return 0;
}

static void pimento_remove(struct platform_device *pdev)
{
	struct pimento *p = platform_get_drvdata(pdev);

	timer_shutdown_sync(&p->timer);
}

static const struct of_device_id pimento_of_match[] = {
	{ .compatible = "logitech,harmony-hub-pimento" },
	{}
};
MODULE_DEVICE_TABLE(of, pimento_of_match);

static struct platform_driver pimento_driver = {
	.probe	= pimento_probe,
	.remove	= pimento_remove,
	.driver	= {
		.name = "harmony-pimento",
		.of_match_table = pimento_of_match,
	},
};
module_platform_driver(pimento_driver);

MODULE_DESCRIPTION("Harmony Hub Pair button and front light (stock Pimento interface)");
MODULE_LICENSE("GPL");
