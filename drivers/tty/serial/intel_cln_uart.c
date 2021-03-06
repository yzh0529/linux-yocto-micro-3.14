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
/*
 * Intel Clanton DMA-UART driver
 *
 * The hardware here consists of
 *	1 x MMIO BAR with 16550 compatible deisgnware UART regs - byte aligned
 *	1 x MMIO BAR with a designware DMAC - modified for byte aligned bursts
 * Lots of code stolen with pride from pch_uart.c/mfd.c
 *
 * DMA Config - set by hardware as a default
 *
 * Channel 0 : RX (Device to host)
 *	CTL0_LO : 0x00304837
 *	CTL0_HI : 0x00000002
 *	CFG0_LO : 0x00000C00 (HS_DST_SRC | HS_SEL_SRC)
 *	CFG0_HI : 0x00000004
 *
 *
 * Channel 1 : TX (Host to device)
 *	CTL1_LO : 0x00304837
 *	CTL1_HI : 0x00000002 
 *	CFG1_LO : 0x00000C20 (HS_DST_SRC | HS_SEL_SRC | CH_PRIOR:001)
 *	CFG1_HI : 0x00000004 (PROTCTL = 001)
 *
 */

#include <asm/io.h>
#include <linux/console.h>
#include <linux/debugfs.h>
#include <linux/intel_mid_dma.h>
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/pm_runtime.h>
#include <linux/serial_core.h>
#include <linux/serial_reg.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/tty_flip.h>

#define CLN_UART_MAX_INSTANCES	2
#define CLN_UART_DMA_CHANNELS	2
#define CLN_UART_DMA_TXCHAN	1
#define CLN_UART_DMA_RXCHAN	0
#define CLN_UART_FIFO_LEN	16
#define CLN_UART_DRIVER_DEVICE "ttyCLN"
#define CLN_UART_DMA_BUF_SIZE PAGE_SIZE

#define CLN_UART_MODE_MSI		0x00000001
#define CLN_UART_MODE_DMA		0x00000002

#define CLN_UART_DEFAULT_UARTCLK	1843200 /*   1.8432 MHz */

/* IIR bits - TO is non-standard */
#define INTEL_CLN_UART_IIR_MS		0x00
#define INTEL_CLN_UART_IIR_NOIRQ	0x01
#define INTEL_CLN_UART_IIR_THRE		0x02
#define INTEL_CLN_UART_IIR_RXD		0x04
#define INTEL_CLN_UART_IIR_RLS		0x06
#define INTEL_CLN_UART_IIR_BUSY		0x07
#define INTEL_CLN_UART_IIR_TO		0x08

static bool dma_enable = false;
static int intel_cln_uart_port_ct = 0;
module_param(dma_enable, bool, 0644);
MODULE_PARM_DESC(dma_enable, "Enable/disable DMA - default true");

/**
 * struct inel_cln_uart_buffer
 *
 * Descriptor for a UART bufer
 */
struct intel_cln_uart_buffer {
	dma_addr_t	dma_addr;
	unsigned char	*buf_virt;
	u32		offs;
	int		size;
};

/**
 * struct intel_cln_uart
 *
 * Describes an individual UART
 */
struct intel_cln_uart {
	char				name[10];
	int				uartclk;
	int				tx_dma_use;
	int				start_tx;
	int				start_rx;
	int				dma_tx_nent;
	int				tx_empty;

	spinlock_t			lock;
	struct dentry			*debugfs;
	struct device			*dev;
	struct dma_async_tx_descriptor	*desc_tx;
	struct dma_async_tx_descriptor	*desc_rx;
	struct dma_chan			*tx_chan;
	struct dma_chan			*rx_chan;
	struct middma_device		mid_dma;
	struct intel_cln_uart_buffer	txbuf;
	struct intel_cln_uart_buffer	rxbuf;
	struct intel_mid_dma_slave	dmas_rx;
	struct intel_mid_dma_slave	dmas_tx;
	struct scatterlist		*sg_tx_p;
	struct scatterlist		sg_rx;
	struct uart_port		port;

	unsigned char			fcr;
	unsigned char			ier;
	unsigned char			lcr;
	unsigned char			mcr;

	unsigned long			paddr;
	unsigned long			iolen;
	unsigned long			tx_trigger_level;
	unsigned long			rx_trigger_level;
	u32				irq;
	u32				mode;
};

/**
 * serial_in
 *
 * @param up: pointer to uart descriptor
 * @param offset: register offset
 *
 * Reads a register @ offset
 */
static inline unsigned int serial_in(struct intel_cln_uart *up, int offset)
{
	return  (unsigned int)readb(up->port.membase + offset);
}

/**
 * serial_out
 *
 * @param up: pointer to uart descriptor
 * @param offset: register offset
 *
 * Writes a register @ offset
 */
static inline void serial_out(struct intel_cln_uart *up, int offset, int value)
{
	unsigned char val = value & 0xff;
	writeb(val, up->port.membase + offset);
}

/**
 * intel_cln_uart_handle_rx_to
 *
 * For FIFO RX timeout just read the data until nothing else to read
 */
static int intel_cln_uart_hal_read(struct intel_cln_uart *up, unsigned char *buf,
			     int rx_size)
{
	int i;
	u8 rbr, lsr;

	lsr = serial_in(up, UART_LSR);
	for (i = 0, lsr = serial_in(up, UART_LSR);
	     i < rx_size && lsr & UART_LSR_DR;
	     lsr = serial_in(up, UART_LSR)) {
		rbr = serial_in(up, UART_RX);
		buf[i++] = rbr;
	}
	return i;
}

/**
 * intel_cln_uart_hal_write
 *
 * For FIFO RX timeout just read the data until nothing else to read
 */
static void intel_cln_uart_hal_write(struct intel_cln_uart *up, unsigned char *buf,
			     int tx_size)
{
	int i;
	unsigned int thr;

	for (i = 0; i < tx_size;) {
		thr = buf[i++];
		serial_out(up, UART_TX, thr);
	}
}

#ifdef CONFIG_DEBUG_FS
#define INTEL_CLN_UART_REGS_BUFSIZE	1024

/**
 * port_show_regs
 *
 * @param file: pointer to uart descriptor
 * @param user_buf: register offset
 * @param count:
 * @param ppos:
 *
 * Dump uart regs to string @ user_buf
 */
static ssize_t port_show_regs(struct file *file, char __user *user_buf,
				size_t count, loff_t *ppos)
{
	struct intel_cln_uart *up = file->private_data;
	char *buf;
	u32 len = 0;
	ssize_t ret;

	buf = kzalloc(INTEL_CLN_UART_REGS_BUFSIZE, GFP_KERNEL);
	if (!buf)
		return 0;

	len += snprintf(buf + len, INTEL_CLN_UART_REGS_BUFSIZE - len,
			"INTEL_CLN_UART port regs:\n");

	len += snprintf(buf + len, INTEL_CLN_UART_REGS_BUFSIZE - len,
			"=================================\n");
	len += snprintf(buf + len, INTEL_CLN_UART_REGS_BUFSIZE - len,
			"IER: \t\t0x%08x\n", serial_in(up, UART_IER));
	len += snprintf(buf + len, INTEL_CLN_UART_REGS_BUFSIZE - len,
			"IIR: \t\t0x%08x\n", serial_in(up, UART_IIR));
	len += snprintf(buf + len, INTEL_CLN_UART_REGS_BUFSIZE - len,
			"LCR: \t\t0x%08x\n", serial_in(up, UART_LCR));
	len += snprintf(buf + len, INTEL_CLN_UART_REGS_BUFSIZE - len,
			"MCR: \t\t0x%08x\n", serial_in(up, UART_MCR));
	len += snprintf(buf + len, INTEL_CLN_UART_REGS_BUFSIZE - len,
			"LSR: \t\t0x%08x\n", serial_in(up, UART_LSR));
	len += snprintf(buf + len, INTEL_CLN_UART_REGS_BUFSIZE - len,
			"MSR: \t\t0x%08x\n", serial_in(up, UART_MSR));
	len += snprintf(buf + len, INTEL_CLN_UART_REGS_BUFSIZE - len,
			"FCR: \t\t0x%08x\n", serial_in(up, UART_FCR));

	if (len > INTEL_CLN_UART_REGS_BUFSIZE)
		len = INTEL_CLN_UART_REGS_BUFSIZE;

	ret =  simple_read_from_buffer(user_buf, count, ppos, buf, len);
	kfree(buf);
	return ret;
}

static const struct file_operations port_regs_ops = {
	.owner		= THIS_MODULE,
	.open		= simple_open,
	.read		= port_show_regs,
	.llseek		= default_llseek,
};

/**
 * intel_cln_uart_debugfs_init
 *
 * @param up: pointer to uart descriptor
 *
 * Create a debug FS entry for the UART and associated register entries
 */
static int intel_cln_uart_debugfs_init(struct intel_cln_uart *up)
{
	up->debugfs = debugfs_create_dir("intel_cln_uart", NULL);
	if (!up->debugfs)
		return -ENOMEM;

	debugfs_create_file(up->name, S_IFREG | S_IRUGO,
		up->debugfs, (void *)up, &port_regs_ops);
	return 0;
}

/**
 * intel_cln_uart_debugfs_remove
 *
 * @param up: pointer to uart descriptor
 *
 * Remove recursive debug FS entries for the UART
 */
static void intel_cln_uart_debugfs_remove(struct intel_cln_uart *intel_cln_uart)
{
	if (intel_cln_uart->debugfs)
		debugfs_remove_recursive(intel_cln_uart->debugfs);
}

#else
static inline int intel_cln_uart_debugfs_init(struct intel_cln_uart *intel_cln_uart)
{
	return 0;
}

static inline void intel_cln_uart_debugfs_remove(struct intel_cln_uart *intel_cln_uart)
{
}
#endif /* CONFIG_DEBUG_FS */

/**
 * intel_cln_uart_enable_ms
 *
 * @param up: pointer to uart port structure
 *
 * Enable the modem status interrupt
 */
static void intel_cln_uart_enable_ms(struct uart_port *port)
{
	struct intel_cln_uart *up =
		container_of(port, struct intel_cln_uart, port);

	up->ier |= UART_IER_MSI;
	serial_out(up, UART_IER, up->ier);
}

/**
 * intel_cln_uart_dma_tx_complete
 *
 * @param arg: Pointer to intel_cln_uart 
 *
 * TX DMA completion callback
 */
static void intel_cln_uart_dma_tx_complete(void *arg)
{
	struct intel_cln_uart *up = arg;
	struct uart_port *port = &up->port;
	struct circ_buf *xmit = &port->state->xmit;
	struct scatterlist *sg = up->sg_tx_p;
	int i;

	for (i = 0; i < up->dma_tx_nent; i++, sg++) {
		xmit->tail += sg_dma_len(sg);
		port->icount.tx += sg_dma_len(sg);
	}
	xmit->tail &= UART_XMIT_SIZE - 1;
	async_tx_ack(up->desc_tx);
	dma_unmap_sg(port->dev, sg, up->dma_tx_nent, DMA_TO_DEVICE);
	up->tx_dma_use = 0;
	up->dma_tx_nent = 0;
	kfree(up->sg_tx_p);

	/* TODO: move to function */
	up->ier |= UART_IER_THRI;
	serial_out(up, UART_IER, up->ier);
}

static int pop_tx(struct intel_cln_uart *up, int size)
{
	int count = 0;
	struct uart_port *port = &up->port;
	struct circ_buf *xmit = &port->state->xmit;

	if (uart_tx_stopped(port) || uart_circ_empty(xmit) || count >= size)
		goto pop_tx_end;

	do {
		int cnt_to_end =
		    CIRC_CNT_TO_END(xmit->head, xmit->tail, UART_XMIT_SIZE);
		int sz = min(size - count, cnt_to_end);
		intel_cln_uart_hal_write(up, &xmit->buf[xmit->tail], sz);
		xmit->tail = (xmit->tail + sz) & (UART_XMIT_SIZE - 1);
		count += sz;
	} while (!uart_circ_empty(xmit) && count < size);

pop_tx_end:
	dev_dbg(up->port.dev, "%d characters. Remained %d characters.(%lu)\n",
		 count, size - count, jiffies);

	return count;
}

static int pop_tx_x(struct intel_cln_uart *up, unsigned char *buf)
{
	int ret = 0;
	struct uart_port *port = &up->port;

	if (port->x_char) {
		dev_dbg(up->port.dev, "%s:X character send %02x (%lu)\n",
			__func__, port->x_char, jiffies);
		buf[0] = port->x_char;
		port->x_char = 0;
		ret = 1;
	}

	return ret;
}

static int push_rx(struct intel_cln_uart *up, const unsigned char *buf,
		   int size)
{
	struct uart_port *port;
	struct tty_struct *tty;

	port = &up->port;
	tty = tty_port_tty_get(&port->state->port);
	if (!tty) {
		dev_dbg(up->port.dev, "%s:tty is busy now", __func__);
		return -EBUSY;
	}

	tty_insert_flip_string(tty, buf, size);
	tty_flip_buffer_push(tty);
	tty_kref_put(tty);

	return 0;
}

/**
 * intel_cln_uart_dma_tx
 *
 * @param arg: Pointer to intel_cln_uart 
 *
 * Initiate a TX DMA transaction
 */
void intel_cln_uart_dma_tx(struct intel_cln_uart *up)
{
	struct uart_port *port = &up->port;
	struct circ_buf *xmit = &port->state->xmit;
	struct scatterlist *sg;
	int nent;
	int fifo_size;
	//int tx_empty;
	struct dma_async_tx_descriptor *desc;
	int num;
	int i;
	int bytes;
	int size;
	int rem;

	if (!up->start_tx) {
		dev_info(up->port.dev, "%s:Tx isn't started. (%lu)\n",
			__func__, jiffies);

		/* TODO: move to function */
		up->ier &= ~UART_IER_THRI;
		serial_out(up, UART_IER, up->ier);

		up->tx_empty = 1;
		return;
	}

	if (up->tx_dma_use) {
		dev_dbg(up->port.dev, "%s:Tx is not completed. (%lu)\n",
			__func__, jiffies);
				
		/* TODO: move to function */
		up->ier &= ~UART_IER_THRI;
		serial_out(up, UART_IER, up->ier);

		up->tx_empty = 1;
		return;
	}

	fifo_size = max((int)port->fifosize, 1);
	if (pop_tx_x(up, xmit->buf)) {
		intel_cln_uart_hal_write(up, xmit->buf, 1);
		port->icount.tx++;
		fifo_size--;
	}

	bytes = min((int)CIRC_CNT(xmit->head, xmit->tail,
			     UART_XMIT_SIZE), CIRC_CNT_TO_END(xmit->head,
			     xmit->tail, UART_XMIT_SIZE));
	if (!bytes) {
		dev_dbg(up->port.dev, "%s 0 bytes return\n", __func__);

		/* TODO: move to function */
		up->ier &= ~UART_IER_THRI;
		serial_out(up, UART_IER, up->ier);

		uart_write_wakeup(port);
		return;
	}

	if (bytes > fifo_size) {
		num = bytes / fifo_size + 1;
		size = fifo_size;
		rem = bytes % fifo_size;
	} else {
		num = 1;
		size = bytes;
		rem = bytes;
	}

	dev_dbg(up->port.dev, "%s num=%d size=%d rem=%d\n",
		__func__, num, size, rem);

	up->tx_dma_use = 1;

	up->sg_tx_p = kzalloc(sizeof(struct scatterlist)*num, GFP_ATOMIC);

	sg_init_table(up->sg_tx_p, num); /* Initialize SG table */
	sg = up->sg_tx_p;

	for (i = 0; i < num; i++, sg++) {
		if (i == (num - 1))
			sg_set_page(sg, virt_to_page(xmit->buf),
				    rem, fifo_size * i);
		else
			sg_set_page(sg, virt_to_page(xmit->buf),
				    size, fifo_size * i);
	}

	sg = up->sg_tx_p;
	nent = dma_map_sg(port->dev, sg, num, DMA_TO_DEVICE);
	if (!nent) {
		dev_err(up->port.dev, "%s:dma_map_sg Failed\n", __func__);
		return;
	}
	up->dma_tx_nent = nent;

	for (i = 0; i < nent; i++, sg++) {
		sg->offset = (xmit->tail & (UART_XMIT_SIZE - 1)) +
			      fifo_size * i;
		sg_dma_address(sg) = (sg_dma_address(sg) &
				    ~(UART_XMIT_SIZE - 1)) + sg->offset;
		if (i == (nent - 1))
			sg_dma_len(sg) = rem;
		else
			sg_dma_len(sg) = size;
	}

	desc = dmaengine_prep_slave_sg(up->tx_chan,
					up->sg_tx_p, nent, DMA_MEM_TO_DEV,
					DMA_PREP_INTERRUPT | DMA_CTRL_ACK);
	if (!desc) {
		dev_err(up->port.dev, "%s:device_prep_slave_sg Failed\n",
			__func__);
		return;
	}
	dma_sync_sg_for_device(port->dev, up->sg_tx_p, nent, DMA_TO_DEVICE);
	up->desc_tx = desc;
	desc->callback = intel_cln_uart_dma_tx_complete;
	desc->callback_param = up;

	desc->tx_submit(desc);

	dma_async_issue_pending(up->tx_chan);
	up->tx_empty = 0;

	return;
}

/**
 * intel_cln_uart_start_tx
 *
 * @param arg: Pointer to intel_cln_uart 
 *
 * Enable TX interrupts on the UART @ port
 */
static void intel_cln_uart_start_tx(struct uart_port *port)
{
	struct intel_cln_uart *up =
		container_of(port, struct intel_cln_uart, port);

	up->start_tx = 1;
	up->ier |= UART_IER_THRI;
	serial_out(up, UART_IER, up->ier);
}

/**
 * intel_cln_uart_stop_tx
 *
 * @param arg: Pointer to intel_cln_uart 
 *
 * Disable TX interrupts on the UART @ port
 */
static void intel_cln_uart_stop_tx(struct uart_port *port)
{
	struct intel_cln_uart *up =
		container_of(port, struct intel_cln_uart, port);

	up->start_tx = 0;
	up->tx_dma_use = 0;
	up->ier &= ~UART_IER_THRI;
	serial_out(up, UART_IER, up->ier);
}

/**
 * intel_cln_uart_tx
 *
 * @up: pointer to UART instance
 *
 * Transmit characters in non-DMA mode
 */
static void intel_cln_uart_tx(struct intel_cln_uart *up)
{
	struct uart_port *port = &up->port;
	struct circ_buf *xmit = &port->state->xmit;
	int fifo_size;
	int tx_size;
	int size;
	int tx_empty;

	if (!up->start_tx) {
		dev_info(up->port.dev, "%s:Tx isn't started. (%lu)\n",
			__func__, jiffies);
		
		/* TODO: move to function */
		up->ier |= UART_IER_THRI;
		serial_out(up, UART_IER, up->ier);

		up->tx_empty = 1;
		return;
	}

	fifo_size = max((int)port->fifosize, 1);
	tx_empty = 1;
	if (pop_tx_x(up, xmit->buf)) {
		intel_cln_uart_hal_write(up, xmit->buf, 1);
		port->icount.tx++;
		tx_empty = 0;
		fifo_size--;
	}
	size = min(xmit->head - xmit->tail, fifo_size);
	if (size < 0)
		size = fifo_size;

	tx_size = pop_tx(up, size);
	if (tx_size > 0) {
		port->icount.tx += tx_size;
		tx_empty = 0;
	}

	up->tx_empty = tx_empty;

	if (tx_empty) {
		/* TODO: move to function */
		up->ier |= UART_IER_THRI;
		serial_out(up, UART_IER, up->ier);

		uart_write_wakeup(port);
	}

	return;
}

/**
 * intel_cln_uart_stop_rx
 *
 * Stop RX on the given UART
 */
static void intel_cln_uart_stop_rx(struct uart_port *port)
{
	struct intel_cln_uart *up =
		container_of(port, struct intel_cln_uart, port);

	up->start_rx = 0;
	up->ier &= ~UART_IER_RLSI;
	up->port.read_status_mask &= ~UART_LSR_DR;
	serial_out(up, UART_IER, up->ier);
}

/**
 * intel_cln_uart_handle_rx_to
 *
 * For FIFO RX timeout just read the data until nothing else to read
 */
static int intel_cln_uart_rx_to(struct intel_cln_uart *up)
{
	struct intel_cln_uart_buffer *buf;
	int rx_size;
	int ret;

	if (!up->start_rx) {
		up->ier &= ~UART_IER_RLSI;
		up->port.read_status_mask &= ~UART_LSR_DR;
		serial_out(up, UART_IER, up->ier);
		return 0;
	}

	buf = &up->rxbuf;
	do {
		rx_size = intel_cln_uart_hal_read(up, buf->buf_virt, buf->size);
		ret = push_rx(up, buf->buf_virt, rx_size);
		if (ret)
			return 0;
	} while (rx_size == buf->size);

	return 0;
}

/**
 * intel_cln_uart_dma_push_rx
 *
 * Take DMA RX data and push into the TTY layer
 */
static int intel_cln_uart_dma_push_rx(struct intel_cln_uart *up, int size)
{
	struct tty_struct *tty;
	int room;
	struct uart_port *port = &up->port;

	port = &up->port;
	tty = tty_port_tty_get(&port->state->port);
	if (!tty) {
		dev_dbg(up->port.dev, "%s:tty is busy now", __func__);
		return 0;
	}

	room = tty_buffer_request_room(tty, size);

	if (room < size)
		dev_warn(up->dev, "Rx overrun: dropping %u bytes\n",
			 size - room);
	if (!room)
		return room;

	tty_insert_flip_string(tty, sg_virt(&up->sg_rx), size);

	port->icount.rx += room;
	tty_kref_put(tty);

	return room;
}

/**
 * intel_cln_uart_dma_rx_complete
 *
 * Called when a UART RX interrupt happens - initiates a DMA transaction
 */
static void intel_cln_uart_dma_rx_complete(void *arg)
{
	struct intel_cln_uart *up = arg;
	struct uart_port *port = &up->port;
	struct tty_struct *tty = tty_port_tty_get(&port->state->port);
	int count;

	if (!tty) {
		dev_dbg(up->port.dev, "%s:tty is busy now", __func__);
		return;
	}

	dma_sync_sg_for_cpu(up->dev, &up->sg_rx, 1, DMA_FROM_DEVICE);
	count = intel_cln_uart_dma_push_rx(up, up->rx_trigger_level);
	if (count)
		tty_flip_buffer_push(tty);
	tty_kref_put(tty);
	async_tx_ack(up->desc_rx);
}

/**
 * intel_cln_uart_dma_rx
 *
 * Called when a UART RX interrupt happens - initiates a DMA transaction
 */
void intel_cln_uart_dma_rx(struct intel_cln_uart *up)
{
	struct dma_async_tx_descriptor *desc;

	sg_init_table(&up->sg_rx, 1); /* Initialize SG table */

	sg_dma_len(&up->sg_rx) = up->rx_trigger_level;

	sg_set_page(&up->sg_rx, virt_to_page(up->rxbuf.buf_virt),
		     sg_dma_len(&up->sg_rx), (unsigned long)up->rxbuf.buf_virt &
		     ~PAGE_MASK);

	sg_dma_address(&up->sg_rx) = up->rxbuf.dma_addr;

	desc = dmaengine_prep_slave_sg(up->rx_chan,
			&up->sg_rx, 1, DMA_DEV_TO_MEM,
			DMA_PREP_INTERRUPT | DMA_CTRL_ACK);

	if (!desc)
		return;

	up->desc_rx = desc;
	desc->callback = intel_cln_uart_dma_rx_complete;
	desc->callback_param = up;
	desc->tx_submit(desc);
	dma_async_issue_pending(up->rx_chan);
}

/**
 * check_modem_status
 *
 * @param up: pointer to UART descriptor
 *
 * Check modem status
 */
static inline void check_modem_status(struct intel_cln_uart *up)
{
	int status;

	status = serial_in(up, UART_MSR);

	if ((status & UART_MSR_ANY_DELTA) == 0)
		return;

	if (status & UART_MSR_TERI)
		up->port.icount.rng++;
	if (status & UART_MSR_DDSR)
		up->port.icount.dsr++;
	/* We may only get DDCD when HW init and reset */
	if (status & UART_MSR_DDCD)
		uart_handle_dcd_change(&up->port, status & UART_MSR_DCD);
	/* Will start/stop_tx accordingly */
	if (status & UART_MSR_DCTS)
		uart_handle_cts_change(&up->port, status & UART_MSR_CTS);

	wake_up_interruptible(&up->port.state->port.delta_msr_wait);
}

/**
 * intel_cln_uart_isr
 * 
 * @param irq: interrupt identifier
 * @param dev_id: pointer to the device structure data
 *
 * This handles the interrupt from one port. And calls into the DMAC interrupt
 * handler directly which is what will run our asynchronous tx/rx DMA callbacks
 * 
 */
static void intel_cln_uart_err_ir(struct intel_cln_uart *up, unsigned int lsr)
{
	up->fcr = serial_in(up, UART_FCR);

	/* Reset FIFO */
	up->fcr |= UART_FCR_CLEAR_RCVR;
	serial_out(up, UART_FCR, up->fcr);

	if (lsr & UART_LSR_FIFOE)
		dev_err(up->port.dev, "Error data in FIFO\n");

	if (lsr & UART_LSR_FE)
		dev_err(up->port.dev, "Framing Error\n");

	if (lsr & UART_LSR_PE)
		dev_err(up->port.dev, "Parity Error\n");

	if (lsr & UART_LSR_OE)
		dev_err(up->port.dev, "Overrun Error\n");
}

/**
 * intel_cln_uart_isr
 * 
 * @param irq: interrupt identifier
 * @param dev_id: pointer to the device structure data
 *
 * This handles the interrupt from one port. And calls into the DMAC interrupt
 * handler directly which is what will run our asynchronous tx/rx DMA callbacks
 * 
 */
static irqreturn_t intel_cln_uart_isr(int irq, void *dev_id)
{
	struct intel_cln_uart *up = dev_id;
	unsigned int iid = 0, lsr, ret = IRQ_HANDLED;
	unsigned long flags;

	if(likely(up->mode & CLN_UART_MODE_MSI)){
		/* TODO: see about moving this to the IO/APIC layer */
	}

	spin_lock_irqsave(&up->port.lock, flags);

	if (up->mode & CLN_UART_MODE_DMA) {
		/* Run the ISR for the DMA directly */
		intel_mid_dma_interrupt(irq, dev_id);
	}

	while ((iid = serial_in(up, UART_IIR)) > 1) {

		switch (iid) {
		case INTEL_CLN_UART_IIR_RLS:
			/* Receiver Line Status */
			lsr = serial_in(up, UART_LSR);
			if (lsr & (UART_LSR_FIFOE | UART_LSR_FE |
						UART_LSR_PE | UART_LSR_OE)) {
				intel_cln_uart_err_ir(up, lsr);
			}
			break;
		case INTEL_CLN_UART_IIR_RXD:
			/* Received Data Ready */
			if(up->mode & CLN_UART_MODE_DMA){
				intel_cln_uart_dma_rx(up);
			}else{
				intel_cln_uart_rx_to(up);
			}
			break;
		case INTEL_CLN_UART_IIR_TO:
			/* Received Data Ready (FIFO Timeout) */
			intel_cln_uart_rx_to(up);
			break;
		case INTEL_CLN_UART_IIR_THRE:
			/* Transmitter Holding Register Empty */
			if(up->mode & CLN_UART_MODE_DMA){
				intel_cln_uart_dma_tx(up);
			}else{
				intel_cln_uart_tx(up);
			}
			break;
		default:
			/* Never junp to this label */
			dev_err(up->port.dev, "%s:iid=%d (%lu)\n", __func__,
				iid, jiffies);
			ret = -1;
			break;
		}
	}

	check_modem_status(up);

	spin_unlock_irqrestore(&up->port.lock, flags);

	if(likely(up->mode & CLN_UART_MODE_MSI)){
		/* TODO: see about moving this to the IO/APIC layer */
	}

	return ret;
}

static unsigned int intel_cln_uart_tx_empty(struct uart_port *port)
{
	struct intel_cln_uart *up =
		container_of(port, struct intel_cln_uart, port);
	unsigned long flags;
	unsigned int ret;

	spin_lock_irqsave(&up->port.lock, flags);
	ret = up->tx_empty;
	spin_unlock_irqrestore(&up->port.lock, flags);

	return ret;
}

static unsigned int intel_cln_uart_get_mctrl(struct uart_port *port)
{
	struct intel_cln_uart *up =
		container_of(port, struct intel_cln_uart, port);
	unsigned char status;
	unsigned int ret;

	status = serial_in(up, UART_MSR);

	ret = 0;
	if (status & UART_MSR_DCD)
		ret |= TIOCM_CAR;
	if (status & UART_MSR_RI)
		ret |= TIOCM_RNG;
	if (status & UART_MSR_DSR)
		ret |= TIOCM_DSR;
	if (status & UART_MSR_CTS)
		ret |= TIOCM_CTS;
	return ret;
}

static void intel_cln_uart_set_mctrl(struct uart_port *port, unsigned int mctrl)
{
	struct intel_cln_uart *up =
		container_of(port, struct intel_cln_uart, port);
	unsigned char mcr = 0;

	if (mctrl & TIOCM_RTS)
		mcr |= UART_MCR_RTS;
	if (mctrl & TIOCM_DTR)
		mcr |= UART_MCR_DTR;
	if (mctrl & TIOCM_OUT1)
		mcr |= UART_MCR_OUT1;
	if (mctrl & TIOCM_OUT2)
		mcr |= UART_MCR_OUT2;
	if (mctrl & TIOCM_LOOP)
		mcr |= UART_MCR_LOOP;

	mcr |= up->mcr;

	serial_out(up, UART_MCR, mcr);
}

static void intel_cln_uart_break_ctl(struct uart_port *port, int break_state)
{
	struct intel_cln_uart *up =
		container_of(port, struct intel_cln_uart, port);
	unsigned long flags;

	pr_info("%s entry\n", __FUNCTION__);

	spin_lock_irqsave(&up->port.lock, flags);
	if (break_state == -1)
		up->lcr |= UART_LCR_SBC;
	else
		up->lcr &= ~UART_LCR_SBC;
	serial_out(up, UART_LCR, up->lcr);
	spin_unlock_irqrestore(&up->port.lock, flags);
}

/**
 * intel_cln_uart_startup
 *
 * @port: Pointer to the uart port to be started
 *
 */
static int intel_cln_uart_startup(struct uart_port *port)
{
	struct intel_cln_uart *up =
		container_of(port, struct intel_cln_uart, port);
	unsigned long flags;

	pr_info("%s entry\n", __FUNCTION__);

	/*
	 * Clear the FIFO buffers and disable them.
	 * (they will be reenabled in set_termios())
	 */
	serial_out(up, UART_FCR, UART_FCR_ENABLE_FIFO);
	serial_out(up, UART_FCR, UART_FCR_ENABLE_FIFO |
			UART_FCR_CLEAR_RCVR | UART_FCR_CLEAR_XMIT);
	serial_out(up, UART_FCR, 0);

	/* Clear the interrupt registers. */
	(void) serial_in(up, UART_LSR);
	(void) serial_in(up, UART_RX);
	(void) serial_in(up, UART_IIR);
	(void) serial_in(up, UART_MSR);

	/* Now, initialize the UART, default is 8n1 */
	serial_out(up, UART_LCR, UART_LCR_WLEN8);

	spin_lock_irqsave(&up->port.lock, flags);

	up->port.mctrl |= TIOCM_OUT2;
	intel_cln_uart_set_mctrl(&up->port, up->port.mctrl);

	/*
	 * Finally, enable interrupts.  Note: Modem status interrupts
	 * are set via set_termios(), which will be occurring imminently
	 * anyway, so we don't enable them here.
	 */
	if (!(up->mode & CLN_UART_MODE_DMA))
		up->ier = UART_IER_RLSI | UART_IER_RDI | UART_IER_RTOIE;
	else
		up->ier = 0;
	serial_out(up, UART_IER, up->ier);

	 /* And clear the interrupt registers again for luck. */
	(void) serial_in(up, UART_LSR);
	(void) serial_in(up, UART_RX);
	(void) serial_in(up, UART_IIR);
	(void) serial_in(up, UART_MSR);

	up->start_rx = 1;

	/* Coarse locking */
	spin_unlock_irqrestore(&up->port.lock, flags);

	return 0;
}

static void intel_cln_uart_shutdown(struct uart_port *port)
{
	struct intel_cln_uart *up =
		container_of(port, struct intel_cln_uart, port);
	unsigned long flags;

	pr_info("%s entry\n", __FUNCTION__);

	/* Disable interrupts from this port */
	up->ier = 0;
	up->start_tx = up->start_rx = 0;
	serial_out(up, UART_IER, 0);

	spin_lock_irqsave(&up->port.lock, flags);
	up->port.mctrl &= ~TIOCM_OUT2;
	intel_cln_uart_set_mctrl(&up->port, up->port.mctrl);
	spin_unlock_irqrestore(&up->port.lock, flags);

	/* Disable break condition and FIFOs */
	serial_out(up, UART_LCR, serial_in(up, UART_LCR) & ~UART_LCR_SBC);
	serial_out(up, UART_FCR, UART_FCR_ENABLE_FIFO |
				  UART_FCR_CLEAR_RCVR |
				  UART_FCR_CLEAR_XMIT);
	serial_out(up, UART_FCR, 0);

	/* Unmap DMA */
	if (up->mode & CLN_UART_MODE_DMA) {
		dma_unmap_single(port->dev, up->txbuf.dma_addr,
				 UART_XMIT_SIZE, DMA_TO_DEVICE);

		dma_unmap_single(port->dev, up->rxbuf.dma_addr,
				 CLN_UART_DMA_BUF_SIZE, DMA_FROM_DEVICE);
	}

}

/**
 * intel_cln_uart_set_termios
 *
 * @param port: Pointer to UART structure
 * @termios: Pointer to termios control structure
 * @old: Pointer to old termios structure
 *
 * Set the UART into the mode specified by the termios structure
 */
static void
intel_cln_uart_set_termios(struct uart_port *port, struct ktermios *termios,
		       struct ktermios *old)
{
	struct intel_cln_uart *up =
			container_of(port, struct intel_cln_uart, port);
	unsigned char cval;
	unsigned long flags;
	unsigned int baud, quot;
//	int div; TODO: on hardware

	pr_info("%s up %p port %p termios %p ktermios %p\n",
		__FUNCTION__, up, port, termios, old);

	switch (termios->c_cflag & CSIZE) {
	case CS5:
		cval = UART_LCR_WLEN5;
		break;
	case CS6:
		cval = UART_LCR_WLEN6;
		break;
	case CS7:
		cval = UART_LCR_WLEN7;
		break;
	default:
	case CS8:
		cval = UART_LCR_WLEN8;
		break;
	}

	if (termios->c_cflag & CSTOPB)
		cval |= UART_LCR_STOP;
	if (termios->c_cflag & PARENB)
		cval |= UART_LCR_PARITY;
	if (!(termios->c_cflag & PARODD))
		cval |= UART_LCR_EPAR;

	termios->c_cflag &= ~CMSPAR; /* Mark/Space parity is not supported */

	/*
	 * Ask the core to calculate the divisor for us.
	 */
	baud = uart_get_baud_rate(port, termios, old,
				  port->uartclk / 16 / 0xffff,
				  port->uartclk / 16);
	quot = uart_get_divisor(port, baud);

	pr_info("%s resulting baud rate was %d\n", __FUNCTION__, baud);

	/* Init to FIFO enabled mode - RX-trig (FIFO-2) TX-trig TX-trig (FIFO/2) */
	up->fcr = UART_FCR_ENABLE_FIFO | UART_FCR_T_TRIG_11 | UART_FCR_R_TRIG_11;
	if (up->mode & CLN_UART_MODE_DMA)
		up->fcr |= UART_FCR_DMA_SELECT;

	up->rx_trigger_level = up->port.fifosize-2;
	up->tx_trigger_level = up->port.fifosize/2;

	/*
	 * Ok, we're now changing the port state.  Do it with
	 * interrupts disabled.
	 */
	spin_lock_irqsave(&up->port.lock, flags);

	/* Update the per-port timeout */
	uart_update_timeout(port, termios->c_cflag, baud);

	up->port.read_status_mask = UART_LSR_OE | UART_LSR_THRE | UART_LSR_DR;
	if (termios->c_iflag & INPCK)
		up->port.read_status_mask |= UART_LSR_FE | UART_LSR_PE;
	if (termios->c_iflag & (BRKINT | PARMRK))
		up->port.read_status_mask |= UART_LSR_BI;

	/* Characters to ignore */
	up->port.ignore_status_mask = 0;
	if (termios->c_iflag & IGNPAR)
		up->port.ignore_status_mask |= UART_LSR_PE | UART_LSR_FE;
	if (termios->c_iflag & IGNBRK) {
		up->port.ignore_status_mask |= UART_LSR_BI;
		/*
		 * If we're ignoring parity and break indicators,
		 * ignore overruns too (for real raw support).
		 */
		if (termios->c_iflag & IGNPAR)
			up->port.ignore_status_mask |= UART_LSR_OE;
	}

	/* Ignore all characters if CREAD is not set */
	if ((termios->c_cflag & CREAD) == 0)
		up->port.ignore_status_mask |= UART_LSR_DR;

	/*
	 * CTS flow control flag and modem status interrupts, disable
	 * MSI by default
	 */
	up->ier &= ~UART_IER_MSI;
	if (UART_ENABLE_MS(&up->port, termios->c_cflag))
		up->ier |= UART_IER_MSI;

	if (termios->c_cflag & CRTSCTS)
		up->mcr |= UART_MCR_AFE | UART_MCR_RTS;
	else
		up->mcr &= ~UART_MCR_AFE;

	serial_out(up, UART_LCR, cval | UART_LCR_DLAB);	/* set DLAB */
	serial_out(up, UART_DLL, quot & 0xff);		/* LS of divisor */
	serial_out(up, UART_DLM, quot >> 8);		/* MS of divisor */
	serial_out(up, UART_LCR, cval);			/* reset DLAB */
	up->lcr = cval;					/* Save LCR */

	intel_cln_uart_set_mctrl(&up->port, up->port.mctrl);
	up->fcr = 0;
	serial_out(up, UART_FCR, up->fcr);

	/* Set IER state */
	serial_out(up, UART_IER, up->ier);

	/* Unlock spinlock */
	spin_unlock_irqrestore(&up->port.lock, flags);
}

static void
intel_cln_uart_pm(struct uart_port *port, unsigned int state,
	      unsigned int oldstate)
{
}

static void intel_cln_uart_release_port(struct uart_port *port)
{
}

static int intel_cln_uart_request_port(struct uart_port *port)
{
	return 0;
}

static void intel_cln_uart_config_port(struct uart_port *port, int flags)
{
	struct intel_cln_uart *up =
		container_of(port, struct intel_cln_uart, port);
	up->port.type = PORT_MFD;
}

/**
 * intel_cln_uart_verify_port
 *
 * @param port: Pointer to UART descriptor
 * @param ser: Serail configuration structure
 *
 * Sets the port into hi-speed/lo-speed mode
 */
static int
intel_cln_uart_verify_port(struct uart_port *port, struct serial_struct *ser)
{
	struct intel_cln_uart *up =
		container_of(port, struct intel_cln_uart, port);

	if (ser->flags & UPF_LOW_LATENCY) {
		dev_info(up->port.dev,
			"CLN UART : Use PIO Mode (without DMA)\n");
		up->mode &= ~CLN_UART_MODE_DMA;
		ser->flags &= ~UPF_LOW_LATENCY;
	} else {
		up->mode |= CLN_UART_MODE_DMA;
		dev_info(up->port.dev, "CLN UART : Use DMA Mode\n");
	}

	return 0;
}

/**
 * intel_cln_uart_type
 *
 * @param port: Pointer to UART descriptor
 *
 * Returns the type of the port
 */
static const char *
intel_cln_uart_type(struct uart_port *port)
{
	struct intel_cln_uart *up =
		container_of(port, struct intel_cln_uart, port);
	return up->name;
}

/* Mainly for uart console use */
static struct uart_driver intel_cln_uart_driver;

#ifdef CONFIG_INTEL_CLN_UART_CONSOLE

static struct intel_cln_uart *intel_cln_uart_ports[2];
#define BOTH_EMPTY (UART_LSR_TEMT | UART_LSR_THRE)

/* Wait for transmitter & holding register to empty */
static inline void wait_for_xmitr(struct intel_cln_uart *up)
{
	unsigned int status, tmout = 1000;

	/* Wait up to 1ms for the character to be sent. */
	do {
		status = serial_in(up, UART_LSR);

		if (status & UART_LSR_BI)
			up->lsr_break_flag = UART_LSR_BI;

		if (--tmout == 0)
			break;
		udelay(1);
	} while (!(status & BOTH_EMPTY));

	/* Wait up to 1s for flow control if necessary */
	if (up->port.flags & UPF_CONS_FLOW) {
		tmout = 1000000;
		while (--tmout &&
		       ((serial_in(up, UART_MSR) & UART_MSR_CTS) == 0))
			udelay(1);
	}
}

static void intel_cln_uart_console_putchar(struct uart_port *port, int ch)
{
	struct intel_cln_uart *up =
		container_of(port, struct intel_cln_uart, port);

	wait_for_xmitr(up);
	serial_out(up, UART_TX, ch);
}

/*
 * Print a string to the serial port trying not to disturb
 * any possible real use of the port...
 *
 *	The console_lock must be held when we get here.
 */
static void
intel_cln_uart_console_write(struct console *co, const char *s, unsigned int count)
{
	struct intel_cln_uart *up = &intel_cln_uart_ports[co->index];
	unsigned long flags;
	unsigned int ier;
	int locked = 1;

	local_irq_save(flags);
	if (up->port.sysrq)
		locked = 0;
	else if (oops_in_progress) {
		locked = spin_trylock(&up->port.lock);
	} else
		spin_lock(&up->port.lock);

	/* First save the IER then disable the interrupts */
	ier = serial_in(up, UART_IER);
	serial_out(up, UART_IER, 0);

	uart_console_write(&up->port, s, count, intel_cln_uart_console_putchar);

	/*
	 * Finally, wait for transmitter to become empty
	 * and restore the IER
	 */
	wait_for_xmitr(up);
	serial_out(up, UART_IER, ier);

	if (locked)
		spin_unlock(&up->port.lock);
	local_irq_restore(flags);
}

static struct console intel_cln_uart_console;

static int __init
intel_cln_uart_console_setup(struct console *co, char *options)
{
	struct intel_cln_uart *up;
	int baud = 115200;
	int bits = 8;
	int parity = 'n';
	int flow = 'n';

	if (co->index == -1 || co->index >= intel_cln_uart_driver.nr)
		co->index = 0;
	up = intel_cln_uart_ports[co->index];
	if (!up)
		return -ENODEV;

	if (options)
		uart_parse_options(options, &baud, &parity, &bits, &flow);

	return uart_set_options(&up->port, co, baud, parity, bits, flow);
}


static struct uart_driver intel_cln_uart_driver;
static struct console intel_cln_uart_console = {
	.name		= "ttyCLN",
	.write		= intel_cln_uart_console_write,
	.device		= uart_console_device,
	.setup		= intel_cln_uart_console_setup,
	.flags		= CON_PRINTBUFFER,
	.index		= -1,
	.data		= &intel_cln_uart_driver,
};

#define INTEL_CLN_UART_CONSOLE	(&intel_cln_uart_console)
#else
#define INTEL_CLN_UART_CONSOLE	NULL
#endif

static struct uart_driver intel_cln_uart_driver = {
	.owner = THIS_MODULE,
	.driver_name = KBUILD_MODNAME,
	.dev_name = CLN_UART_DRIVER_DEVICE,
	.major = TTY_MAJOR,
	.minor = 129,
	.nr = CLN_UART_MAX_INSTANCES,
	.cons = INTEL_CLN_UART_CONSOLE,
};

static struct uart_ops intel_cln_uart_ops = {
	.tx_empty	= intel_cln_uart_tx_empty,
	.set_mctrl	= intel_cln_uart_set_mctrl,
	.get_mctrl	= intel_cln_uart_get_mctrl,
	.stop_tx	= intel_cln_uart_stop_tx,
	.start_tx	= intel_cln_uart_start_tx,
	.stop_rx	= intel_cln_uart_stop_rx,
	.enable_ms	= intel_cln_uart_enable_ms,
	.break_ctl	= intel_cln_uart_break_ctl,
	.startup	= intel_cln_uart_startup,
	.shutdown	= intel_cln_uart_shutdown,
	.set_termios	= intel_cln_uart_set_termios,
	.pm		= intel_cln_uart_pm,
	.type		= intel_cln_uart_type,
	.release_port	= intel_cln_uart_release_port,
	.request_port	= intel_cln_uart_request_port,
	.config_port	= intel_cln_uart_config_port,
	.verify_port	= intel_cln_uart_verify_port,
};

/**
 * intel_cln_dma_chan_filter
 *
 * Simple descriptor disjunct function
 */
static bool intel_cln_dma_chan_filter(struct dma_chan * chan, void *param)
{
//	struct intel_mid_dma_slave *dws = param;

//	pr_info("%s compare device %p to %p\n", __FUNCTION__, dws->dma_dev, chan->device->dev);

	//return dws->dmac && (&dws->dmac->dev == chan->device->dev);
	return 1;	// TBD
}

/**
 * intel_cln_uart_probe
 *
 * @param dev: the PCI device matching
 * @param id: entry in the match table
 * @return 0
 *
 * Callback from PCI layer when dev/vendor ids match.
 * Sets up necessary resources
 */
static int intel_cln_uart_probe(struct pci_dev *pdev,
				const struct pci_device_id *id)
{
	dma_cap_mask_t mask;
	int ret = 0;
	struct intel_cln_uart *up = NULL;
	unsigned long flags = 0, len = 0;

	printk(KERN_INFO "Intel Clanton UART-DMA (ID: %04x:%04x)\n",
		pdev->vendor, pdev->device);

	/* Driver desc */
	up = kzalloc(sizeof(struct intel_cln_uart), GFP_KERNEL);
	if (up == NULL){
		ret = -ENOMEM;
		goto err;
	}
	up->mid_dma.pdev = pci_dev_get(pdev);

	ret = pci_enable_device(pdev);
	if (ret){
		goto err;
	}

	/* Attempt MSI enable */
	//if(pci_enable_msi(pdev)){
	if(1){
		dev_warn(&pdev->dev, "MSI enable fail\n");
		flags = IRQF_SHARED;
	}else{
		/*
		 * MSI enable good - set IRQ type to level. This seems wrong
		 * since PCI is an edge triggered interrupt system - but, the IP
		 * block connected to the bridge is level triggered. Setting the
		 * IRQ type to LEVEL_HIGH will trigger the
		 * io_apic->irq_mask()/unmask() functions to be automagically
		 * called by the kernel - which saves us from having to do nasty
		 * PCI config space writes explicitely in the ISR - kernel
		 * entry/exit functions will do that for us
		 */
		irq_set_irq_type(pdev->irq, IRQ_TYPE_LEVEL_HIGH);
		up->mode |= CLN_UART_MODE_MSI;
	}

	/* DMA hook */
	if(dma_enable == true){
		up->mode |= CLN_UART_MODE_DMA;
	}
	up->mode = 0;

	/* Hook an IRQ - in whichever mode */
	ret = request_irq(pdev->irq, intel_cln_uart_isr, flags, KBUILD_MODNAME,
			  up);
	if (ret) {
		dev_err(&pdev->dev, "can not get IRQ\n");
		goto err_dev;
	}

	/* Add debugfs entries */
	intel_cln_uart_debugfs_init(up);

	/* Init spinlock */
	spin_lock_init(&up->lock);

	/* UART regs on BAR0 */
	up->port.mapbase = pci_resource_start(pdev, 0);
	len = pci_resource_len(pdev, 0);
	up->port.membase = ioremap_nocache(up->port.mapbase, len);
	if(up->port.membase == NULL){
		ret = -ENODEV;
		goto err_dev;
	}

	/* Init DMA driver */
	up->mid_dma.max_chan = CLN_UART_DMA_CHANNELS;	/* Max channels */
	up->mid_dma.chan_base = 0;			/* Index start */
	up->mid_dma.block_size = CLN_UART_FIFO_LEN;	/* MAX DMA block */
	up->mid_dma.pimr_mask = 0;			/* Per int regs bool */

	ret = intel_cln_dma_probe(pdev, &up->mid_dma);
	if(ret != 0){
		dev_err(&pdev->dev, "Unable to init DMA sub-system\n");
		goto err_dev;
	}

	/* Request DMA channels TODO: move to startup() once debugged on hw */
	dma_cap_zero(mask);
	dma_cap_set(DMA_SLAVE, mask);

	up->rx_chan = dma_request_channel(mask, intel_cln_dma_chan_filter, &up->dmas_rx);
	if(up->rx_chan == NULL){
		dev_err(&pdev->dev, "Unable to hook DMA RX channel\n");
		goto err_bar0;
	};
	up->dmas_rx.hs_mode = LNW_DMA_SW_HS;
	up->dmas_rx.cfg_mode = LNW_DMA_PER_TO_MEM;

	up->tx_chan = dma_request_channel(mask, intel_cln_dma_chan_filter, &up->dmas_tx);
	if(up->tx_chan == NULL){
		dev_err(&pdev->dev, "Unable to hook DMA RX channel\n");
		goto err_bar0;
	};
	up->dmas_tx.hs_mode = LNW_DMA_SW_HS;
	up->dmas_tx.cfg_mode = LNW_DMA_MEM_TO_PER;

	dev_info(&pdev->dev, "using %s for DMA RX %s for DMA TX\n",
			dev_name(&up->rx_chan->dev->device), dev_name(&up->tx_chan->dev->device));

	/* Enumerate port */
	up->irq = pdev->irq;
	up->dev = &pdev->dev;
	up->tx_empty = 1;

//	up->port_type = PORT_MAX_8250 + 10;	/* TODO: add to include/linux/serial_core.h */
	up->uartclk = CLN_UART_DEFAULT_UARTCLK;
	up->port.uartclk = up->uartclk;
	up->port.dev = &pdev->dev;
	up->port.irq = pdev->irq;
	up->port.iotype = UPIO_MEM;
	up->port.ops = &intel_cln_uart_ops;
	up->port.flags = UPF_BOOT_AUTOCONF;
	up->port.fifosize = 16;
	up->port.line = pdev->dev.id;
	snprintf(up->name, sizeof(up->name), "cln_port%d", intel_cln_uart_port_ct++);

	/* Get Consistent memory for DMA TODO: move to startup() once debugged on hw */
	up->rxbuf.buf_virt = dma_alloc_coherent(up->port.dev, up->port.fifosize,
				    &up->rxbuf.dma_addr, GFP_KERNEL);
	up->rxbuf.size = up->port.fifosize;

	/* Add UART */
	uart_add_one_port(&intel_cln_uart_driver, &up->port);
	pci_set_drvdata(pdev, up);

	pm_runtime_put_noidle(&pdev->dev);
	pm_runtime_allow(&pdev->dev);

	return 0;

err_bar0:
	iounmap(up->port.membase);
err_dev:
	free_irq(up->irq, NULL);
	pci_disable_device(pdev);
err:
	kfree(up);
	return ret;
}

/**
 * uart_remove
 *
 * @param pdev: PCI device
 * @return nothing
 *
 * Callback from PCI sub-system upon PCI dev removal
 */
static void intel_cln_uart_remove(struct pci_dev *pdev)
{
	struct intel_cln_uart *up = pci_get_drvdata(pdev);
	if (!up)
		return;

	/* Shutdown DMA */
	intel_cln_dma_remove(pdev, &up->mid_dma);

	/* TODO: move to remove() when h/w proved out */
	if (up->tx_chan) {
		dma_release_channel(up->tx_chan);
		up->tx_chan = NULL;
	}
	if (up->rx_chan) {
		dma_release_channel(up->rx_chan);
		up->rx_chan = NULL;
	}

	if (sg_dma_address(&up->sg_rx))
		dma_free_coherent(up->port.dev, up->port.fifosize,
				  sg_virt(&up->sg_rx),
				  sg_dma_address(&up->sg_rx));

	/* Remove UART */
	uart_remove_one_port(&intel_cln_uart_driver, &up->port);

	pci_set_drvdata(pdev, NULL);
	free_irq(up->irq, NULL);
	pci_disable_device(pdev);

	/* Remove debugfs entries */
	intel_cln_uart_debugfs_remove(up);

	kfree(up);
}

#ifdef CONFIG_PM

static int intel_cln_uart_suspend(struct pci_dev *pdev, pm_message_t state)
{
	struct intel_cln_uart *up = pci_get_drvdata(pdev);

	/* Suspend DMA regs */
	intel_cln_dma_suspend(&up->mid_dma);


	/* Suspend UART */
	uart_suspend_port(&intel_cln_uart_driver, &up->port);

	pci_save_state(pdev);
	pci_set_power_state(pdev, pci_choose_state(pdev, state));
        return 0;
}

static int intel_cln_uart_resume(struct pci_dev *pdev)
{
	struct intel_cln_uart *up = pci_get_drvdata(pdev);
	int ret;

	pci_set_power_state(pdev, PCI_D0);
	pci_restore_state(pdev);

	ret = pci_enable_device(pdev);
	if (ret){
		dev_warn(&pdev->dev,
			"INTEL_CLN_UART: can't re-enable device, try to continue\n");
	}

	uart_resume_port(&intel_cln_uart_driver, &up->port);

	/* Resume DMA regs */
	intel_cln_dma_resume(&up->mid_dma);

	return 0;
}

#else

#define intel_cln_uart_suspend	NULL
#define intel_cln_uart_resume	NULL

#endif

struct pci_device_id intel_cln_uart_ids[] = {
        { PCI_VDEVICE(INTEL, 0x0936), 0},
        { 0 }
};

MODULE_DEVICE_TABLE(pci, intel_cln_uart_ids);

/* PCI callbacks */
static struct pci_driver intel_cln_uart_pci_desc = {
	.name = "intel_cln_uart",
	.id_table = intel_cln_uart_ids,
	.probe = intel_cln_uart_probe,
	.remove = intel_cln_uart_remove,
	.suspend = intel_cln_uart_suspend,
	.resume = intel_cln_uart_resume,
};

/**
 * intel_cln_uart_init
 *
 * Module entry point
 */
static int __init intel_cln_uart_init(void)
{
	int ret;

	/* register as UART driver */
	ret = uart_register_driver(&intel_cln_uart_driver);
	if (ret < 0)
		return ret;

	/* register as PCI driver */
	ret = pci_register_driver(&intel_cln_uart_pci_desc);
	if (ret < 0)
		uart_unregister_driver(&intel_cln_uart_driver);

	return ret;
}

/**
 * intel_cln_uart_exit
 *
 * Module exit
 */
static void __exit intel_cln_uart_exit(void)
{
	pci_unregister_driver(&intel_cln_uart_pci_desc);
}

MODULE_AUTHOR("Bryan O'Donoghue <bryan.odonoghue@linux.intel.com>");
MODULE_DESCRIPTION("Intel Clanton UART-DMA driver");
MODULE_LICENSE("Dual BSD/GPL");

module_init(intel_cln_uart_init);
module_exit(intel_cln_uart_exit);
