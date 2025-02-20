// SPDX-License-Identifier: GPL-2.0

#include <linux/leds.h>
#include <linux/module.h>
#include <linux/of_device.h>
#include <linux/spi/spi.h>
#include <uapi/linux/uleds.h>

/*
 *  SK9822 SPI protocol description:
 *  +------+----------------------------------------+------+
 *  |START |               DATA FIELD:              | END  |
 *  |FRAME |               N LED FRAMES             |FRAME |
 *  +------+------+------+------+------+-----+------+------+
 *  | 0*32 | LED1 | LED2 | LED3 | LED4 | --- | LEDN | 1*32 |
 *  +------+------+------+------+------+-----+------+------+
 *
 *  +-----------------------------------+
 *  |START FRAME 32bits                 |
 *  +--------+--------+--------+--------+
 *  |00000000|00000000|00000000|00000000|
 *  +--------+--------+--------+--------+
 *
 *  +------------------------------------+
 *  |LED  FRAME 32bits                   |
 *  +---+-----+--------+--------+--------+
 *  |111|LUMA |  BLUE  | GREEN  |  RED   |
 *  +---+-----+--------+--------+--------+
 *  |3b |5bits| 8bits  | 8bits  | 8bits  |
 *  +---+-----+--------+--------+--------+
 *  |MSB   LSB|MSB  LSB|MSB  LSB|MSB  LSB|
 *  +---+-----+--------+--------+--------+
 *
 *  +-----------------------------------+
 *  |END FRAME 32bits                   |
 *  +--------+--------+--------+--------+
 *  |11111111|11111111|11111111|11111111|
 *  +--------+--------+--------+--------+
 */

/* sk9822 default settings */
#define CR_MAX_BRIGHTNESS	GENMASK(7, 0)
#define LM_MAX_BRIGHTNESS	GENMASK(4, 0)
#define CH_NUM			4
#define START_BYTE		0
#define END_BYTE		GENMASK(7, 0)
#define LED_FRAME_HEADER	GENMASK(7, 5)

enum led_channels {
	RED,
	GREEN,
	BLUE,
	LUMA,
};

struct sk9822_led {
	char			name[LED_MAX_NAME_SIZE];
	struct sk9822		*priv;
	struct led_classdev	ldev;
	u8			brightness;
};

struct sk9822 {
	size_t			led_count;
	struct device		*dev;
	struct mutex		lock;
	struct spi_device	*spi;
	u8			*buf;
	struct sk9822_led	leds[];
};

static int sk9822_sync(struct sk9822 *priv)
{
	int	ret;
	size_t	i;
	size_t	bytes = 0;

	for (i = 0; i < 4; i++)
		priv->buf[bytes++] = START_BYTE;

	for (i = 0; i < priv->led_count; i++) {
		priv->buf[bytes++] = LED_FRAME_HEADER |
				     priv->leds[i * CH_NUM + LUMA].brightness;
		priv->buf[bytes++] = priv->leds[i * CH_NUM + BLUE].brightness;
		priv->buf[bytes++] = priv->leds[i * CH_NUM + GREEN].brightness;
		priv->buf[bytes++] = priv->leds[i * CH_NUM + RED].brightness;
	}

	for (i = 0; i < 4; i++)
		priv->buf[bytes++] = END_BYTE;

	ret = spi_write(priv->spi, priv->buf, bytes);

	return ret;
}

static int sk9822_set_sync(struct led_classdev *ldev,
			   enum led_brightness brightness)
{
	int			ret;
	struct sk9822_led	*led = container_of(ldev,
						    struct sk9822_led,
						    ldev);

	dev_dbg(led->priv->dev, "Set brightness of %s to %d\n",
		led->name, brightness);

	mutex_lock(&led->priv->lock);
	led->brightness = (u8)brightness;
	ret = sk9822_sync(led->priv);
	mutex_unlock(&led->priv->lock);

	return ret;
}

static int sk9822_probe_dt(struct sk9822 *priv)
{
	u32			i = 0;
	int			j = 0;
	struct sk9822_led	*led;
	struct fwnode_handle	*child;
	struct device_node	*np;
	int			ret;
	const char		*str;
	static const char	* const rgb_name[] = {"red",
						      "green",
						      "blue",
						      "luma"};

	device_for_each_child_node(priv->dev, child) {
		np = to_of_node(child);
		/* for each physical LED, 4 LEDs are created representing
		 * the 4 components: red, green, blue and global luma.
		 */
		for (j = 0; j < CH_NUM; j++) {
			ret = fwnode_property_read_u32(child, "reg", &i);
			if (ret)
				return ret;

			if (i >= priv->led_count)
				return -EINVAL;

			led = &priv->leds[i * CH_NUM + j];
			ret = fwnode_property_read_string(child, "label", &str);
			if (ret)
				snprintf(led->name, sizeof(led->name),
					 "sk9822:%s:%d", rgb_name[j], i);
			else
				snprintf(led->name, sizeof(led->name),
					 "sk9822:%s:%s", rgb_name[j], str);

			fwnode_property_read_string(child,
						    "linux,default-trigger",
						    &led->ldev.default_trigger);

			led->priv			  = priv;
			led->ldev.name			  = led->name;
			if (j == LUMA) {
				led->ldev.brightness	 = led->brightness
							 = LM_MAX_BRIGHTNESS;
				led->ldev.max_brightness = LM_MAX_BRIGHTNESS;
			} else {
				led->ldev.brightness	 = led->brightness
							 = 0;
				led->ldev.max_brightness = CR_MAX_BRIGHTNESS;
			}

			led->ldev.brightness_set_blocking = sk9822_set_sync;

			ret = devm_led_classdev_register(priv->dev, &led->ldev);
			if (ret) {
				dev_err(priv->dev,
					"failed to register LED %s, err %d",
					led->name, ret);
				fwnode_handle_put(child);
				return ret;
			}

			led->ldev.dev->of_node = np;

		}
	}

	return 0;
}

static int sk9822_probe(struct spi_device *spi)
{
	struct sk9822	*priv;
	size_t		led_count;
	int		ret;

	led_count = device_get_child_node_count(&spi->dev);
	if (!led_count) {
		dev_err(&spi->dev, "No LEDs defined in device tree!");
		return -ENODEV;
	}

	priv = devm_kzalloc(&spi->dev,
			    struct_size(priv, leds, led_count * CH_NUM),
			    GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->buf = devm_kzalloc(&spi->dev, led_count * CH_NUM + 8, GFP_KERNEL);
	if (!priv->buf)
		return -ENOMEM;

	mutex_init(&priv->lock);
	priv->led_count	= led_count;
	priv->dev	= &spi->dev;
	priv->spi	= spi;

	ret = sk9822_probe_dt(priv);
	if (ret)
		return ret;

	/* Set the LEDs with default values at start */
	sk9822_sync(priv);
	if (ret)
		return ret;

	spi_set_drvdata(spi, priv);

	return 0;
}

static int sk9822_remove(struct spi_device *spi)
{
	struct sk9822 *priv = spi_get_drvdata(spi);

	mutex_destroy(&priv->lock);

	return 0;
}

static const struct of_device_id sk9822_dt_ids[] = {
	{ .compatible = "opsco,sk9822", },
	{},
};

MODULE_DEVICE_TABLE(of, sk9822_dt_ids);

static struct spi_driver sk9822_driver = {
	.probe		= sk9822_probe,
	.remove		= sk9822_remove,
	.driver = {
		.name		= KBUILD_MODNAME,
		.of_match_table	= sk9822_dt_ids,
	},
};

module_spi_driver(sk9822_driver);

MODULE_AUTHOR("Nicolas Belin <nbelin@baylibre.com>");
MODULE_DESCRIPTION("sk9822 LED driver");
MODULE_LICENSE("GPL v2");
MODULE_ALIAS("spi:sk9822");
