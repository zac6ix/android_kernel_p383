/* drivers/input/touchscreen/gt1x_tpd.c
 *
 * 2010 - 2014 Goodix Technology.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be a reference
 * to you, when you are integrating the GOODiX's CTP IC into your system,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 * 
 * Version: 1.4   
 * Release Date:  2015/07/10
 */

#include "include/gt1x_tpd_custom.h"
#include "include/gt1x_generic.h"
#ifdef GTP_CONFIG_OF
#include <linux/of.h>
#include <linux/of_irq.h>
#endif
#if TPD_SUPPORT_I2C_DMA
#include <linux/dma-mapping.h>
#endif
#if GTP_ICS_SLOT_REPORT
#include <linux/input/mt.h>
#endif
#include <linux/wakelock.h>

#if (GTP_HAVE_TOUCH_KEY && TPD_HAVE_BUTTON)
#error GTP_HAVE_TOUCH_KEY and TPD_HAVE_BUTTON are mutually exclusive.
#endif

extern struct tpd_device *tpd;
static spinlock_t irq_lock; 
static int tpd_flag = 0;
static int tpd_irq_flag;
static int tpd_eint_mode = 1;
static int tpd_polling_time = 50;
static struct task_struct *thread = NULL;

static DECLARE_WAIT_QUEUE_HEAD(waiter);
static DEFINE_MUTEX(i2c_access);


#if (defined(TPD_WARP_START) && defined(TPD_WARP_END))
static int tpd_wb_start_local[TPD_WARP_CNT] = TPD_WARP_START;
static int tpd_wb_end_local[TPD_WARP_CNT] = TPD_WARP_END;
#endif

#if (defined(TPD_HAVE_CALIBRATION) && !defined(TPD_CUSTOM_CALIBRATION))
static int tpd_def_calmat_local[8] = TPD_CALIBRATION_MATRIX;
#endif
#if GTP_GESTURE_WAKEUP
static int tpd_gestrue_keys[] = {KEY_RIGHT,KEY_LEFT,KEY_UP,KEY_DOWN,KEY_U,KEY_O,KEY_W,KEY_M,KEY_E,KEY_C,KEY_Z,KEY_S,KEY_V};
#define TPD_GESTRUE_KEY_CNT     (sizeof( tpd_gestrue_keys )/sizeof( tpd_gestrue_keys[0] ))
#endif

/* Vanzo:wangfei on: Sat, 23 Apr 2016 14:10:14 +0800
 * added by wf for wakeup hung
 */
struct wake_lock gesture_suspend_lock;
// End of Vanzo:wangfei

#ifdef GTP_CONFIG_OF
static unsigned int tpd_touch_irq;
static irqreturn_t tpd_eint_interrupt_handler(unsigned irq, struct irq_desc *desc);
#else
static void tpd_eint_interrupt_handler(void);
#endif
static int tpd_event_handler(void *unused);
static int tpd_i2c_probe(struct i2c_client *client, const struct i2c_device_id *id);
static int tpd_i2c_detect(struct i2c_client *client, struct i2c_board_info *info);
static int tpd_i2c_remove(struct i2c_client *client);


#define GTP_DRIVER_NAME  "gt1x"
static const struct i2c_device_id tpd_i2c_id[] = { {GTP_DRIVER_NAME, 0}, {} };
static unsigned short force[] = { 0, GTP_I2C_ADDRESS, I2C_CLIENT_END, I2C_CLIENT_END };
static const unsigned short *const forces[] = { force, NULL };

//static struct i2c_client_address_data addr_data = { .forces = forces,};
static const struct of_device_id tpd_of_match[] = {
            {.compatible = "mediatek,cap_touch"},
                    {},  
};

static struct i2c_driver tpd_i2c_driver = {
	.probe = tpd_i2c_probe,
	.remove = tpd_i2c_remove,
	.detect = tpd_i2c_detect,
	.driver.name = "gt1x",
        .driver = {
            .name = "gt1x",
            .of_match_table = tpd_of_match,
        },
	.id_table = tpd_i2c_id,
	.address_list = (const unsigned short *)forces,
};

#if TPD_SUPPORT_I2C_DMA
static u8 *gpDMABuf_va = NULL;
static dma_addr_t gpDMABuf_pa = 0;
struct mutex dma_mutex;

static s32 i2c_dma_write_mtk(u16 addr, u8 * buffer, s32 len)
{
	s32 ret = 0;
	s32 pos = 0;
	s32 transfer_length;
	u16 address = addr;
	struct i2c_msg msg = {
		.flags = !I2C_M_RD,
		.ext_flag = (gt1x_i2c_client->ext_flag | I2C_ENEXT_FLAG | I2C_DMA_FLAG),
		.addr = (gt1x_i2c_client->addr & I2C_MASK_FLAG),
		.timing = I2C_MASTER_CLOCK,
		.buf = (u8 *) gpDMABuf_pa,
	};

	mutex_lock(&dma_mutex);
	while (pos != len) {
		if (len - pos > (IIC_DMA_MAX_TRANSFER_SIZE - GTP_ADDR_LENGTH)) {
			transfer_length = IIC_DMA_MAX_TRANSFER_SIZE - GTP_ADDR_LENGTH;
		} else {
			transfer_length = len - pos;
		}

		gpDMABuf_va[0] = (address >> 8) & 0xFF;
		gpDMABuf_va[1] = address & 0xFF;
		memcpy(&gpDMABuf_va[GTP_ADDR_LENGTH], &buffer[pos], transfer_length);

		msg.len = transfer_length + GTP_ADDR_LENGTH;

		ret = i2c_transfer(gt1x_i2c_client->adapter, &msg, 1);
		if (ret != 1) {
			GTP_ERROR("I2c Transfer error! (%d)", ret);
			ret = ERROR_IIC;
			break;
		}
		ret = 0;
		pos += transfer_length;
		address += transfer_length;
	}
	mutex_unlock(&dma_mutex);
	return ret;
}

static s32 i2c_dma_read_mtk(u16 addr, u8 * buffer, s32 len)
{
	s32 ret = ERROR;
	s32 pos = 0;
	s32 transfer_length;
	u16 address = addr;
	u8 addr_buf[GTP_ADDR_LENGTH] = { 0 };
	struct i2c_msg msgs[2] = {
		{
		 .flags = 0,	//!I2C_M_RD,
		 .addr = (gt1x_i2c_client->addr & I2C_MASK_FLAG),
		 .timing = I2C_MASTER_CLOCK,
		 .len = GTP_ADDR_LENGTH,
		 .buf = addr_buf,
		 },
		{
		 .flags = I2C_M_RD,
		 .ext_flag = (gt1x_i2c_client->ext_flag | I2C_ENEXT_FLAG | I2C_DMA_FLAG),
		 .addr = (gt1x_i2c_client->addr & I2C_MASK_FLAG),
		 .timing = I2C_MASTER_CLOCK,
		 .buf = (u8 *) gpDMABuf_pa,
		 },
	};
	mutex_lock(&dma_mutex);
	while (pos != len) {
		if (len - pos > IIC_DMA_MAX_TRANSFER_SIZE) {
			transfer_length = IIC_DMA_MAX_TRANSFER_SIZE;
		} else {
			transfer_length = len - pos;
		}

		msgs[0].buf[0] = (address >> 8) & 0xFF;
		msgs[0].buf[1] = address & 0xFF;
		msgs[1].len = transfer_length;

		ret = i2c_transfer(gt1x_i2c_client->adapter, msgs, 2);
		if (ret != 2) {
			GTP_ERROR("I2C Transfer error! (%d)", ret);
			ret = ERROR_IIC;
			break;
		}
		ret = 0;
		memcpy(&buffer[pos], gpDMABuf_va, transfer_length);
		pos += transfer_length;
		address += transfer_length;
	};
	mutex_unlock(&dma_mutex);
	return ret;
}

#else

static s32 i2c_write_mtk(u16 addr, u8 * buffer, s32 len)
{
	s32 ret;

	struct i2c_msg msg = {
		.flags = 0,
		.addr = (gt1x_i2c_client->addr & I2C_MASK_FLAG) | (I2C_ENEXT_FLAG),	//remain
		.timing = I2C_MASTER_CLOCK,
	};

	ret = _do_i2c_write(&msg, addr, buffer, len);
	return ret;
}

static s32 i2c_read_mtk(u16 addr, u8 * buffer, s32 len)
{
	int ret;
	u8 addr_buf[GTP_ADDR_LENGTH] = { (addr >> 8) & 0xFF, addr & 0xFF };

	struct i2c_msg msgs[2] = {
		{
		 .addr = ((gt1x_i2c_client->addr & I2C_MASK_FLAG) | (I2C_ENEXT_FLAG)),
		 .flags = 0,
		 .buf = addr_buf,
		 .len = GTP_ADDR_LENGTH,
		 .timing = I2C_MASTER_CLOCK},
		{
		 .addr = ((gt1x_i2c_client->addr & I2C_MASK_FLAG) | (I2C_ENEXT_FLAG)),
		 .flags = I2C_M_RD,
		 .timing = I2C_MASTER_CLOCK},
	};

	ret = _do_i2c_read(msgs, addr, buffer, len);
	return ret;
}
#endif /* TPD_SUPPORT_I2C_DMA */

/**
 * @return: return 0 if success, otherwise return a negative number
 *          which contains the error code.
 */
s32 gt1x_i2c_read(u16 addr, u8 * buffer, s32 len)
{
#if TPD_SUPPORT_I2C_DMA
	return i2c_dma_read_mtk(addr, buffer, len);
#else
	return i2c_read_mtk(addr, buffer, len);
#endif
}

/**
 * @return: return 0 if success, otherwise return a negative number
 *          which contains the error code.
 */
s32 gt1x_i2c_write(u16 addr, u8 * buffer, s32 len)
{
#if TPD_SUPPORT_I2C_DMA
	return i2c_dma_write_mtk(addr, buffer, len);
#else
	return i2c_write_mtk(addr, buffer, len);
#endif
}

#ifdef TPD_REFRESH_RATE
/**
 * gt1x_set_refresh_rate - Write refresh rate
 * @rate: refresh rate N (Duration=5+N ms, N=0~15)
 * Return: 0---succeed.
 */
static u8 gt1x_set_refresh_rate(u8 rate)
{
	u8 buf[1] = { rate };

	if (rate > 0xf) {
		GTP_ERROR("Refresh rate is over range (%d)", rate);
		return ERROR_VALUE;
	}

	GTP_INFO("Refresh rate change to %d", rate);
	return gt1x_i2c_write(GTP_REG_REFRESH_RATE, buf, sizeof(buf));
}

/**
  *gt1x_get_refresh_rate - get refresh rate
  *Return: Refresh rate or error code
 */
static u8 gt1x_get_refresh_rate(void)
{
	int ret;
	u8 buf[1] = { 0x00 };
	ret = gt1x_i2c_read(GTP_REG_REFRESH_RATE, buf, sizeof(buf));
	if (ret < 0)
		return ret;

	GTP_INFO("Refresh rate is %d", buf[0]);
	return buf[0];
}

static ssize_t show_refresh_rate(struct device *dev, struct device_attribute *attr, char *buf)
{
	int ret = gt1x_get_refresh_rate();
	if (ret < 0)
		return 0;
	else
		return sprintf(buf, "%d\n", ret);
}

static ssize_t store_refresh_rate(struct device *dev, struct device_attribute *attr, const char *buf, size_t size)
{
	gt1x_set_refresh_rate(simple_strtoul(buf, NULL, 16));
	return size;
}

static DEVICE_ATTR(tpd_refresh_rate, 0664, show_refresh_rate, store_refresh_rate);

static struct device_attribute *gt1x_attrs[] = {
	&dev_attr_tpd_refresh_rate,
};
#endif

static int tpd_i2c_detect(struct i2c_client *client, struct i2c_board_info *info)
{
	strcpy(info->type, "mtk-tpd");
	return 0;
}

static int tpd_power_on(void)
{
	gt1x_power_switch(SWITCH_ON);

    gt1x_reset_guitar();

	if (gt1x_get_chip_type() != 0) {
		return -1;
	}

	if (gt1x_reset_guitar() != 0) {
		return -1;
	}
	return 0;
}

void gt1x_irq_enable(void)
{
    unsigned long flag;
    
    spin_lock_irqsave(&irq_lock, flag);
    if (!tpd_irq_flag) { // 0-disabled
        tpd_irq_flag = 1;  // 1-enabled
#ifdef GTP_CONFIG_OF
        enable_irq(tpd_touch_irq);
#else
	    mt_eint_unmask(CUST_EINT_TOUCH_PANEL_NUM);
#endif
    }
    spin_unlock_irqrestore(&irq_lock, flag);
}

void gt1x_irq_disable(void)
{
    unsigned long flag;

    spin_lock_irqsave(&irq_lock, flag);
    if (tpd_irq_flag) {
        tpd_irq_flag = 0;
#ifdef GTP_CONFIG_OF
        disable_irq(tpd_touch_irq);
#else
        mt_eint_mask(CUST_EINT_TOUCH_PANEL_NUM);
#endif
    }
    spin_unlock_irqrestore(&irq_lock, flag);
}

int gt1x_power_switch(s32 state)
{
    static int power_state = 0;
    
	GTP_GPIO_OUTPUT(GTP_RST_PORT, 0);
	GTP_GPIO_OUTPUT(GTP_INT_PORT, 0);
	msleep(10);

	switch (state) {
	case SWITCH_ON:
        if (power_state == 0) {    
    		GTP_DEBUG("Power switch on!");
#ifdef VANZO_TPD_POWER_SOURCE_VIA_EXT_LDO
                //GTP_LDOEN_GPIO_OUTPUT(1);
            tpd_ldo_power_enable(1);
#else // ( defined(MT6575) || defined(MT6577) || defined(MT6589) )
                if(regulator_enable(tpd->reg))      /*enable regulator*/
                    GTP_ERROR("regulator_enable() failed!\n");
#endif
            power_state = 1;
        }
    		break;
	case SWITCH_OFF:
        if (power_state == 1) {
		    GTP_DEBUG("Power switch off!");
#ifdef VANZO_TPD_POWER_SOURCE_VIA_EXT_LDO
                    //GTP_LDOEN_GPIO_OUTPUT(0);
            tpd_ldo_power_enable(0);
#else
            //disable regulator
            if (regulator_disable(tpd->reg))
                GTP_ERROR("regulator_disable() failed!");
#endif
            power_state = 0;
        }
		break;
	default:
		GTP_ERROR("Invalid power switch command!");
		break;
	}
	return 0;
}

static int tpd_irq_registration(void)
{
	struct device_node *node = NULL;
	int ret = 0;
	u32 ints[2] = {0,0};
	GTP_INFO("Device Tree Tpd_irq_registration!");
	
        node = of_find_matching_node(node, touch_of_match);
	if(node){
		of_property_read_u32_array(node , "debounce", ints, ARRAY_SIZE(ints));
		gpio_set_debounce(ints[0], ints[1]);

		tpd_touch_irq = irq_of_parse_and_map(node, 0);
		GTP_INFO("Device gt1x_int_type = %d!", gt1x_int_type);
		if (!gt1x_int_type)	//EINTF_TRIGGER
		{
			ret = request_irq(tpd_touch_irq, (irq_handler_t)tpd_eint_interrupt_handler, IRQF_TRIGGER_RISING, "touch-eint", NULL);
			if(ret > 0){
			    ret = -1;
			    GTP_ERROR("tpd request_irq IRQ LINE NOT AVAILABLE!.");
			}
		} else {
			ret = request_irq(tpd_touch_irq, (irq_handler_t)tpd_eint_interrupt_handler, IRQF_TRIGGER_FALLING, "touch-eint", NULL);
			if(ret > 0){
			    ret = -1;
			    GTP_ERROR("tpd request_irq IRQ LINE NOT AVAILABLE!.");
			}
		}
	}else{
		GTP_ERROR("tpd request_irq can not find touch eint device node!.");
		ret = -1;
	}
	GTP_INFO("irq:%d, debounce:%d-%d:", tpd_touch_irq, ints[0], ints[1]);
	return ret;
    
}
#if GTP_GESTURE_WAKEUP
static int _is_open_gesture_mode = 0;
extern int gesture_enabled;
static ssize_t gt9xx_gesture_show(struct device *dev,struct device_attribute *attr,char *buf)
{
    ssize_t num_read_chars = 0;

    num_read_chars = snprintf(buf, PAGE_SIZE, "%d\n", _is_open_gesture_mode);

    return num_read_chars;
}

static ssize_t gt9xx_gesture_store(struct device *dev,struct device_attribute *attr,const char *buf, size_t count)
{
    if(count == 0)
        return 0;
#ifdef GTP_CONFIG_OF
    disable_irq(tpd_touch_irq);
#else
    mt_eint_mask(CUST_EINT_TOUCH_PANEL_NUM);
#endif
    if(buf[0] == '1' && _is_open_gesture_mode == 0){
        _is_open_gesture_mode = 1;
    }
    if(buf[0] == '0' && _is_open_gesture_mode == 1){
        _is_open_gesture_mode = 0;
    }

    if (_is_open_gesture_mode==1)
    {
        gesture_enabled=1;
    }
    if (_is_open_gesture_mode==0)
    {
        gesture_enabled=0;
    }

#ifdef GTP_CONFIG_OF
    enable_irq(tpd_touch_irq);
#else
    mt_eint_unmask(CUST_EINT_TOUCH_PANEL_NUM);
#endif
    return count;
}

static DEVICE_ATTR(gesture, 0664, gt9xx_gesture_show, gt9xx_gesture_store);

static struct attribute *gt9xx_attributes[] = {

    &dev_attr_gesture.attr,


    NULL
};
static struct attribute_group gt9xx_attribute_group = {
    .attrs = gt9xx_attributes
};
#endif

static s32 tpd_i2c_probe(struct i2c_client *client, const struct i2c_device_id *id)
{
	s32 err = 0;
	s32 idx = 0;
#if GTP_GESTURE_WAKEUP
    int ret;
    int i;
#endif

    printk("kls probe\n");
    if (RECOVERY_BOOT == get_boot_mode())   // important
        return -1;

	gt1x_i2c_client = client;
    spin_lock_init(&irq_lock);

	if (gt1x_init()) {
		/* TP resolution == LCD resolution, no need to match resolution when initialized fail */
		gt1x_abs_x_max = 0;
		gt1x_abs_y_max = 0;
	}

	thread = kthread_run(tpd_event_handler, 0, TPD_DEVICE);
	if (IS_ERR(thread)) {
		err = PTR_ERR(thread);
		GTP_ERROR(TPD_DEVICE " failed to create kernel thread: %d\n", err);
	}
	for (idx = 0; idx < tpd_dts_data.tpd_key_num; idx++) {
		input_set_capability(tpd->dev, EV_KEY, tpd_dts_data.tpd_key_local[idx]);
	}

#if GTP_GESTURE_WAKEUP
	input_set_capability(tpd->dev, EV_KEY, KEY_GES_CUSTOM);
    input_set_capability(tpd->dev, EV_KEY, KEY_GES_REGULAR);
#endif

	GTP_GPIO_AS_INT(GTP_INT_PORT);
	msleep(50);

    /* interrupt registration */
    tpd_irq_registration();
	gt1x_irq_enable();

#if GTP_ESD_PROTECT
	/*  must before auto update */
	gt1x_init_esd_protect();
	gt1x_esd_switch(SWITCH_ON);
#endif

#if GTP_AUTO_UPDATE
        thread = kthread_run(gt1x_auto_update_proc, (void *)NULL, "gt1x auto update");
        if (IS_ERR(thread)) {
            err = PTR_ERR(thread);
            GTP_ERROR(TPD_DEVICE "failed to create auto-update thread: %d\n", err);
        }
#endif
#if GTP_GESTURE_WAKEUP
        if ((ret = sysfs_create_group(&client->dev.kobj, &gt9xx_attribute_group))) {
            GTP_ERROR("create attr fail\n");
            return -1;
        }
#endif
#if GTP_GESTURE_WAKEUP
        input_set_capability(tpd->dev, EV_KEY, KEY_POWER);
        for(i=0; i<TPD_GESTRUE_KEY_CNT; i++)
        {
            input_set_capability(tpd->dev, EV_KEY, tpd_gestrue_keys[i]);
        }
#endif

        tpd_load_status = 1;

/* Vanzo:wangfei on: Sat, 23 Apr 2016 14:10:29 +0800
 *added by wf for wakeup hung
 */
        wake_lock_init(&gesture_suspend_lock,WAKE_LOCK_SUSPEND,"gt1x_wakelock");
// End of Vanzo:wangfei
/* Vanzo:zhangqingzhan on: Tue, 01 Nov 2016 18:38:34 +0800
 *for tpd info
 */
#ifdef VANZO_DEVICE_NAME_SUPPORT
{
    extern void v_set_dev_name(int id, char *name);
    v_set_dev_name(2, "GT1XX");
}
#endif
// End of Vanzo: zhangqingzhan
	return 0;
}

#ifdef GTP_CONFIG_OF
static irqreturn_t tpd_eint_interrupt_handler(unsigned irq, struct irq_desc *desc)
{
    TPD_DEBUG_PRINT_INT;
	
	tpd_flag = 1;

	/* enter EINT handler disable INT, make sure INT is disable when handle touch event including top/bottom half */
	/* use _nosync to avoid deadlock */
    spin_lock(&irq_lock);
    tpd_irq_flag = 0;
	disable_irq_nosync(tpd_touch_irq);
    spin_unlock(&irq_lock);

 	wake_up_interruptible(&waiter);
    return IRQ_HANDLED;
}
#else
static void tpd_eint_interrupt_handler(void)
{
	TPD_DEBUG_PRINT_INT;
	tpd_flag = 1;
    gt1x_irq_disable();
	wake_up_interruptible(&waiter);
}
#endif

void gt1x_touch_down(s32 x, s32 y, s32 size, s32 id)
{
#if GTP_CHANGE_X2Y
	GTP_SWAP(x, y);
#endif

#if GTP_ICS_SLOT_REPORT
	input_mt_slot(tpd->dev, id);
	input_report_abs(tpd->dev, ABS_MT_PRESSURE, size);
	input_report_abs(tpd->dev, ABS_MT_TOUCH_MAJOR, size);
	input_report_abs(tpd->dev, ABS_MT_TRACKING_ID, id);
	input_report_abs(tpd->dev, ABS_MT_POSITION_X, x);
	input_report_abs(tpd->dev, ABS_MT_POSITION_Y, y);
#else
	input_report_key(tpd->dev, BTN_TOUCH, 1);
	if ((!size) && (!id)) {
		/* for virtual button */
		input_report_abs(tpd->dev, ABS_MT_PRESSURE, 100);
		input_report_abs(tpd->dev, ABS_MT_TOUCH_MAJOR, 100);
	} else {
		input_report_abs(tpd->dev, ABS_MT_PRESSURE, size);
		input_report_abs(tpd->dev, ABS_MT_TOUCH_MAJOR, size);
		input_report_abs(tpd->dev, ABS_MT_TRACKING_ID, id);
	}
	input_report_abs(tpd->dev, ABS_MT_POSITION_X, x);
	input_report_abs(tpd->dev, ABS_MT_POSITION_Y, y);
	input_mt_sync(tpd->dev);
#endif
    if(tpd_dts_data.use_tpd_button){
	if (FACTORY_BOOT == get_boot_mode() || RECOVERY_BOOT == get_boot_mode()) {
		tpd_button(x, y, 1);
	}
    }
}

void gt1x_touch_up(s32 id)
{
#if GTP_ICS_SLOT_REPORT
	input_mt_slot(tpd->dev, id);
	input_report_abs(tpd->dev, ABS_MT_TRACKING_ID, -1);
#else
	input_report_key(tpd->dev, BTN_TOUCH, 0);
	input_mt_sync(tpd->dev);
#endif
    if(tpd_dts_data.use_tpd_button){
	if (FACTORY_BOOT == get_boot_mode() || RECOVERY_BOOT == get_boot_mode()) {
		tpd_button(0, 0, 0);
	}
    }
}

#if GTP_CHARGER_SWITCH
#ifdef MT6573
#define CHR_CON0      (0xF7000000+0x2FA00)
#else
extern kal_bool upmu_is_chr_det(void);
#endif

u32 gt1x_get_charger_status(void)
{
	u32 chr_status = 0;
#ifdef MT6573
	chr_status = *(volatile u32 *)CHR_CON0;
	chr_status &= (1 << 13);
#else /* ( defined(MT6575) || defined(MT6577) || defined(MT6589) ) */
	chr_status = upmu_is_chr_det();
#endif
	return chr_status;
}
#endif

static int tpd_event_handler(void *unused)
{
	u8 finger = 0;
	u8 end_cmd = 0;
	s32 ret = 0;
	u8 point_data[11] = { 0 };
	struct sched_param param = {.sched_priority = 4};

	sched_setscheduler(current, SCHED_RR, &param);
	do {
		set_current_state(TASK_INTERRUPTIBLE);

		if (tpd_eint_mode) {
			wait_event_interruptible(waiter, tpd_flag != 0);
			tpd_flag = 0;
		} else {
			GTP_DEBUG("Polling coordinate mode!");
			msleep(tpd_polling_time);
		}

		set_current_state(TASK_RUNNING);

        if (update_info.status) {
            GTP_DEBUG("Ignore interrupts during fw updating.");
            continue;
        }
        
		mutex_lock(&i2c_access);
		/* don't reset before "if (gt1x_halt..."  */

#if GTP_GESTURE_WAKEUP
		ret = gesture_event_handler(tpd->dev);
		if (ret >= 0) {
			gt1x_irq_enable();
			mutex_unlock(&i2c_access);
			continue;
		}
#endif
		if (gt1x_halt) {
			mutex_unlock(&i2c_access);
			GTP_DEBUG("Ignore interrupts after suspend.");
			continue;
		}

		/* read coordinates */
		ret = gt1x_i2c_read(GTP_READ_COOR_ADDR, point_data, sizeof(point_data));
		if (ret < 0) {
			GTP_ERROR("I2C transfer error!");
#if !GTP_ESD_PROTECT
			gt1x_power_reset();
#endif
			gt1x_irq_enable();
			mutex_unlock(&i2c_access);
            continue;
		}
		finger = point_data[0];

		/* response to a ic request */
		if (finger == 0x00) {
			gt1x_request_event_handler();
		}

		if ((finger & 0x80) == 0) {
#if HOTKNOT_BLOCK_RW
			if (!hotknot_paired_flag)
#endif
			{
				gt1x_irq_enable();
				mutex_unlock(&i2c_access);
				//GTP_ERROR("buffer not ready:0x%02x", finger);
				continue;
			}
		}
#if HOTKNOT_BLOCK_RW
		ret = hotknot_event_handler(point_data);
		if (!ret) {
			goto exit_work_func;
		}
#endif

#if GTP_PROXIMITY
		ret = gt1x_prox_event_handler(point_data);
		if (ret > 0) {
			goto exit_work_func;
		}
#endif

#if GTP_WITH_STYLUS
		ret = gt1x_touch_event_handler(point_data, tpd->dev, pen_dev);
#else
		ret = gt1x_touch_event_handler(point_data, tpd->dev, NULL);
#endif

#if (GTP_PROXIMITY || GTP_WITH_STYLUS)
exit_work_func:
#endif
		if (!gt1x_rawdiff_mode && (ret >= 0 || ret == ERROR_VALUE)) {
			ret = gt1x_i2c_write(GTP_READ_COOR_ADDR, &end_cmd, 1);
			if (ret < 0) {
				GTP_INFO("I2C write end_cmd  error!");
			}
		}
		gt1x_irq_enable();
		mutex_unlock(&i2c_access);

	} while (!kthread_should_stop());

	return 0;
}

int gt1x_debug_proc(u8 * buf, int count)
{
	char mode_str[50] = { 0 };
	int mode;

	sscanf(buf, "%s %d", (char *)&mode_str, &mode);

	/***********POLLING/EINT MODE switch****************/
	if (strcmp(mode_str, "polling") == 0) {
		if (mode >= 10 && mode <= 200) {
			GTP_INFO("Switch to polling mode, polling time is %d", mode);
			tpd_eint_mode = 0;
			tpd_polling_time = mode;
			tpd_flag = 1;
			wake_up_interruptible(&waiter);
		} else {
			GTP_INFO("Wrong polling time, please set between 10~200ms");
		}
		return count;
	}
	if (strcmp(mode_str, "eint") == 0) {
		GTP_INFO("Switch to eint mode");
		tpd_eint_mode = 1;
		return count;
	}
	/**********************************************/
	if (strcmp(mode_str, "switch") == 0) {
		if (mode == 0)	// turn off
			tpd_off();
		else if (mode == 1)	//turn on
			tpd_on();
		else
			GTP_ERROR("error mode :%d", mode);
		return count;
	}

	return -1;
}

static u16 convert_productname(u8 * name)
{
	int i;
	u16 product = 0;
	for (i = 0; i < 4; i++) {
		product <<= 4;
		if (name[i] < '0' || name[i] > '9') {
			product += '*';
		} else {
			product += name[i] - '0';
		}
	}
	return product;
}

static int tpd_i2c_remove(struct i2c_client *client)
{
	gt1x_deinit();
	return 0;
}

static int tpd_local_init(void)
{
    printk("kls init\n");
#ifdef TPD_POWER_SOURCE_CUSTOM
#ifdef GTP_CONFIG_OF
#ifdef CONFIG_ARCH_MT6580
    tpd->reg = regulator_get(tpd->tpd_dev,TPD_POWER_SOURCE_CUSTOM); // get pointer to regulator structure
    if (IS_ERR(tpd->reg)) {
        GTP_ERROR("regulator_get() failed.");
    }
#endif
#endif
#endif

#if TPD_SUPPORT_I2C_DMA
	mutex_init(&dma_mutex);
	tpd->dev->dev.coherent_dma_mask = DMA_BIT_MASK(32);
	gpDMABuf_va = (u8 *) dma_alloc_coherent(&tpd->dev->dev, IIC_DMA_MAX_TRANSFER_SIZE, &gpDMABuf_pa, GFP_KERNEL);
	if (!gpDMABuf_va) {
		GTP_ERROR("Allocate DMA I2C Buffer failed!");
		return -1;
	}
	memset(gpDMABuf_va, 0, IIC_DMA_MAX_TRANSFER_SIZE);
#endif
	if (i2c_add_driver(&tpd_i2c_driver) != 0) {
		GTP_ERROR("unable to add i2c driver.");
		return -1;
	}

	if (tpd_load_status == 0)	// disable auto load touch driver for linux3.0 porting
	{
		GTP_ERROR("add error touch panel driver.");
		i2c_del_driver(&tpd_i2c_driver);
		return -1;
	}
	input_set_abs_params(tpd->dev, ABS_MT_TRACKING_ID, 0, (GTP_MAX_TOUCH - 1), 0, 0);
        if (tpd_dts_data.use_tpd_button) {
	    tpd_button_setting(tpd_dts_data.tpd_key_num, tpd_dts_data.tpd_key_local, tpd_dts_data.tpd_key_dim_local);	// initialize tpd button data
        }
#if (defined(TPD_WARP_START) && defined(TPD_WARP_END))
	TPD_DO_WARP = 1;
	memcpy(tpd_wb_start, tpd_wb_start_local, TPD_WARP_CNT * 4);
	memcpy(tpd_wb_end, tpd_wb_start_local, TPD_WARP_CNT * 4);
#endif

#if (defined(TPD_HAVE_CALIBRATION) && !defined(TPD_CUSTOM_CALIBRATION))
	memcpy(tpd_calmat, tpd_def_calmat_local, 8 * 4);
	memcpy(tpd_def_calmat, tpd_def_calmat_local, 8 * 4);
#endif

	// set vendor string
	tpd->dev->id.vendor = 0x00;
	tpd->dev->id.product = convert_productname(gt1x_version.product_id);
	tpd->dev->id.version = (gt1x_version.patch_id >> 8);

	GTP_INFO("end %s, %d\n", __FUNCTION__, __LINE__);
	tpd_type_cap = 1;

	return 0;
}

/* Function to manage low power suspend */
static void tpd_suspend(struct device *h)
{
	gt1x_suspend();
}

/* Function to manage power-on resume */
static void tpd_resume(struct device *h)
{
	gt1x_resume();
}

static struct tpd_driver_t tpd_device_driver = {
	.tpd_device_name = "gt1x",
	.tpd_local_init = tpd_local_init,
	.suspend = tpd_suspend,
	.resume = tpd_resume,
#ifdef TPD_HAVE_BUTTON
	.tpd_have_button = 1,
#else
	.tpd_have_button = 0,
#endif
};

void tpd_off(void)
{
	gt1x_power_switch(SWITCH_OFF);
	gt1x_halt = 1;
	gt1x_irq_disable();
}

void tpd_on(void)
{
	s32 ret = -1, retry = 0;

	while (retry++ < 5) {
		ret = tpd_power_on();
		if (ret < 0) {
			GTP_ERROR("I2C Power on ERROR!");
		}

		ret = gt1x_send_cfg(gt1x_config, gt1x_cfg_length);
		if (ret == 0) {
			GTP_DEBUG("Wakeup sleep send gt1x_config success.");
			break;
		}
	}
	if (ret < 0) {
		GTP_ERROR("GTP later resume failed.");
	}
	//gt1x_irq_enable();
	gt1x_halt = 0;
}

/* called when loaded into kernel */
static int __init tpd_driver_init(void)
{
	GTP_INFO("Goodix touch panel driver init.");

        tpd_get_dts_info();

	if (tpd_driver_add(&tpd_device_driver) < 0) {
		GTP_ERROR("add generic driver failed.");
	}

	return 0;
}

/* should never be called */
static void __exit tpd_driver_exit(void)
{
	GTP_INFO("MediaTek GT1x touch panel driver exit.");
	tpd_driver_remove(&tpd_device_driver);

/* Vanzo:wangfei on: Sat, 23 Apr 2016 14:10:43 +0800
 * added by wf for wakeup hung
 */
    wake_lock_destroy(&gesture_suspend_lock);
// End of Vanzo:wangfei
}

module_init(tpd_driver_init);
module_exit(tpd_driver_exit);
