/*
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
 */
/**
 * intel_cln_smep_test.c
 *
 * Simple test module to verify SMEP works as expected on MIA
 * DO NOT RELEASE THIS FILE OUTSIDE OF CLANTON GROUP
 * DO NOT ATTEMPT TO UPSTREAM THIS CODE - YOU WILL BE PUBLICLY EMBARRSSED !
 *
 * Author : Bryan O'Donoghue <bryan.odonoghue@intel.com>
 *
 */
#include <asm/processor.h>
#include <asm/processor-flags.h>
#include <linux/cdev.h>
#include <linux/crc16.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/fs.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/printk.h>
#include <linux/slab.h>

#define DRIVER_NAME			"intel_cln_smep_test"

/**
 * struct intel_cln_smep_dev
 *
 * Structre to represent module state/data/etc
 */
struct intel_cln_smep_test_dev{
	unsigned int opened;
	struct platform_device *pldev;	/* Platform device */
	struct cdev cdev;
	struct mutex open_lock;
	char * pdata;
	u32 size;
};

static struct intel_cln_smep_test_dev smep_test_dev;
static struct class *smep_test_class;
static DEFINE_MUTEX(smep_test_mutex);
static int smep_test_major;
static char * name = "testmap";

/**
 * smep_test_ioctl
 *
 * Allows user-space to command kernel switch SMEP on/off
 */
static long smep_test_ioctl(struct file *file, unsigned int cmd,
		unsigned long arg)
{
	int cr4 = 0;

	cr4 = read_cr4();
	printk(KERN_INFO "%s entry CR4 is 0x%08x\n", __FUNCTION__, cr4);

	switch(cmd){
		case 0:
			printk(KERN_INFO "Switching SMEP off\n");
			cr4 &= ~X86_CR4_SMEP;
			
			break;
		case 1:
			printk(KERN_INFO "Switching SMEP on\n");
			cr4 |= X86_CR4_SMEP;
			break;
		default:
			return -EINVAL;
	}
	/* Latch value */
	write_cr4(cr4);

	/* Print contents of CR4 */
	cr4 = read_cr4();
	printk(KERN_INFO "%s exit CR4 is 0x%08x\n", __FUNCTION__, cr4);
	
	return 0;
}

/**
 * smep_test_write
 *
 * Accepts a buffer from user-space and then tries to execute the contents
 * Be very careful
 */
static ssize_t smep_test_write(struct file *file, const char __user *buf,
						size_t count, loff_t *ppos)
{
	/* 
	 * We assume we are passed a pointer to function of type
	 * void fn(void)
	 */
	void (*fn)(void) = (void(*))buf;
	if (count) {
		printk(KERN_INFO "Will attempt exec %d bytes of ring3 code @ 0x%p\n",
			count, buf);
		fn();
		printk(KERN_INFO "Exec of data @ 0x%p complete\n", buf);
	}
	return count;
}

static int smep_test_open(struct inode *inode, struct file *file)
{
	mutex_lock(&smep_test_mutex);
	nonseekable_open(inode, file);

	if (mutex_lock_interruptible(&smep_test_dev.open_lock)) {
		mutex_unlock(&smep_test_mutex);
		return -ERESTARTSYS;
	}

	if (smep_test_dev.opened) {
		mutex_unlock(&smep_test_dev.open_lock);
		mutex_unlock(&smep_test_mutex);
		return -EINVAL;
	}

	smep_test_dev.opened++;
	mutex_unlock(&smep_test_dev.open_lock);
	mutex_unlock(&smep_test_mutex);
	return 0;
}

static int smep_test_release(struct inode *inode, struct file *file)
{
	mutex_lock(&smep_test_dev.open_lock);
	smep_test_dev.opened = 0;
	mutex_unlock(&smep_test_dev.open_lock);

	return 0;
}



static const struct file_operations smep_test_file_ops = {
	.open = smep_test_open,
	.release = smep_test_release,
	.unlocked_ioctl = smep_test_ioctl,
	.write		= smep_test_write,
	.llseek = no_llseek,
};


/**
 * intel_cln_smep_test_probe
 *
 * @param pdev: Platform device
 * @return 0 success < 0 failure
 *
 * Callback from platform sub-system to probe
 *
 * This driver manages eSRAM on a per-page basis. Therefore if we find block
 * mode is enabled, or any global, block-level or page-level locks are in place
 * at module initialisation time - we bail out.
 */
static int intel_cln_smep_test_probe(struct platform_device * pdev)
{
	int retval = 0;
	unsigned int minor = 0;

	mutex_init(&smep_test_dev.open_lock);
	cdev_init(&smep_test_dev.cdev, &smep_test_file_ops);
	smep_test_dev.cdev.owner = THIS_MODULE;

	retval = cdev_add(&smep_test_dev.cdev, MKDEV(smep_test_major, minor), 1);
	if (retval) {
		printk(KERN_ERR "chardev registration failed\n");
		return -EINVAL;
	}
	if (IS_ERR(device_create(smep_test_class, NULL,
				 MKDEV(smep_test_major, minor), NULL,
				 "smeptest%u", minor))){
		dev_err(&pdev->dev, "can't create device\n");
		return -EINVAL;
	}
	printk(KERN_INFO "%s complete OK - device /dev/smeptest%u\n", __FUNCTION__, minor);
	return 0;

}

/**
 * intel_cln_smep_remove
 *
 * @return 0 success < 0 failure
 *
 * Removes a platform device
 */
static int intel_cln_smep_test_remove(struct platform_device * pdev)
{
	unsigned int minor = MINOR(smep_test_dev.cdev.dev);

	device_destroy(smep_test_class, MKDEV(smep_test_major, minor));
	cdev_del(&smep_test_dev.cdev);

	return 0;
}

/*
 * Platform structures useful for interface to PM subsystem
 */
static struct platform_driver intel_cln_smep_test_driver = {
	.driver = {
		.name = DRIVER_NAME,
		.owner = THIS_MODULE,
	},
	.remove = intel_cln_smep_test_remove,
};

/**
 * intel_cln_smep_init
 *
 * @return 0 success < 0 failure
 *
 * Module entry point
 */
static int __init intel_cln_smep_test_init(void)
{
	int retval = 0;
	dev_t dev;

	smep_test_class = class_create(THIS_MODULE,"cln_smep_test");
	if (IS_ERR(smep_test_class)) {
		retval = PTR_ERR(smep_test_class);
		printk(KERN_ERR "smep_test: can't register earam_test class\n");
		goto err;
	}

	retval = alloc_chrdev_region(&dev, 0, 1, "smep_test");
	if (retval) {
		printk(KERN_ERR "smep_test: can't register character device\n");
		goto err_class;
	}
	smep_test_major = MAJOR(dev);

	memset(&smep_test_dev, 0x00, sizeof(smep_test_dev));
	smep_test_dev.pldev = platform_create_bundle(
		&intel_cln_smep_test_driver, intel_cln_smep_test_probe, NULL, 0, NULL, 0);

	if(IS_ERR(smep_test_dev.pldev)){
		printk(KERN_ERR "smep_test platform_create_bundle fail!\n"); 
		retval = PTR_ERR(smep_test_dev.pldev);
		goto err_class;
	}

	return 0;

err_class:
	class_destroy(smep_test_class);
err:
	return retval;
}

/**
 * intel_cln_smep_exit
 *
 * Module exit
 */
static void __exit intel_cln_smep_test_exit(void)
{
	platform_device_unregister(smep_test_dev.pldev);
	platform_driver_unregister(&intel_cln_smep_test_driver);
}

MODULE_AUTHOR("Bryan O'Donoghue <bryan.odonoghue@linux.intel.com>");
MODULE_DESCRIPTION("Intel Clanton SMEP test");
MODULE_LICENSE("Dual BSD/GPL");

module_init(intel_cln_smep_test_init);
module_exit(intel_cln_smep_test_exit);
