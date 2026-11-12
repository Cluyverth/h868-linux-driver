// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * H868 USB ISDB-T receiver (07ca:0868), digital path.
 * EP0, power and TS register sequences were validated on hardware.
 * No analog/AV firmware path.
 */
#include <linux/completion.h>
#include <linux/delay.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/usb.h>
#include <linux/workqueue.h>
#include <media/dmxdev.h>
#include <media/dvb_demux.h>
#include <media/dvbdev.h>

#include "h868.h"

#define H868_NAME "H868 ISDB-T"
#define H868_TIMEOUT 1000
#define H868_TS_EP 0x81
#define H868_URBS 8
#define H868_TS_BUFSIZE 16384
#define H868_FILTERS 256
#define H868_I2C_MAX 80

DVB_DEFINE_MOD_OPT_ADAPTER_NR(adapter_nr);

struct h868_device;

struct h868_i2c {
	struct i2c_adapter adapter;
	struct h868_device *dev;
	u8 bus;
};

struct h868_device {
	struct usb_device *udev;
	struct usb_interface *control_intf;
	struct usb_interface *stream_intf;
	/* Lock order: demux/dmxdev mutex -> stream_mutex -> io_mutex.
	 * Completion only takes stream_lock and never sleeps or accesses EP0.
	 */
	struct mutex io_mutex;
	struct mutex stream_mutex;
	spinlock_t stream_lock;
	bool disconnected;
	bool ready;
	bool powered;
	bool running;
	bool transport_saved;
	unsigned int feeds;
	int stream_error;
	u32 gpio_direction;
	u32 gpio_value;
	u32 saved_mode;
	u32 saved_config;
	u32 saved_endpoints;
	/* DMA-safe, serialized EP0 staging area (never a stack buffer). */
	u8 control_buf[4];
	struct h868_i2c i2c[2];
	struct urb *urbs[H868_URBS];
	struct work_struct stream_error_work;
	struct work_struct release_work;
	struct completion frontend_released;
	void (*frontend_release)(struct dvb_frontend *fe);
	bool dmxdev_registered;
	bool frontend_registered;
	bool release_module_ref;
	struct dvb_adapter adapter;
	struct dvb_frontend *frontend;
	struct dvb_demux demux;
	struct dmxdev dmxdev;
	struct dmx_frontend hw_frontend;
	struct dmx_frontend mem_frontend;
};

static struct usb_driver h868_usb_driver;
/* Private queue: module exit drains callbacks even when module_put() in the
 * last callback makes unloading possible before that callback returns.
 */
static struct workqueue_struct *h868_release_wq;

/* All EP0 helpers require io_mutex. Return zero only for an exact transfer. */
static int h868_control(struct h868_device *dev, bool read, u8 request,
			u16 value, u16 index, void *data, unsigned int len)
{
	unsigned int pipe;
	int ret;

	lockdep_assert_held(&dev->io_mutex);
	if (dev->disconnected)
		return -ENODEV;
	if (!len || len > sizeof(dev->control_buf))
		return -EINVAL;

	if (!read)
		memcpy(dev->control_buf, data, len);
	pipe = read ? usb_rcvctrlpipe(dev->udev, 0) :
		      usb_sndctrlpipe(dev->udev, 0);
	ret = usb_control_msg(dev->udev, pipe, request,
			     USB_TYPE_VENDOR | USB_RECIP_DEVICE |
			     (read ? USB_DIR_IN : USB_DIR_OUT),
			     value, index, dev->control_buf, len, H868_TIMEOUT);
	if (ret < 0)
		return ret;
	if (ret != len)
		return -EREMOTEIO;
	if (read)
		memcpy(data, dev->control_buf, len);
	return 0;
}

static int h868_read_reg(struct h868_device *dev, u16 reg, u32 *value)
{
	__le32 data;
	int ret = h868_control(dev, true, 0x0d, 0x0f, reg, &data, 4);

	if (!ret)
		*value = le32_to_cpu(data);
	return ret;
}

static int h868_write_reg(struct h868_device *dev, u16 reg, u32 value)
{
	__le32 data = cpu_to_le32(value);

	return h868_control(dev, false, 0x0c, 0x0f, reg, &data, 4);
}

/* Modern bridge protocol: reserve continues the same I2C transaction across
 * EP0 chunks, nostop holds it open until the last chunk. Subaddress stays in
 * wIndex throughout, exactly as in the validated userspace implementation.
 */
static int h868_i2c_xfer(struct h868_device *dev, u8 bus, u16 addr,
			 bool read, u16 reg, u8 reg_len, u8 *data, u16 len)
{
	unsigned int offset = 0;
	int ret;

	if (!len || bus > 2 || addr > 0x7f || reg_len > 2)
		return -EINVAL;
	while (offset < len) {
		unsigned int chunk = min_t(unsigned int, len - offset, 4);
		u16 value = (addr << 9) | (3 << 4) | (reg_len << 2) | 1;

		if (offset)
			value |= BIT(6);
		if (offset + chunk < len)
			value |= BIT(1);
		ret = h868_control(dev, read, bus + (read ? 4 : 0),
				   value, reg, data + offset, chunk);
		if (ret)
			return ret;
		offset += chunk;
	}
	return 0;
}

static int h868_i2c_transfer(struct i2c_adapter *adapter,
			     struct i2c_msg *msgs, int num)
{
	struct h868_i2c *bus = i2c_get_adapdata(adapter);
	struct h868_device *dev = bus->dev;
	struct i2c_msg *msg = &msgs[0];
	u16 reg = 0;
	int ret, i;

	if (num < 1 || num > 2)
		return -EOPNOTSUPP;
	for (i = 0; i < num; i++) {
		if (msgs[i].flags & ~(I2C_M_RD | I2C_M_DMA_SAFE))
			return -EOPNOTSUPP;
		if (!msgs[i].len || msgs[i].len > H868_I2C_MAX ||
		    msgs[i].addr > 0x7f)
			return -EOPNOTSUPP;
	}
	if (num == 2 && ((msg->flags & I2C_M_RD) ||
			 !(msgs[1].flags & I2C_M_RD) ||
			 msg->addr != msgs[1].addr || msg->len > 2))
		return -EOPNOTSUPP;

	ret = mutex_lock_interruptible(&dev->io_mutex);
	if (ret)
		return ret;
	if (num == 2) {
		reg = msg->buf[0];
		if (msg->len == 2)
			reg = (reg << 8) | msg->buf[1];
		ret = h868_i2c_xfer(dev, bus->bus, msg->addr, true, reg,
				    msg->len, msgs[1].buf, msgs[1].len);
	} else if (!(msg->flags & I2C_M_RD) && msg->addr == 0x10) {
		/* H868 demod writes use the bridge subaddress route. */
		if (msg->len < 2 || msg->len > 5)
			ret = -EOPNOTSUPP;
		else
			ret = h868_i2c_xfer(dev, bus->bus, msg->addr, false,
					    msg->buf[0], 1, msg->buf + 1,
					    msg->len - 1);
	} else {
		/* TDA18271 register byte belongs to the raw write payload. */
		ret = h868_i2c_xfer(dev, bus->bus, msg->addr,
				    msg->flags & I2C_M_RD, 0, 0,
				    msg->buf, msg->len);
	}
	mutex_unlock(&dev->io_mutex);
	return ret ? ret : num;
}

static u32 h868_i2c_functionality(struct i2c_adapter *adapter)
{
	return I2C_FUNC_I2C;
}

static const struct i2c_algorithm h868_i2c_algorithm = {
	.master_xfer = h868_i2c_transfer,
	.functionality = h868_i2c_functionality,
};

static const struct i2c_adapter_quirks h868_i2c_quirks = {
	.flags = I2C_AQ_NO_ZERO_LEN,
	.max_num_msgs = 2,
	.max_write_len = H868_I2C_MAX,
	.max_read_len = H868_I2C_MAX,
};

static int h868_write_gpio(struct h868_device *dev)
{
	__le32 value = cpu_to_le32(dev->gpio_value);

	return h868_control(dev, false, 0x08, dev->gpio_direction >> 16,
			    dev->gpio_direction, &value, 4);
}

static int h868_set_gpio(struct h868_device *dev, u8 pin, bool high)
{
	int ret;

	if (!(dev->gpio_direction & BIT(pin))) {
		dev->gpio_direction |= BIT(pin);
		ret = h868_write_gpio(dev);
		if (ret)
			return ret;
	}
	if (high)
		dev->gpio_value |= BIT(pin);
	else
		dev->gpio_value &= ~BIT(pin);
	return h868_write_gpio(dev);
}

static int h868_power_off(struct h868_device *dev)
{
	int ret, power_ret;

	if (!dev->powered)
		return 0;
	dev->gpio_direction = 0;
	dev->gpio_value = 0;
	ret = h868_write_gpio(dev);
	power_ret = h868_write_reg(dev, 0x74, 0);
	dev->powered = false;
	return ret ? ret : power_ret;
}

static int h868_power_on(struct h868_device *dev)
{
	static const u16 afe_regs[] = { 0x23, 0x43, 0x63 };
	u32 power;
	u8 value;
	int ret, i;

	ret = h868_read_reg(dev, 0x74, &power);
	if (ret)
		return ret;
	if (power) {
		dev_err(&dev->udev->dev,
			"unexpected power state %#x. Reconnect the receiver before binding\n",
			power);
		return -EBUSY;
	}
	/* Cleanup must also cover a partially completed power sequence. */
	dev->powered = true;
	ret = h868_write_reg(dev, 0x74, 0x08);
	if (ret)
		return ret;
	msleep(5);
	ret = h868_write_reg(dev, 0x74, 0x48);
	if (ret)
		return ret;
	msleep(5);
	ret = h868_write_reg(dev, 0x74, 0x59);
	if (ret)
		return ret;
	ret = h868_set_gpio(dev, 29, true);
	if (ret)
		return ret;
	msleep(50);
	ret = h868_set_gpio(dev, 30, true);
	if (ret)
		return ret;
	msleep(50);
	ret = h868_read_reg(dev, 0x74, &power);
	if (ret)
		return ret;
	ret = h868_write_reg(dev, 0x74, power | 0x100);
	if (ret)
		return ret;
	msleep(50);
	ret = h868_set_gpio(dev, 31, true);
	if (ret)
		return ret;
	ret = h868_set_gpio(dev, 20, true);
	if (ret)
		return ret;
	ret = h868_set_gpio(dev, 28, false);
	if (ret)
		return ret;
	msleep(5);
	value = 0x70;
	for (i = 0; i < ARRAY_SIZE(afe_regs); i++) {
		ret = h868_i2c_xfer(dev, 0, 0x30, false, afe_regs[i], 2,
				    &value, 1);
		if (ret)
			return ret;
	}
	ret = h868_i2c_xfer(dev, 0, 0x30, true, 0x08, 2, &value, 1);
	if (ret)
		return ret;
	value |= 7;
	return h868_i2c_xfer(dev, 0, 0x30, false, 0x08, 2, &value, 1);
}

/* stream_mutex serializes feed changes, error work and disconnect. */
static int h868_restore_transport(struct h868_device *dev)
{
	int ret, first = 0;

	lockdep_assert_held(&dev->stream_mutex);
	if (!dev->transport_saved)
		return 0;
	mutex_lock(&dev->io_mutex);
	ret = h868_write_reg(dev, 0x18, dev->saved_endpoints);
	if (ret)
		first = ret;
	ret = h868_write_reg(dev, 0x08, dev->saved_config);
	if (ret && !first)
		first = ret;
	ret = h868_write_reg(dev, 0x04, dev->saved_mode);
	if (ret && !first)
		first = ret;
	mutex_unlock(&dev->io_mutex);
	if (!first)
		dev->transport_saved = false;
	return first;
}

static int h868_stop_stream(struct h868_device *dev)
{
	unsigned long flags;
	int i;

	lockdep_assert_held(&dev->stream_mutex);
	spin_lock_irqsave(&dev->stream_lock, flags);
	dev->running = false;
	spin_unlock_irqrestore(&dev->stream_lock, flags);
	for (i = 0; i < H868_URBS; i++)
		usb_kill_urb(dev->urbs[i]);
	return h868_restore_transport(dev);
}

/* dmxdev_release waits for existing descriptors. Wake blocked read/poll users
 * first, and keep the driver, demux and I2C adapters alive until they close.
 */
static void h868_notify_error(struct h868_device *dev, int error, bool unplug)
{
	unsigned long flags;
	int i;

	mutex_lock(&dev->dmxdev.mutex);
	if (!unplug && READ_ONCE(dev->stream_error) != error)
		goto unlock;
	if (unplug)
		dev->dmxdev.exit = 1;
	spin_lock_irqsave(&dev->dmxdev.lock, flags);
	dev->dmxdev.dvr_buffer.error = error;
	wake_up_all(&dev->dmxdev.dvr_buffer.queue);
	for (i = 0; i < dev->dmxdev.filternum; i++) {
		struct dmxdev_filter *filter = &dev->dmxdev.filter[i];

		if (filter->state == DMXDEV_STATE_FREE)
			continue;
		filter->buffer.error = error;
		wake_up_all(&filter->buffer.queue);
	}
	spin_unlock_irqrestore(&dev->dmxdev.lock, flags);
unlock:
	mutex_unlock(&dev->dmxdev.mutex);
}

static void h868_stream_error_work(struct work_struct *work)
{
	struct h868_device *dev = container_of(work, struct h868_device,
					      stream_error_work);
	int error, ret;

	mutex_lock(&dev->stream_mutex);
	error = READ_ONCE(dev->stream_error);
	if (!error || READ_ONCE(dev->disconnected)) {
		mutex_unlock(&dev->stream_mutex);
		return;
	}
	ret = h868_stop_stream(dev);
	mutex_unlock(&dev->stream_mutex);
	dev_err(&dev->udev->dev, "TS USB stream stopped: %d (restore: %d)\n",
		error, ret);
	h868_notify_error(dev, error, false);
}

static void h868_ts_complete(struct urb *urb)
{
	struct h868_device *dev = urb->context;
	unsigned long flags;
	int ret;

	spin_lock_irqsave(&dev->stream_lock, flags);
	if (!dev->running)
		goto unlock;
	if (urb->status) {
		ret = urb->status;
		goto failed;
	}
	if (urb->actual_length)
		dvb_dmx_swfilter(&dev->demux, urb->transfer_buffer,
				 urb->actual_length);
	ret = usb_submit_urb(urb, GFP_ATOMIC);
	if (!ret)
		goto unlock;
failed:
	/* Stop all URBs in process context, not from their own callback. */
	WRITE_ONCE(dev->stream_error, ret);
	dev->running = false;
	schedule_work(&dev->stream_error_work);
unlock:
	spin_unlock_irqrestore(&dev->stream_lock, flags);
}

static int h868_start_stream(struct h868_device *dev)
{
	unsigned long flags;
	int ret, restore, i;

	lockdep_assert_held(&dev->stream_mutex);
	/* A failed rollback owns the original snapshot until every register has
	 * been restored. Never capture the partially configured transport.
	 */
	ret = h868_restore_transport(dev);
	if (ret)
		return ret;
	mutex_lock(&dev->io_mutex);
	ret = h868_read_reg(dev, 0x04, &dev->saved_mode);
	if (ret)
		goto unlock_io;
	ret = h868_read_reg(dev, 0x08, &dev->saved_config);
	if (ret)
		goto unlock_io;
	ret = h868_read_reg(dev, 0x18, &dev->saved_endpoints);
	if (ret)
		goto unlock_io;
	dev->transport_saved = true;
	ret = h868_write_reg(dev, 0x04, dev->saved_mode | 0x101);
	if (ret)
		goto unlock_io;
	ret = h868_write_reg(dev, 0x08, dev->saved_config | 0x408);
	if (ret)
		goto unlock_io;
	ret = h868_write_reg(dev, 0x18, dev->saved_endpoints | 1);
unlock_io:
	mutex_unlock(&dev->io_mutex);
	if (ret)
		goto stop;

	/* A failed previous stream may have left the endpoint halted. */
	ret = usb_clear_halt(dev->udev, usb_rcvbulkpipe(dev->udev, H868_TS_EP));
	if (ret)
		goto stop;
	dev->demux.tsbufp = 0;
	spin_lock_irqsave(&dev->stream_lock, flags);
	WRITE_ONCE(dev->stream_error, 0);
	dev->running = true;
	for (i = 0; i < H868_URBS; i++) {
		ret = usb_submit_urb(dev->urbs[i], GFP_ATOMIC);
		if (ret) {
			dev->running = false;
			break;
		}
	}
	spin_unlock_irqrestore(&dev->stream_lock, flags);
	if (!ret)
		return 0;
stop:
	restore = h868_stop_stream(dev);
	if (restore)
		dev_warn(&dev->udev->dev, "TS rollback failed: %d\n", restore);
	return ret;
}

static int h868_start_feed(struct dvb_demux_feed *feed)
{
	struct h868_device *dev = feed->demux->priv;
	int ret;

	if (!feed->demux->dmx.frontend)
		return -EINVAL;
	/* Feeds for the memory frontend do not consume USB bandwidth. */
	if (feed->demux->dmx.frontend->source == DMX_MEMORY_FE) {
		feed->priv = NULL;
		return 0;
	}
	mutex_lock(&dev->stream_mutex);
	if (!READ_ONCE(dev->ready) || READ_ONCE(dev->disconnected)) {
		ret = -ENODEV;
		goto unlock;
	}
	if (dev->feeds)
		ret = READ_ONCE(dev->stream_error);
	else
		ret = h868_start_stream(dev);
	if (ret)
		goto unlock;
	dev->feeds++;
	feed->priv = dev;
	ret = 0;
unlock:
	mutex_unlock(&dev->stream_mutex);
	return ret;
}

static int h868_stop_feed(struct dvb_demux_feed *feed)
{
	struct h868_device *dev = feed->demux->priv;
	int ret = 0;

	/* Source selection may have changed since start_feed. */
	if (!feed->priv)
		return 0;
	mutex_lock(&dev->stream_mutex);
	if (dev->feeds && !--dev->feeds) {
		ret = h868_stop_stream(dev);
		WRITE_ONCE(dev->stream_error, ret);
	}
	feed->priv = NULL;
	mutex_unlock(&dev->stream_mutex);
	return ret;
}

static void h868_free_urbs(struct h868_device *dev)
{
	int i;

	for (i = 0; i < H868_URBS; i++) {
		struct urb *urb = dev->urbs[i];

		if (!urb)
			continue;
		if (urb->transfer_buffer)
			usb_free_coherent(dev->udev, H868_TS_BUFSIZE,
					  urb->transfer_buffer, urb->transfer_dma);
		usb_free_urb(urb);
	}
}

static int h868_alloc_urbs(struct h868_device *dev)
{
	int i;

	for (i = 0; i < H868_URBS; i++) {
		struct urb *urb = usb_alloc_urb(0, GFP_KERNEL);
		void *buffer;

		if (!urb)
			return -ENOMEM;
		dev->urbs[i] = urb;
		buffer = usb_alloc_coherent(dev->udev, H868_TS_BUFSIZE,
					    GFP_KERNEL, &urb->transfer_dma);
		if (!buffer)
			return -ENOMEM;
		usb_fill_bulk_urb(urb, dev->udev,
				  usb_rcvbulkpipe(dev->udev, H868_TS_EP),
				  buffer, H868_TS_BUFSIZE, h868_ts_complete, dev);
		urb->transfer_flags |= URB_NO_TRANSFER_DMA_MAP;
	}
	return 0;
}

static void h868_frontend_released(struct dvb_frontend *fe)
{
	struct h868_device *dev = fe->dvb->priv;

	dev->frontend_release(fe);
	/* Frontend descriptors can outlive unregister/detach and still access
	 * fe->dvb on close. Keep the adapter and its backing device until here.
	 */
	complete(&dev->frontend_released);
}

static int h868_register_dvb(struct h868_device *dev)
{
	struct dmx_demux *dmx = &dev->demux.dmx;
	int ret;

	ret = dvb_register_adapter(&dev->adapter, H868_NAME, THIS_MODULE,
				   &dev->udev->dev, adapter_nr);
	if (ret < 0)
		return ret;
	dev->adapter.priv = dev;
	dev->frontend = h868_frontend_attach(&dev->i2c[0].adapter,
					    &dev->i2c[1].adapter);
	if (!dev->frontend) {
		ret = -ENODEV;
		goto unregister_adapter;
	}
	dev->frontend_release = dev->frontend->ops.release;
	dev->frontend->dvb = &dev->adapter;
	dev->frontend->ops.release = h868_frontend_released;
	dev->demux.priv = dev;
	dev->demux.filternum = H868_FILTERS;
	dev->demux.feednum = H868_FILTERS;
	dev->demux.start_feed = h868_start_feed;
	dev->demux.stop_feed = h868_stop_feed;
	dmx->capabilities = DMX_TS_FILTERING | DMX_SECTION_FILTERING |
			    DMX_MEMORY_BASED_FILTERING;
	ret = dvb_dmx_init(&dev->demux);
	if (ret)
		goto detach_frontend;
	dev->hw_frontend.source = DMX_FRONTEND_0;
	ret = dmx->add_frontend(dmx, &dev->hw_frontend);
	if (ret)
		goto release_demux;
	dev->mem_frontend.source = DMX_MEMORY_FE;
	ret = dmx->add_frontend(dmx, &dev->mem_frontend);
	if (ret)
		goto remove_hw;
	ret = dmx->connect_frontend(dmx, &dev->hw_frontend);
	if (ret)
		goto remove_mem;
	dev->dmxdev.filternum = H868_FILTERS;
	dev->dmxdev.demux = dmx;
	ret = dvb_dmxdev_init(&dev->dmxdev, &dev->adapter);
	if (ret)
		goto disconnect_demux;
	dev->dmxdev_registered = true;
	/* Publish frontend last: all capture resources are now ready. */
	ret = dvb_register_frontend(&dev->adapter, dev->frontend);
	if (!ret)
		dev->frontend_registered = true;
	/* Once dmxdev is published, even probe failure must drain open files
	 * asynchronously rather than freeing their demux/I2C backing storage.
	 */
	return ret;
disconnect_demux:
	dmx->disconnect_frontend(dmx);
remove_mem:
	dmx->remove_frontend(dmx, &dev->mem_frontend);
remove_hw:
	dmx->remove_frontend(dmx, &dev->hw_frontend);
release_demux:
	dvb_dmx_release(&dev->demux);
detach_frontend:
	/* The DVB core drops a module reference for ops.release when
	 * CONFIG_MEDIA_ATTACH is enabled, even for our in-module frontend.
	 */
	if (IS_ENABLED(CONFIG_MEDIA_ATTACH))
		__module_get(THIS_MODULE);
	dvb_frontend_detach(dev->frontend);
	dev->frontend = NULL;
unregister_adapter:
	dvb_unregister_adapter(&dev->adapter);
	return ret;
}

static void h868_unregister_dvb(struct h868_device *dev)
{
	struct dmx_demux *dmx = &dev->demux.dmx;

	if (dev->frontend_registered)
		dvb_unregister_frontend(dev->frontend);
	dvb_dmxdev_release(&dev->dmxdev);
	dmx->disconnect_frontend(dmx);
	dmx->remove_frontend(dmx, &dev->mem_frontend);
	dmx->remove_frontend(dmx, &dev->hw_frontend);
	dvb_dmx_release(&dev->demux);
	if (IS_ENABLED(CONFIG_MEDIA_ATTACH))
		__module_get(THIS_MODULE);
	dvb_frontend_detach(dev->frontend);
	wait_for_completion(&dev->frontend_released);
	dvb_unregister_adapter(&dev->adapter);
}

static void h868_release_work(struct work_struct *work)
{
	struct h868_device *dev = container_of(work, struct h868_device,
					      release_work);
	bool module_ref = dev->release_module_ref;

	h868_unregister_dvb(dev);
	i2c_del_adapter(&dev->i2c[1].adapter);
	i2c_del_adapter(&dev->i2c[0].adapter);
	h868_free_urbs(dev);
	usb_put_dev(dev->udev);
	kfree(dev);
	if (module_ref)
		module_put(THIS_MODULE);
}

static void h868_defer_release(struct h868_device *dev)
{
	/* During module exit a new reference is forbidden. Exit itself drains
	 * our private queue after usb_deregister() has finished disconnecting.
	 */
	dev->release_module_ref = try_module_get(THIS_MODULE);
	queue_work(h868_release_wq, &dev->release_work);
}

static void h868_quiesce(struct h868_device *dev, struct device *logdev)
{
	int ret;

	mutex_lock(&dev->stream_mutex);
	WRITE_ONCE(dev->ready, false);
	ret = h868_stop_stream(dev);
	if (ret && ret != -ENODEV && ret != -ESHUTDOWN)
		dev_warn(logdev, "TS stop during disconnect: %d\n", ret);
	mutex_lock(&dev->io_mutex);
	ret = h868_power_off(dev);
	/* All later frontend/I2C and feed callbacks see an inert instance.
	 * No deferred cleanup may touch hardware, especially after rebind.
	 */
	WRITE_ONCE(dev->disconnected, true);
	mutex_unlock(&dev->io_mutex);
	mutex_unlock(&dev->stream_mutex);
	if (ret && ret != -ENODEV && ret != -ESHUTDOWN)
		dev_warn(logdev, "power-off during disconnect: %d\n", ret);
	cancel_work_sync(&dev->stream_error_work);
	h868_notify_error(dev, -ENODEV, true);
}

static int h868_probe(struct usb_interface *intf,
		       const struct usb_device_id *id)
{
	struct usb_device *udev = interface_to_usbdev(intf);
	struct usb_host_interface *alt;
	struct h868_device *dev;
	int ret, i, added = 0;
	bool ts_endpoint = false;

	if (intf->cur_altsetting->desc.bInterfaceNumber != 1)
		return -ENODEV;
	if (!le16_to_cpu(udev->descriptor.bcdDevice)) {
		dev_err(&intf->dev, "unvalidated legacy bridge protocol (bcdDevice=0)\n");
		return -ENODEV;
	}
	dev = kzalloc(sizeof(*dev), GFP_KERNEL);
	if (!dev)
		return -ENOMEM;
	dev->udev = usb_get_dev(udev);
	dev->control_intf = intf;
	mutex_init(&dev->io_mutex);
	mutex_init(&dev->stream_mutex);
	spin_lock_init(&dev->stream_lock);
	INIT_WORK(&dev->stream_error_work, h868_stream_error_work);
	INIT_WORK(&dev->release_work, h868_release_work);
	init_completion(&dev->frontend_released);
	dev->stream_intf = usb_ifnum_to_if(udev, 2);
	if (!dev->stream_intf) {
		ret = -ENODEV;
		goto free_dev;
	}
	alt = usb_altnum_to_altsetting(dev->stream_intf, 0);
	if (!alt) {
		ret = -ENODEV;
		goto free_dev;
	}
	for (i = 0; i < alt->desc.bNumEndpoints; i++) {
		struct usb_endpoint_descriptor *ep = &alt->endpoint[i].desc;

		if (usb_endpoint_is_bulk_in(ep) &&
		    ep->bEndpointAddress == H868_TS_EP)
			ts_endpoint = true;
	}
	if (!ts_endpoint) {
		dev_err(&intf->dev, "interface 2 alt 0 has no bulk TS endpoint 0x81\n");
		ret = -ENODEV;
		goto free_dev;
	}
	ret = usb_driver_claim_interface(&h868_usb_driver, dev->stream_intf, dev);
	if (ret) {
		dev_err(&intf->dev, "cannot claim TS interface 2: %d\n", ret);
		goto free_dev;
	}
	ret = usb_set_interface(udev, 2, 0);
	if (ret)
		goto release_interface;
	ret = h868_alloc_urbs(dev);
	if (ret)
		goto free_urbs;
	mutex_lock(&dev->io_mutex);
	ret = h868_power_on(dev);
	mutex_unlock(&dev->io_mutex);
	if (ret)
		goto power_off;
	for (i = 0; i < ARRAY_SIZE(dev->i2c); i++) {
		struct h868_i2c *bus = &dev->i2c[i];

		bus->dev = dev;
		bus->bus = i + 1;
		bus->adapter.owner = THIS_MODULE;
		bus->adapter.algo = &h868_i2c_algorithm;
		bus->adapter.quirks = &h868_i2c_quirks;
		bus->adapter.dev.parent = &udev->dev;
		snprintf(bus->adapter.name, sizeof(bus->adapter.name),
			 "H868 I2C bus %u", bus->bus);
		i2c_set_adapdata(&bus->adapter, bus);
		ret = i2c_add_adapter(&bus->adapter);
		if (ret)
			goto remove_i2c;
		added++;
	}
	ret = h868_register_dvb(dev);
	if (ret) {
		if (dev->dmxdev_registered)
			goto release_dvb;
		goto remove_i2c;
	}
	usb_set_intfdata(intf, dev);
	WRITE_ONCE(dev->ready, true);
	dev_info(&intf->dev, H868_NAME " registered as DVB adapter %d\n",
		 dev->adapter.num);
	return 0;

release_dvb:
	h868_quiesce(dev, &intf->dev);
	usb_set_intfdata(dev->stream_intf, NULL);
	usb_driver_release_interface(&h868_usb_driver, dev->stream_intf);
	h868_defer_release(dev);
	return ret;

remove_i2c:
	while (added)
		i2c_del_adapter(&dev->i2c[--added].adapter);
power_off:
	mutex_lock(&dev->io_mutex);
	i = h868_power_off(dev);
	mutex_unlock(&dev->io_mutex);
	if (i)
		dev_warn(&intf->dev, "power-off after probe failure: %d\n", i);
free_urbs:
	h868_free_urbs(dev);
release_interface:
	usb_set_intfdata(dev->stream_intf, NULL);
	usb_driver_release_interface(&h868_usb_driver, dev->stream_intf);
free_dev:
	usb_put_dev(dev->udev);
	kfree(dev);
	return ret;
}

static void h868_disconnect(struct usb_interface *intf)
{
	struct h868_device *dev = usb_get_intfdata(intf);
	struct usb_interface *other;

	if (!dev)
		return;
	/* Either claimed interface can be unbound first. Prevent recursive
	 * teardown when releasing its sibling interface below.
	 */
	usb_set_intfdata(dev->control_intf, NULL);
	usb_set_intfdata(dev->stream_intf, NULL);
	other = intf == dev->control_intf ? dev->stream_intf : dev->control_intf;
	h868_quiesce(dev, &intf->dev);
	/* Release both USB interfaces before queuing anything: an old instance
	 * must neither release interfaces nor power down a freshly rebound one.
	 */
	usb_driver_release_interface(&h868_usb_driver, other);
	h868_defer_release(dev);
}

static const struct usb_device_id h868_usb_ids[] = {
	{ USB_DEVICE_INTERFACE_NUMBER(0x07ca, 0x0868, 1) },
	{ }
};
MODULE_DEVICE_TABLE(usb, h868_usb_ids);

static struct usb_driver h868_usb_driver = {
	.name = "h868",
	.id_table = h868_usb_ids,
	.probe = h868_probe,
	.disconnect = h868_disconnect,
};
static int __init h868_init(void)
{
	int ret;

	h868_release_wq = alloc_workqueue("h868-release", WQ_UNBOUND, 0);
	if (!h868_release_wq)
		return -ENOMEM;
	ret = usb_register(&h868_usb_driver);
	if (ret)
		destroy_workqueue(h868_release_wq);
	return ret;
}

static void __exit h868_exit(void)
{
	usb_deregister(&h868_usb_driver);
	destroy_workqueue(h868_release_wq);
}

module_init(h868_init);
module_exit(h868_exit);

MODULE_DESCRIPTION("H868 standalone USB ISDB-T receiver");
MODULE_AUTHOR("H868 Linux driver contributors");
MODULE_VERSION("0.1.0");
MODULE_LICENSE("GPL");
