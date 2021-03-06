/*
 * Intel Clanton Hill platform accelerometer driver
 *
 * Copyright(c) 2013 Intel Corporation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of version 2 of the GNU General Public License as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St - Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Contact Information:
 * Intel Corporation
 *
 * Derived from STMicroelectronics accelerometers driver by Denis Ciocca
 *
 * The Intel Clanton Hill platform hardware design includes an
 * STMicroelectronics LIS331DLH accelerometer, intended to be used mainly for
 * sensing orientation, movement and sudden impacts (e.g. vehicle collision)
 *
 * This driver plugs into the Linux Industrial-IO framework to provide a
 * standardised user-space application interface for retreiving data and events
 * from the accelerometer.
 *
 * The LIS331DLH is connected via I2C to the host CPU on the Clanton Hill
 * platform and so this driver registers to the kernel as an I2C device driver
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/errno.h>
#include <linux/types.h>
#include <linux/mutex.h>
#include <linux/interrupt.h>
#include <linux/i2c.h>
#include <linux/gpio.h>
#include <linux/irq.h>
#include <linux/iio/events.h>
#include <linux/iio/iio.h>
#include <linux/iio/sysfs.h>
#include <linux/iio/trigger.h>
#include <linux/iio/buffer.h>

#include <linux/iio/common/st_sensors.h>
#include <linux/iio/common/st_sensors_i2c.h>

#include <linux/platform_data/lis331dlh_intel_cln.h>

/* DEFAULT VALUE FOR SENSORS */
#define ST_ACCEL_DEFAULT_OUT_X_L_ADDR		0x28
#define ST_ACCEL_DEFAULT_OUT_Y_L_ADDR		0x2a
#define ST_ACCEL_DEFAULT_OUT_Z_L_ADDR		0x2c

/* FULLSCALE */
#define ST_ACCEL_FS_AVL_2G			2
#define ST_ACCEL_FS_AVL_4G			4
#define ST_ACCEL_FS_AVL_6G			6
#define ST_ACCEL_FS_AVL_8G			8
#define ST_ACCEL_FS_AVL_16G			16

/* CUSTOM VALUES FOR SENSOR 2 */
#define ST_ACCEL_2_WAI_EXP			0x32
#define ST_ACCEL_2_ODR_ADDR			0x20
#define ST_ACCEL_2_ODR_MASK			0x18
#define ST_ACCEL_2_ODR_AVL_50HZ_VAL		0x00
#define ST_ACCEL_2_ODR_AVL_100HZ_VAL		0x01
#define ST_ACCEL_2_ODR_AVL_400HZ_VAL		0x02
#define ST_ACCEL_2_ODR_AVL_1000HZ_VAL		0x03
#define ST_ACCEL_2_PW_ADDR			0x20
#define ST_ACCEL_2_PW_MASK			0xe0
#define ST_ACCEL_2_PW_DOWN			0x00
#define ST_ACCEL_2_PW_NORMAL		0x20
#define ST_ACCEL_2_CTRL_REG1_XEN	0x01
#define ST_ACCEL_2_CTRL_REG1_YEN	0x02
#define ST_ACCEL_2_CTRL_REG1_ZEN	0x04
#define ST_ACCEL_2_FS_ADDR			0x23
#define ST_ACCEL_2_FS_MASK			0x30
#define ST_ACCEL_2_FS_AVL_2_VAL			0X00
#define ST_ACCEL_2_FS_AVL_4_VAL			0X01
#define ST_ACCEL_2_FS_AVL_8_VAL			0x03
#define ST_ACCEL_2_FS_AVL_2_GAIN		IIO_G_TO_M_S_2(1000)
#define ST_ACCEL_2_FS_AVL_4_GAIN		IIO_G_TO_M_S_2(2000)
#define ST_ACCEL_2_FS_AVL_8_GAIN		IIO_G_TO_M_S_2(3900)
#define ST_ACCEL_2_BDU_ADDR			0x23
#define ST_ACCEL_2_BDU_MASK			0x80
#define ST_ACCEL_2_DRDY_IRQ_ADDR		0x22
#define ST_ACCEL_2_DRDY_IRQ_MASK		0x02
#define ST_ACCEL_2_THRESH_IRQ_ADDR		0x30
#define ST_ACCEL_2_THRESH_IRQ_MASK		0x7f
#define ST_ACCEL_2_INT1_CFG_ADDR		0x30
#define ST_ACCEL_2_INT1_SRC_ADDR		0x31
#define ST_ACCEL_2_INT1_THRESH_ADDR		0x32
#define ST_ACCEL_2_INT1_DURATION_ADDR		0x33
#define ST_ACCEL_2_INT2_CFG_ADDR		0x34
#define ST_ACCEL_2_INT2_SRC_ADDR		0x35
#define ST_ACCEL_2_INT2_THRESH_ADDR		0x36
#define ST_ACCEL_2_INT2_DURATION_ADDR		0x37
#define ST_ACCEL_2_INT_IA_MASK			0x40
#define ST_ACCEL_2_INT_LIR_MASK			0x05
#define ST_ACCEL_2_INT_SRC_HIGH_MASK	0x20
#define ST_ACCEL_2_INT_CFG_XLIE_EN		0x01
#define ST_ACCEL_2_INT_CFG_XHIE_EN		0x02
#define ST_ACCEL_2_INT_CFG_YLIE_EN		0x04
#define ST_ACCEL_2_INT_CFG_YHIE_EN		0x08
#define ST_ACCEL_2_INT_CFG_ZLIE_EN		0x10
#define ST_ACCEL_2_INT_CFG_ZHIE_EN		0x20

#define ST_ACCEL_2_MULTIREAD_BIT		true
#define CLN_ACCEL_INT2_WAKEUP_THRESH_VAL	0x7f

static const u8 iio_modifier_map[] = {
	IIO_NO_MOD,
	IIO_MOD_X,
	IIO_MOD_Y,
	IIO_MOD_X_AND_Y,
	IIO_MOD_Z,
	IIO_MOD_X_AND_Z,
	IIO_MOD_Y_AND_Z,
	IIO_MOD_X_AND_Y_AND_Z,
};

/*  Threshold event ISR bottom half.  This function reads interrupt status
 *  registers for INT1 to reset any active interrupt conditions
 *  and pushes an IIO event if a threshold interrupt was active.
 */
static irqreturn_t lis331dlh_intel_cln_threshold_event_handler(
	int irq,
	void *private)
{
	int err;
	u8 data;
	u8 mask;
	int i;
	u64 iio_modifier;

	struct st_sensor_data *sdata = iio_priv(private);
	s64 timestamp = iio_get_time_ns();
	err = sdata->tf->read_byte(&sdata->tb, sdata->dev,
			ST_ACCEL_2_INT1_SRC_ADDR,
			&data);

	if (err < 0)
		goto st_sensors_read_err;

	err = sdata->tf->read_byte(&sdata->tb, sdata->dev,
			ST_ACCEL_2_INT1_CFG_ADDR,
				&mask);

	if (err < 0)
		goto st_sensors_read_err;

	if (data & ST_ACCEL_2_INT_IA_MASK) {
		data &= mask;

		iio_modifier = 0;
		for (i = 0; i < ST_SENSORS_NUMBER_DATA_CHANNELS; i++) {
			iio_modifier <<= 1;
			iio_modifier += !!(data & ST_ACCEL_2_INT_SRC_HIGH_MASK);
			data <<= 2;
		}

		iio_modifier = iio_modifier_map[iio_modifier];

		iio_push_event(private,
				IIO_EVENT_CODE(IIO_ACCEL,
				0, /* non differential */
				iio_modifier,
				IIO_EV_TYPE_THRESH,
				IIO_EV_DIR_RISING, 0, 0, 0),
				timestamp);
	}

st_sensors_read_err:
	return IRQ_HANDLED;
}

static int lis331dlh_intel_cln_read_raw(
	struct iio_dev *indio_dev,
	struct iio_chan_spec const *ch,
	int *val, int *val2, long mask)
{
	int err;
	struct st_sensor_data *adata = iio_priv(indio_dev);

	switch (mask) {
	case IIO_CHAN_INFO_RAW:
		err = st_sensors_read_info_raw(indio_dev, ch, val);
		if (err < 0)
			goto read_error;

		return IIO_VAL_INT;
	case IIO_CHAN_INFO_SCALE:
		*val = 0;
		*val2 = adata->current_fullscale->gain;
		return IIO_VAL_INT_PLUS_MICRO;
	default:
		return -EINVAL;
	}

read_error:
	return err;
}

static int lis331dlh_intel_cln_write_raw(
	struct iio_dev *indio_dev,
	struct iio_chan_spec const *chan,
	int val, int val2, long mask)
{
	int err;

	switch (mask) {
	case IIO_CHAN_INFO_SCALE:
		err = st_sensors_set_fullscale_by_gain(indio_dev, val2);
		break;
	default:
		return -EINVAL;
	}

	return err;
}


static ST_SENSOR_DEV_ATTR_SAMP_FREQ();
static ST_SENSORS_DEV_ATTR_SAMP_FREQ_AVAIL();
static ST_SENSORS_DEV_ATTR_SCALE_AVAIL(in_accel_scale_available);

static struct attribute *lis331dlh_intel_cln_attributes[] = {
	&iio_dev_attr_sampling_frequency_available.dev_attr.attr,
	&iio_dev_attr_in_accel_scale_available.dev_attr.attr,
	&iio_dev_attr_sampling_frequency.dev_attr.attr,
	NULL,
};

static const struct attribute_group lis331dlh_intel_cln_attribute_group = {
	.attrs = lis331dlh_intel_cln_attributes,
};

static int lis331dlh_intel_cln_read_event_value(
	struct iio_dev *indio_dev,
	u64 event_code,
	int *val)
{
	int err;
	u8 data;
	struct st_sensor_data *sdata = iio_priv(indio_dev);

	err = sdata->tf->read_byte(&sdata->tb, sdata->dev,
				ST_ACCEL_2_INT1_THRESH_ADDR, &data);

	*val = (int) data;
	return err;
}

static int lis331dlh_intel_cln_write_event_value(
	struct iio_dev *indio_dev,
	u64 event_code,
	int val)
{
	int err;
	struct st_sensor_data *sdata;

	/* range check */
	if ((val < 0) || (val > 0x7f))
		return -EINVAL;

	sdata = iio_priv(indio_dev);

	err = sdata->tf->write_byte(&sdata->tb, sdata->dev,
				ST_ACCEL_2_INT1_THRESH_ADDR, val);

	return err;

}

/*  Configure the INT1 pin to fire an interrupt on a high threshold event.
 */
static int lis331dlh_intel_cln_configure_threshold_interrupt(
	struct iio_dev *indio_dev, bool state)
{
	int err = 0;
	struct st_sensor_data *sdata = iio_priv(indio_dev);

	if (sdata->int_thresh == state)
		return 0;

	if (state) {
		err = request_threaded_irq(sdata->get_irq_data_ready(indio_dev),
				NULL,
				lis331dlh_intel_cln_threshold_event_handler,
				IRQF_TRIGGER_RISING | IRQF_ONESHOT,
				"lis331dlh_intel_cln_threshold",
				indio_dev);
		if (err == 0) {
			sdata->int_thresh = true;
			err = sdata->tf->write_byte(
				&sdata->tb, sdata->dev,
				ST_ACCEL_2_INT1_DURATION_ADDR, 1);
		}
	} else {
		free_irq(sdata->get_irq_data_ready(indio_dev), indio_dev);
		sdata->int_thresh = false;
	}

	return err;
}

static int lis331dlh_intel_cln_read_event_config(
	struct iio_dev *indio_dev,
	u64 event_code)
{
	int err = 0;
	u8 data, mask;
	struct st_sensor_data *sdata = iio_priv(indio_dev);

	err = sdata->tf->read_byte(&sdata->tb, sdata->dev,
				   ST_ACCEL_2_INT1_CFG_ADDR,
				   &data);

	mask = 1 << ((IIO_EVENT_CODE_EXTRACT_MODIFIER(event_code) << 1) - 1);

	return !!(data & mask);
}

static int lis331dlh_intel_cln_write_event_config(
	struct iio_dev *indio_dev,
	u64 event_code,
	int state)
{
	int err;
	u8 data;
	u8 mask;

	bool new_int_state;

	struct st_sensor_data *sdata = iio_priv(indio_dev);
	mask = 1 << ((IIO_EVENT_CODE_EXTRACT_MODIFIER(event_code) << 1) - 1);

	err = st_sensors_write_data_with_mask(indio_dev,
					      ST_ACCEL_2_INT1_CFG_ADDR,
					      mask, state);

	if (err == 0)
		err = sdata->tf->read_byte(&sdata->tb, sdata->dev,
					   ST_ACCEL_2_INT1_CFG_ADDR, &data);

	if (err == 0) {
		new_int_state = data & (ST_ACCEL_2_INT_CFG_XHIE_EN |
					ST_ACCEL_2_INT_CFG_YHIE_EN |
					ST_ACCEL_2_INT_CFG_ZHIE_EN);
		err = lis331dlh_intel_cln_configure_threshold_interrupt(
			indio_dev, new_int_state);
	}

	return err;
}

/*  Configure the INT2 pin to fire an interrupt on a threshold high event.  INT2
 *  should be wired to a suspend well IRQ to wake up the host.
 */
static int lis331dlh_intel_cln_enable_wakeup_interrupt(
	struct iio_dev *indio_dev)
{
	int err = 0;
	u8 data;
	struct st_sensor_data *sdata = iio_priv(indio_dev);

	if (err == 0)
		err = sdata->tf->write_byte(&sdata->tb, sdata->dev,
					ST_ACCEL_2_INT2_THRESH_ADDR,
					CLN_ACCEL_INT2_WAKEUP_THRESH_VAL);

	/* Latch interrupt request on INT2 */
	if (err == 0)
		err = st_sensors_write_data_with_mask(
			indio_dev, ST_ACCEL_2_DRDY_IRQ_ADDR,
			ST_ACCEL_2_INT_LIR_MASK, 1);

	if (err == 0)
		err = sdata->tf->write_byte(&sdata->tb, sdata->dev,
					    ST_ACCEL_2_INT2_DURATION_ADDR, 0);

	if (err == 0)
		err = sdata->tf->write_byte(&sdata->tb, sdata->dev,
					    ST_ACCEL_2_INT2_CFG_ADDR,
					    ST_ACCEL_2_INT_CFG_XHIE_EN |
					    ST_ACCEL_2_INT_CFG_YHIE_EN);

	/* Clean ST_ACCEL_2_INT2_SRC */
	if (err == 0)
		err = sdata->tf->read_byte(&sdata->tb, sdata->dev,
					   ST_ACCEL_2_INT2_SRC_ADDR,
					   &data);

	return err;
}

static int lis331dlh_intel_cln_disable_wakeup_interrupt(
	struct iio_dev *indio_dev)
{
	int err = 0;
	u8 data;
	struct st_sensor_data *sdata = iio_priv(indio_dev);

	if (err == 0)
		err = sdata->tf->write_byte(&sdata->tb, sdata->dev,
					    ST_ACCEL_2_INT2_CFG_ADDR,
					    0);

	/* Clean ST_ACCEL_2_INT2_SRC */
	if (err == 0)
		err = sdata->tf->read_byte(&sdata->tb, sdata->dev,
					   ST_ACCEL_2_INT2_SRC_ADDR,
					   &data);

	return err;
}

static int lis331dlh_intel_cln_handle_wakeup_interrupt(
	struct iio_dev *indio_dev)
{
	int err;
	u8 data;
	struct st_sensor_data *sdata = iio_priv(indio_dev);
	s64 timestamp = iio_get_time_ns();

	err = sdata->tf->read_byte(&sdata->tb, sdata->dev,
				   ST_ACCEL_2_INT2_SRC_ADDR,
				   &data);

	if (err == 0)
		if (data & ST_ACCEL_2_INT_IA_MASK) {
			iio_push_event(indio_dev,
					IIO_EVENT_CODE(IIO_ACCEL,
					0, /* non differential */
					IIO_MOD_X_OR_Y_OR_Z,
					IIO_EV_TYPE_THRESH,
					IIO_EV_DIR_EITHER, 0, 0, 0),
					timestamp);
		}

	return err;
}

static const struct iio_info accel_info = {
	.driver_module = THIS_MODULE,
	.attrs = &lis331dlh_intel_cln_attribute_group,
	.read_raw = &lis331dlh_intel_cln_read_raw,
	.write_raw = &lis331dlh_intel_cln_write_raw,
	.read_event_config = &lis331dlh_intel_cln_read_event_config,
	.write_event_config = &lis331dlh_intel_cln_write_event_config,
	.read_event_value = &lis331dlh_intel_cln_read_event_value,
	.write_event_value = &lis331dlh_intel_cln_write_event_value,
};

static const struct iio_chan_spec st_accel_12bit_channels[] = {
	ST_SENSORS_LSM_CHANNELS(IIO_ACCEL, ST_SENSORS_SCAN_X, IIO_MOD_X, IIO_LE,
		ST_SENSORS_DEFAULT_12_REALBITS, ST_ACCEL_DEFAULT_OUT_X_L_ADDR),
	ST_SENSORS_LSM_CHANNELS(IIO_ACCEL, ST_SENSORS_SCAN_Y, IIO_MOD_Y, IIO_LE,
		ST_SENSORS_DEFAULT_12_REALBITS, ST_ACCEL_DEFAULT_OUT_Y_L_ADDR),
	ST_SENSORS_LSM_CHANNELS(IIO_ACCEL, ST_SENSORS_SCAN_Z, IIO_MOD_Z, IIO_LE,
		ST_SENSORS_DEFAULT_12_REALBITS, ST_ACCEL_DEFAULT_OUT_Z_L_ADDR),
	IIO_CHAN_SOFT_TIMESTAMP(3)
};

static struct st_sensors lis331dlh_intel_cln_sensor = {
	.wai = ST_ACCEL_2_WAI_EXP,
	.sensors_supported = {
		[0] = "lis331dlh_cln",
	},
	.ch = (struct iio_chan_spec *)st_accel_12bit_channels,
	.odr = {
		.addr = ST_ACCEL_2_ODR_ADDR,
		.mask = ST_ACCEL_2_ODR_MASK,
		.odr_avl = {
			{ 50, ST_ACCEL_2_ODR_AVL_50HZ_VAL, },
			{ 100, ST_ACCEL_2_ODR_AVL_100HZ_VAL, },
			{ 400, ST_ACCEL_2_ODR_AVL_400HZ_VAL, },
			{ 1000, ST_ACCEL_2_ODR_AVL_1000HZ_VAL, },
		},
	},
	.pw = {
		.addr = ST_ACCEL_2_PW_ADDR,
		.mask = ST_ACCEL_2_PW_MASK,
		.value_on = ST_SENSORS_DEFAULT_POWER_ON_VALUE,
		.value_off = ST_SENSORS_DEFAULT_POWER_OFF_VALUE,
	},
	.enable_axis = {
		.addr = ST_SENSORS_DEFAULT_AXIS_ADDR,
		.mask = ST_SENSORS_DEFAULT_AXIS_MASK,
	},
	.fs = {
		.addr = ST_ACCEL_2_FS_ADDR,
		.mask = ST_ACCEL_2_FS_MASK,
		.fs_avl = {
			[0] = {
				.num = ST_ACCEL_FS_AVL_2G,
				.value = ST_ACCEL_2_FS_AVL_2_VAL,
				.gain = ST_ACCEL_2_FS_AVL_2_GAIN,
			},
			[1] = {
				.num = ST_ACCEL_FS_AVL_4G,
				.value = ST_ACCEL_2_FS_AVL_4_VAL,
				.gain = ST_ACCEL_2_FS_AVL_4_GAIN,
			},
			[2] = {
				.num = ST_ACCEL_FS_AVL_8G,
				.value = ST_ACCEL_2_FS_AVL_8_VAL,
				.gain = ST_ACCEL_2_FS_AVL_8_GAIN,
			},
		},
	},
	.bdu = {
		.addr = ST_ACCEL_2_BDU_ADDR,
		.mask = ST_ACCEL_2_BDU_MASK,
	},
	.drdy_irq = {
		.addr = ST_ACCEL_2_DRDY_IRQ_ADDR,
		.mask = ST_ACCEL_2_DRDY_IRQ_MASK,
	},
	.multi_read_bit = ST_ACCEL_2_MULTIREAD_BIT,
	.bootime = 2,
};

static int lis331dlh_intel_cln_probe(
	struct i2c_client *client,
	const struct i2c_device_id *id)
{
	struct iio_dev *indio_dev;
	struct st_sensor_data *adata;
	struct lis331dlh_intel_cln_platform_data *pdata;
	int ret = 0;

	indio_dev = iio_device_alloc(sizeof(*adata));
	if (indio_dev == NULL) {
		ret = -ENOMEM;
		goto iio_device_alloc_error;
	}

	i2c_set_clientdata(client, indio_dev);
	indio_dev->dev.parent = &client->dev;
	indio_dev->name = client->name;

	adata = iio_priv(indio_dev);
	adata->dev = &client->dev;

	pdata = client->dev.platform_data;
	if (!pdata) {
		pr_err("No platform data provided\n");
		goto lis331dlh_intel_cln_init_err;
	}

	ret = gpio_to_irq(pdata->irq1_pin);
	if (ret < 0) {
		pr_err(
			"Failed to obtain valid IRQ for GPIO %d, "
			"gpio_to_irq returned %d\n",
			pdata->irq1_pin, ret);
		goto lis331dlh_intel_cln_init_err;
	}
	to_i2c_client(adata->dev)->irq = ret;

	st_sensors_i2c_configure(indio_dev, client, adata);

	indio_dev->modes = INDIO_DIRECT_MODE;
	indio_dev->info = &accel_info;

	ret = st_sensors_check_device_support(indio_dev,
					      1, &lis331dlh_intel_cln_sensor);
	if (ret < 0)
		goto lis331dlh_intel_cln_init_err;

	indio_dev->channels = adata->sensor->ch;
	indio_dev->num_channels = ST_SENSORS_NUMBER_ALL_CHANNELS;

	adata->multiread_bit = adata->sensor->multi_read_bit;
	adata->current_fullscale = (struct st_sensor_fullscale_avl *)
		&adata->sensor->fs.fs_avl[0];
	adata->odr = adata->sensor->odr.odr_avl[0].hz;
	adata->int_thresh = false;

	ret = st_sensors_init_sensor(indio_dev);
	if (ret < 0)
		goto lis331dlh_intel_cln_init_err;

	ret = iio_device_register(indio_dev);
	if (ret)
		goto lis331dlh_intel_cln_init_err;

	return 0;

lis331dlh_intel_cln_init_err:
	iio_device_free(indio_dev);
iio_device_alloc_error:
	return ret;
}

static int lis331dlh_intel_cln_remove(
	struct i2c_client *client)
{
	struct iio_dev *indio_dev = i2c_get_clientdata(client);
	struct st_sensor_data *adata = iio_priv(indio_dev);

	st_sensors_set_enable(indio_dev, false);

	if (adata->int_thresh)
		free_irq(adata->get_irq_data_ready(indio_dev), indio_dev);

	iio_device_unregister(indio_dev);

	iio_device_free(indio_dev);

	return 0;
}

#ifdef CONFIG_PM
static int lis331dlh_intel_cln_suspend(
	struct device *dev)
{
	struct iio_dev *indio_dev = dev_get_drvdata(dev);

	lis331dlh_intel_cln_enable_wakeup_interrupt(indio_dev);

	return 0;
}

static int lis331dlh_intel_cln_resume(
	struct device *dev)
{
	struct iio_dev *indio_dev = dev_get_drvdata(dev);

	lis331dlh_intel_cln_handle_wakeup_interrupt(indio_dev);
	lis331dlh_intel_cln_disable_wakeup_interrupt(indio_dev);

	return 0;
}

static const struct dev_pm_ops lis331dlh_intel_cln_pm_ops = {
	.suspend = lis331dlh_intel_cln_suspend,
	.resume = lis331dlh_intel_cln_resume,
};

#define LIS331DLH_INTEL_CLN_PM_OPS (&lis331dlh_intel_cln_pm_ops)
#else
#define LIS331DLH_INTEL_CLN_PM_OPS NULL
#endif

static const struct i2c_device_id lis331dlh_intel_cln_id_table[] = {
	{ "lis331dlh_cln" },
	{},
};
MODULE_DEVICE_TABLE(i2c, lis331dlh_intel_cln_id_table);

static struct i2c_driver lis331dlh_intel_cln_driver = {
	.driver = {
		.owner = THIS_MODULE,
		.name = "lis331dlh_cln",
		.pm	= LIS331DLH_INTEL_CLN_PM_OPS,
	},
	.probe = lis331dlh_intel_cln_probe,
	.remove = lis331dlh_intel_cln_remove,
	.id_table = lis331dlh_intel_cln_id_table,
};

module_i2c_driver(lis331dlh_intel_cln_driver);

MODULE_AUTHOR("Wojciech Ziemba <wojciech.ziemba@emutex.com>");
MODULE_DESCRIPTION("STMicroelectronics LIS331DLH accelerometer i2c driver for Intel Clanton platform");
MODULE_LICENSE("GPL v2");
