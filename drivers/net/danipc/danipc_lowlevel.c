/*
 *	All files except if stated otherwise in the beginning of the file
 *	are under the ISC license:
 *	----------------------------------------------------------------------
 *	Copyright (c) 2015-2017, The Linux Foundation. All rights reserved.
 *	Copyright (c) 2010-2012 Design Art Networks Ltd.
 *
 *	Permission to use, copy, modify, and/or distribute this software for any
 *	purpose with or without fee is hereby granted, provided that the above
 *	copyright notice and this permission notice appear in all copies.
 *
 *	THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 *	WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 *	MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 *	ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 *	WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 *	ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 *	OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <linux/module.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/mutex.h>
#include <linux/irq.h>

#include "danipc_k.h"

#include "ipc_api.h"

#include "danipc_lowlevel.h"

/* Almost Full(AF) interrupt threshold */
#define AF_THRESHOLD		0x7D
#define AF_PCAP_THRESHOLD	0x7D

/* IPC FIFO interrupt register offsets */
#define FIFO_0_COUNTER		0x6c
#define FIFO_1_COUNTER		0x70
#define FIFO_2_COUNTER		0x74
#define FIFO_3_COUNTER		0x78

#define FIFO_0_STATUS		0x24
#define FIFO_1_STATUS		0x28
#define FIFO_2_STATUS		0x2c
#define FIFO_3_STATUS		0x30
#define FIFO_THR_AF_CFG_F0	0x34
#define FIFO_THR_AE_CFG_F0	0x38
#define CDU_INT0_MASK_F0	0x44
#define CDU_INT0_ENABLE_F0	0x4c
#define CDU_INT0_STATUS_F0	0x54
#define CDU_INT0_RAW_STATUS_F0	0x5C
#define CDU_INT0_CLEAR_F0	0x64

#define CDU_INT1_MASK_F0	0x48
#define CDU_INT1_ENABLE_F0	0x50
#define CDU_INT1_STATUS_F0	0x58
#define CDU_INT1_RAW_STATUS_F0	0x60
#define CDU_INT1_CLEAR_F0	0x68

#define FIFO_0_POP_COUNTER	0x6C
#define FIFO_POP_COUNTER_ENABLE 0x80

/* Pop counter set FIFO'd*/
#define FIFO_POP_SET_ALL_FIFOS 0x15

#define FIFO_SHIFT(n)		((n) * 8)
/* Almost Full(AF) interrupt indication bit */
#define IPC_INT0_FIFO_AF	5
#define IPC_INT1_FIFO_AF	21
#define IPC_INTR(intr)		(1 << (intr))

/* AF threshold offset for a FIFO */
#define AF_THR_FIFO_OFFSET(fifo) (fifo<<3)

/* Allocation of IPC FIFOS among CPU cores is evident from
 * Platform specific dts file.
 */
#define TCSR_IPC0_CDU0_INT0_MUX	0x1
#define TCSR_IPC0_CDU0_INT1_MUX	0x2
#define TCSR_IPC3_CDU1_INT0_MUX	0x4000
#define TCSR_IPC3_CDU1_INT1_MUX	0x8000
#define TCSR_IPC4_CDU0_INT0_MUX	0x10000
#define TCSR_IPC4_CDU1_INT0_MUX	0x20000

/* Mux IPC0_CDU0 and IPC3_CDU1 interrupts to APPS.
 * As APPS receives all its messages from
 * FIFOs in TCSR_IPC0_CDU0 alone.
 */
#define APPS_IPC_DATA_FIFO_INT	(TCSR_IPC0_CDU0_INT0_MUX)
#define APPS_IPC_PCAP_FIFO_INT	(TCSR_IPC3_CDU1_INT0_MUX)
#define APPS_IPC_HEX2_LOG_FIFO_INT	(TCSR_IPC4_CDU0_INT0_MUX)
#define APPS_IPC_HEX3_LOG_FIFO_INT	(TCSR_IPC4_CDU1_INT0_MUX)

/* IPC and Linux coexistence.
 * IPC uses/needs physical addresses with bit 31 set while Linux obviously
 * uses virtual addresses. So when writing an address to IPC / reading from IPC
 * make sure it is converted from virtual to IPC address
 * (physical address with bit 31 set) and vice versa.
 * For every cpuid (except my own) FIFO buffers of both priorities remapped.
 * It is guaranteed (?) that FIFO buffers are in contiguous memory of 16kB long.
 * So remapping of 2*16kB is a safe way to access all possible FIFO buffers.
 * For my own CPU just take physical address.
 * Vladik, 21.08.2011
 */

#define FIFO_MAP_SIZE		SZ_256K
#define FIFO_MAP_MASK		(FIFO_MAP_SIZE - 1)

uint8_t	__iomem		*ipc_buffers = NULL;

struct ipc_to_virt_map		ipc_to_virt_map[PLATFORM_MAX_NUM_OF_NODES][2];

void __iomem		*apps_ipc_mux;

/* Maps the priority (lo = 0, hi = 1) to it's corresponding register set */
uint32_t reg_prio_map[max_ipc_prio][5] = {
	{CDU_INT1_MASK_F0, CDU_INT1_ENABLE_F0, CDU_INT1_STATUS_F0,
	 CDU_INT1_RAW_STATUS_F0, CDU_INT1_CLEAR_F0},
	{CDU_INT0_MASK_F0, CDU_INT0_ENABLE_F0, CDU_INT0_STATUS_F0,
	 CDU_INT0_RAW_STATUS_F0, CDU_INT0_CLEAR_F0},
};

/* Index to one of registers in reg_prio_map */
#define CDU_INT_MASK_F0        0
#define CDU_INT_ENABLE_F0      1
#define CDU_INT_STATUS_F0      2
#define CDU_INT_RAW_STATUS_F0  3
#define CDU_INT_CLEAR_F0       4

/* Maps priority to appropriate AF bit */
uint32_t int_enable_prio[max_ipc_prio] = {IPC_INT1_FIFO_AF, IPC_INT0_FIFO_AF};

static uint32_t apps_ipc_mux_val[DANIPC_MAX_LFIFO] = {
	APPS_IPC_DATA_FIFO_INT,
	APPS_IPC_PCAP_FIFO_INT,
	APPS_IPC_HEX2_LOG_FIFO_INT,
	APPS_IPC_HEX3_LOG_FIFO_INT
};

static const struct ipc_buf_desc *find_ext_buf_from_addr(uint32_t addr)
{
	uint32_t buf;

	for (buf = 0; buf < num_ext_bufs; buf++) {
		if (addr >= ext_bufs[buf].phy_addr &&
		    addr < ext_bufs[buf].phy_addr + ext_bufs[buf].sz)
			return ext_bufs + buf;
	}

	return NULL;
}

void danipc_clear_interrupt(uint8_t fifo, uint32_t prio)
{
	const unsigned          base_addr = ipc_regs[fifo];

	if (unlikely(prio >= max_ipc_prio))
		return;

	__raw_writel_no_log(IPC_INTR(int_enable_prio[prio]),
			    (void *)(base_addr +
			    reg_prio_map[prio][CDU_INT_CLEAR_F0]));
}

void danipc_mask_interrupt(uint8_t fifo, uint32_t prio)
{
	const unsigned          base_addr = ipc_regs[fifo];

	if (unlikely(prio >= max_ipc_prio))
		return;

	__raw_writel_no_log(IPC_INTR(int_enable_prio[prio]),
			    (void *)(base_addr +
			    reg_prio_map[prio][CDU_INT_MASK_F0]));
}

void danipc_unmask_interrupt(uint8_t fifo, uint32_t prio)
{
	const unsigned          base_addr = ipc_regs[fifo];

	if (unlikely(prio >= max_ipc_prio))
		return;

	__raw_writel_no_log(~IPC_INTR(int_enable_prio[prio]),
			    (void *)(base_addr +
			    reg_prio_map[prio][CDU_INT_MASK_F0]));
}

static void *map_ipc_buffers(struct danipc_drvr *drv)
{
	resource_size_t res_start = drv->res_start[IPC_BUFS_RES];
	resource_size_t res_len = drv->res_len[IPC_BUFS_RES];

	ipc_buffers = ioremap_cache(res_start, res_len);

	if (ipc_buffers) {
		memset(ipc_buffers, 0, res_len);
	} else {
		pr_err("%s: cannot allocate IPC buffers!\n", __func__);
		BUG();
	}

	return ipc_buffers;
}

static void free_ipc_buffers(void)
{
	if (ipc_buffers) {
		iounmap(ipc_buffers);
		ipc_buffers = NULL;
	}
}

int init_own_ipc_to_virt_map(struct danipc_fifo *fifo)
{
	int	rc = 0;
	struct danipc_drvr *drvr = &danipc_driver;
	struct ipc_to_virt_map *high_map =
		&ipc_to_virt_map[fifo->node_id][ipc_trns_prio_1];
	struct ipc_to_virt_map *low_map =
		&ipc_to_virt_map[fifo->node_id][ipc_trns_prio_0];
	uint8_t __iomem *buf = (ipc_buffers == NULL) ?
		map_ipc_buffers(drvr) : ipc_buffers;

	if (buf) {
		/* This prevents remapping by remap_fifo_mem() */
		high_map->vaddr = buf;
		high_map->paddr = drvr->res_start[IPC_BUFS_RES];
		high_map->size = drvr->res_len[IPC_BUFS_RES];
		low_map->vaddr	= high_map->vaddr;
		low_map->paddr	= high_map->paddr;
		low_map->size = high_map->size;
	} else {
		rc = -1;
	}

	return rc;
}

/* Free IPC buffers for all remote FIFO's.*/
static void unmap_ipc_to_virt_map(struct danipc_drvr *drvr)
{
	uint8_t			cpuid;
	void			*vaddr_hi;
	void			*vaddr_lo;

	for (cpuid = 0; cpuid < PLATFORM_MAX_NUM_OF_NODES; cpuid++) {
		vaddr_hi = ipc_to_virt_map[cpuid][ipc_trns_prio_1].vaddr;
		vaddr_lo = ipc_to_virt_map[cpuid][ipc_trns_prio_0].vaddr;

		/* Exclude APPS IPC buffers */
		if (((uint8_t *)vaddr_hi >= ipc_buffers) &&
		    ((uint8_t *)vaddr_hi <
		     ipc_buffers + drvr->res_len[IPC_BUFS_RES]))
			continue;
		if (vaddr_hi)
			iounmap(vaddr_hi);
		if (vaddr_lo != vaddr_hi)
			iounmap(vaddr_lo);
		memset(&ipc_to_virt_map[cpuid][ipc_trns_prio_1], 0,
		       sizeof(ipc_to_virt_map[cpuid][ipc_trns_prio_1]));
	}
}

static void remap_fifo_mem(const int cpuid, const unsigned prio,
			   const uint32_t paddr)
{
	struct ipc_to_virt_map *const map = ipc_to_virt_map[cpuid];
	unsigned other_prio = (prio == ipc_trns_prio_0) ?
				ipc_trns_prio_1 : ipc_trns_prio_0;
	const struct ipc_buf_desc *desc = NULL;
	uint32_t start_addr;
	uint32_t map_size;
	uint32_t map_mask;

	/* If mem_map was not defined in DT, this will return NULL */
	desc = find_ext_buf_from_addr(paddr);

	if (desc != NULL) {
		start_addr = desc->phy_addr;
		map[prio].paddr = desc->phy_addr;
		map[prio].size  = desc->sz;
		map[other_prio].paddr = desc->phy_addr;
		map[other_prio].size  = desc->sz;
		map[prio].vaddr = ioremap_cache(start_addr, desc->sz);
		map[other_prio].vaddr = map[prio].vaddr;
	} else if (ipc_shared_mem_sizes[cpuid]) {
		map_size = ipc_shared_mem_sizes[cpuid];
		map_mask = map_size - 1;
		start_addr = ((paddr + map_mask) & ~map_mask) - map_size;
		map[prio].paddr = start_addr;
		map[prio].size = map_size;
		map[other_prio].paddr = start_addr;
		map[other_prio].size = map_size;
		map[prio].vaddr = ioremap_cache(start_addr, 2 * map_size);
		map[other_prio].vaddr = map[prio].vaddr;
	} else {
		map_size = FIFO_MAP_SIZE;
		map_mask = FIFO_MAP_MASK;
		start_addr = ((paddr + map_mask) & ~map_mask) - 2 * map_size;
		map[prio].paddr = start_addr;
		map[prio].size = map_size;
		map[prio].vaddr = ioremap_cache(start_addr, 2 * map_size);
	}

	if (!map[prio].vaddr) {
		pr_err(
			"%s:%d cpuid = %d priority = %u cannot remap FIFO memory at addr. 0x%x\n",
			__func__, __LINE__, cpuid, prio, start_addr);
		BUG();
	}
}

uint32_t virt_to_ipc(const int cpuid, const unsigned prio, void *vaddr)
{
	if (likely(prio <= ipc_trns_prio_1)) {
		struct ipc_to_virt_map *map = &ipc_to_virt_map[cpuid][prio];
		int offset;

		if (unlikely(!map->paddr)) {
			pr_err("%s:%d: cpuid = %d priority = %u unmapped\n",
			       __func__, __LINE__, cpuid, prio);
			BUG();
		}
		offset = (unsigned)vaddr - (unsigned)map->vaddr;
		return map->paddr + offset;
	}

	pr_err("%s:%d: cpuid = %d illegal priority = %u\n",
	       __func__, __LINE__, cpuid, prio);
	BUG();

	return 0;
}

void *ipc_to_virt(const int cpuid, const unsigned prio, const uint32_t ipc_addr)
{
	if (likely(prio <= ipc_trns_prio_1 ||
		   cpuid < PLATFORM_MAX_NUM_OF_NODES)) {
		struct ipc_to_virt_map	*map = &ipc_to_virt_map[cpuid][prio];
		const uint32_t	paddr = ipc_addr;
		int		offset;

		if (unlikely(!map->paddr))
			remap_fifo_mem(cpuid, prio, paddr);
		offset = paddr - map->paddr;
		return (uint8_t *)map->vaddr + offset;
	}

	pr_err("%s:%d: cpuid = %d illegal priority = %u\n",
	       __func__, __LINE__, cpuid, prio);
	BUG();

	return NULL;
}

static inline bool valid_ipc_msg_hdr(struct danipc_cdev *cdev,
				     const struct ipc_msg_hdr *hdr)
{
	if (hdr->msg_len > IPC_FIRST_BUF_DATA_SIZE_MAX) {
		dev_dbg(cdev->dev,
			"%s: receive the message with bad message len(%u)\n",
			__func__, hdr->msg_len);
		cdev->status.rx_oversize_msg++;
		return false;
	}
	if (!hdr->msg_len) {
		cdev->status.rx_zlen_msg++;
		return false;
	}
	if (ipc_get_node(hdr->dest_aid) != cdev->fifo->node_id) {
		dev_dbg(cdev->dev,
			"%s: receive the message with bad dest_aid(%u)\n",
			__func__, hdr->dest_aid);
		cdev->status.rx_inval_msg++;
		return false;
	}
	/* no buffer chain */
	if (hdr->next != NULL) {
		dev_dbg(cdev->dev,
			"%s: receive the message with next point(%p)\n",
			__func__, hdr->next);
		cdev->status.rx_chained_msg++;
		return false;
	}
	return true;
}

static int danipc_cdev_recv(struct danipc_cdev *cdev, uint8_t hwfifo)
{
	struct danipc_fifo *fifo = cdev->fifo;
	uint32_t reg_addr = ipc_regs[fifo->node_id];
	enum ipc_trns_prio prio =
		(hwfifo == 0) ? ipc_trns_prio_1 : ipc_trns_prio_0;
	struct rx_queue *pq = &cdev->rx_queue[prio];
	uint32_t msg_paddr;
	struct shm_buf *buf;
	struct ipc_msg_hdr *hdr;
	int n = 0;

	reg_addr += TCSR_IPC_IF_FIFO_RD_ACCESS_0_OFFSET + (8 * hwfifo);

	while (1) {
		msg_paddr = __raw_readl_no_log((void *)reg_addr);
		if (!msg_paddr)
			break;
		buf = shm_region_find_buf_by_pa(cdev->rx_region, msg_paddr);
		if (buf == NULL) {
			cdev->status.rx_error++;
			dev_warn(cdev->dev,
				 "%s: can't find buffer, paddr=0x%x\n",
				 __func__, msg_paddr);
			continue;
		}

		if (shm_bufpool_del_buf(&pq->bq, buf)) {
			cdev->status.rx_error++;
			dev_err(cdev->dev,
				"%s: buffer %p not in the buffer queue\n",
				__func__, buf);
			continue;
		}
		hdr = buf_vaddr(buf);
		ipc_msg_hdr_cache_invalid(hdr);

		if (!valid_ipc_msg_hdr(cdev, hdr)) {
			cdev->status.rx_drop++;
			dev_dbg(cdev->dev,
				"%s: drop message paddr=0x%x, vaddr=%p\n",
				__func__, msg_paddr, hdr);
			shm_bufpool_put_buf(&pq->freeq, buf);
		} else {
			cdev->status.rx++;
			cdev->status.rx_bytes += hdr->msg_len;
			shm_bufpool_put_buf(&pq->recvq, buf);
			ipc_msg_payload_cache_invalid(hdr);
			if (pq->recvq.count > pq->status.recvq_hi)
				pq->status.recvq_hi = pq->recvq.count;
			n++;
		}
	}

	danipc_cdev_refill_rx_b_fifo(cdev, prio);

	return n;
}

static void danipc_cdev_rx_poll(unsigned long data)
{
	struct danipc_cdev *cdev = (struct danipc_cdev *)data;
	unsigned long flags;
	int n;

	cdev->status.rx_poll++;

	spin_lock_irqsave(&cdev->rx_lock, flags);
	n = danipc_cdev_recv(cdev, 0);
	n += danipc_cdev_recv(cdev, 2);
	spin_unlock_irqrestore(&cdev->rx_lock, flags);

	if (n)
		wake_up(&cdev->rx_wq);
}

enum hrtimer_restart danipc_cdev_rx_timer_timeout(struct hrtimer *timer)
{
	struct danipc_cdev *cdev = container_of(timer,
						struct danipc_cdev, rx_timer);

	hrtimer_forward_now(timer, cdev->rx_poll_interval);
	tasklet_schedule(&cdev->rx_work);

	return HRTIMER_RESTART;
}

irqreturn_t danipc_cdev_interrupt(int irq, void *data)
{
	struct danipc_cdev *cdev = (struct danipc_cdev *)data;
	struct danipc_fifo *fifo = cdev->fifo;
	const unsigned	base_addr = ipc_regs[fifo->node_id];

	dev_dbg(cdev->dev, "%s: receive danipc IRQ, base_addr = 0x%x\n",
		__func__, base_addr);

	/* Mask all IPC interrupts and start the polling */
	__raw_writel_no_log(~0, (void *)(base_addr + CDU_INT0_MASK_F0));
	hrtimer_start(&cdev->rx_timer, cdev->rx_poll_interval,
		      HRTIMER_MODE_REL|HRTIMER_MODE_PINNED);

	return IRQ_HANDLED;
}

void danipc_cdev_init_rx_work(struct danipc_cdev *cdev)
{
	if (likely(cdev)) {
		tasklet_init(&cdev->rx_work,
			     danipc_cdev_rx_poll,
			     (unsigned long)cdev);
		hrtimer_init(&cdev->rx_timer, CLOCK_MONOTONIC,
			     HRTIMER_MODE_REL|HRTIMER_MODE_PINNED);
		cdev->rx_timer.function = danipc_cdev_rx_timer_timeout;
		cdev->rx_poll_interval = ns_to_ktime(
			DANIPC_CDEV_DEFAULT_RX_POLL_INTERVAL_IN_US * 1000);
	}
}

void danipc_cdev_stop_rx_work(struct danipc_cdev *cdev)
{
	if (likely(cdev)) {
		hrtimer_cancel(&cdev->rx_timer);
		tasklet_kill(&cdev->rx_work);
	}
}

irqreturn_t danipc_interrupt(int irq, void *data)
{
	/* By default we always pass packet_proc for low-prio
	 * danipc fifo.
	 */
	struct packet_proc_info *pproc =
		 (struct packet_proc_info *)data;
	struct danipc_if		*intf = pproc->intf;
	struct packet_proc		*proc_rx =
		&intf->drvr->proc_rx[pproc->rxproc_type];
	const unsigned	base_addr = ipc_regs[intf->rx_fifo_idx];
	uint8_t	prio = pproc - intf->pproc;
	uint32_t *reg_prio;

	if (unlikely(prio > max_ipc_prio))
		return IRQ_NONE;

	reg_prio = reg_prio_map[prio];

	/* Mask all IPC interrupts. */
	__raw_writel_no_log(~0,
			    (void *)(base_addr + reg_prio[CDU_INT_MASK_F0]));

	(proc_rx->schedule_work)(&pproc->rx_work);

	return IRQ_HANDLED;
}

void danipc_init_irq(struct danipc_fifo *fifo, uint32_t prio)
{
	const unsigned		 base_addr = ipc_regs[fifo->node_id];
	uint32_t int_val = IPC_INTR(int_enable_prio[prio]);
	uint32_t af_val = AF_THRESHOLD;
	uint32_t		*reg_prio;

	if (prio >= max_ipc_prio || fifo == NULL)
		return;

	/* Cannot request for separate interrupt on low priority FIFO and have
	 * character device. Char device has 1 interrupt for both hi and lo FIFO
	 */
	if (prio == 0 &&
	    fifo->owner_type == DANIPC_FIFO_OWNER_TYPE_CDEV) {
		pr_err("chardev and low priority FIFO interrupt not supported");
		return;
	}

	reg_prio = reg_prio_map[prio];

	if (fifo->owner_type == DANIPC_FIFO_OWNER_TYPE_CDEV)
		int_val |= IPC_INTR(IPC_INT0_FIFO_AF) << FIFO_SHIFT(2);

	if (fifo->idx == 0) {
		if (fifo->owner_type == DANIPC_FIFO_OWNER_TYPE_CDEV)
			af_val |= AF_THRESHOLD << FIFO_SHIFT(2);
	} else if (fifo->idx == 1) {
		af_val = AF_PCAP_THRESHOLD;
		if (fifo->owner_type == DANIPC_FIFO_OWNER_TYPE_CDEV)
			af_val |= AF_PCAP_THRESHOLD << FIFO_SHIFT(2);
	} else if (fifo->idx == 2 || fifo->idx == 3) {
		af_val = __raw_readl((void *)(base_addr + FIFO_THR_AF_CFG_F0));

		if (prio == 1)
			af_val |= AF_PCAP_THRESHOLD<<FIFO_SHIFT(0);
		else
			af_val |= AF_PCAP_THRESHOLD<<FIFO_SHIFT(2);
	} else {
		pr_err("%s: Unknown device passed %p\n", __func__ , fifo);
		return;
	}

	__raw_writel_no_log(af_val,
			    (void *)(base_addr + FIFO_THR_AF_CFG_F0));

	__raw_writel_no_log(FIFO_POP_SET_ALL_FIFOS,
			    (void *)(base_addr + FIFO_POP_COUNTER_ENABLE));
	__raw_writel_no_log(int_val,
			    (void *)(base_addr + reg_prio[CDU_INT_CLEAR_F0]));
	__raw_writel_no_log(int_val,
			    (void *)(base_addr + reg_prio[CDU_INT_ENABLE_F0]));
	__raw_writel_no_log(~int_val,
			    (void *)(base_addr + reg_prio[CDU_INT_MASK_F0]));

	/* Route interrupts from TCSR to APPS (relevant to APPS-FIFO) */
	/* TBD: makesure apps_ipc_mux is incremented by 4 bytes */
	__raw_writel_no_log(apps_ipc_mux_val[fifo->idx],
			    &((uint32_t *)apps_ipc_mux)[fifo->idx]);
}

uint32_t danipc_read_af_threshold(uint8_t fifo)
{
	const unsigned	base_addr = ipc_regs[fifo];
	uint32_t	af        =
		__raw_readl((void *)(base_addr + FIFO_THR_AF_CFG_F0));
	return af;
}

uint32_t danipc_set_af_threshold(uint8_t fifo, uint8_t n, uint8_t thr)
{
	const unsigned base_addr = ipc_regs[fifo];
	uint32_t       af;

	if (thr > 127) {
		pr_err("%s: Threshold value out of valid range (0-127) %d\n",
		       __func__, n);
		return 0;
	}
	if (n > 3) {
		pr_err("%s: fifo number(%d) out of valid range (0-3)\n",
		       __func__, n);
		return 0;
	}
	af = __raw_readl((void *)(base_addr + FIFO_THR_AF_CFG_F0));
	af = ((af & ~((((uint32_t)0x7f)) << (n*8))) |
	      (((uint32_t)(thr)) << (n*8)));
	__raw_writel_no_log(af, (void *)(base_addr + FIFO_THR_AF_CFG_F0));
	af = __raw_readl((void *)(base_addr + FIFO_THR_AF_CFG_F0));
	return af;
}

uint32_t danipc_read_ae_threshold(uint8_t fifo)
{
	const unsigned	base_addr = ipc_regs[fifo];
	uint32_t	ae        =
		__raw_readl((void *)(base_addr + FIFO_THR_AE_CFG_F0));
	return ae;
}

uint32_t danipc_set_ae_threshold(uint8_t fifo, uint8_t n, uint8_t thr)
{
	const unsigned base_addr = ipc_regs[fifo];
	uint32_t       ae;

	if (thr < 0 || thr > 127) {
		pr_err("%s: Threshold value out of valid range (0-127) %d\n",
		       __func__, n);
		return 0;
	}
	if (n < 0 || n > 3) {
		pr_err("%s: Unsupported fifo number %d\n", __func__, n);
		return 0;
	}
	ae = __raw_readl((void *)(base_addr + FIFO_THR_AE_CFG_F0));
	ae = ((ae & ~((((uint32_t)0x7f)) << (n*8))) |
	      ((uint32_t)(thr) << (n*8)));
	__raw_writel_no_log(ae, (void *)(base_addr + FIFO_THR_AE_CFG_F0));
	ae = __raw_readl((void *)(base_addr + FIFO_THR_AE_CFG_F0));
	return ae;
}

uint32_t danipc_read_fifo_counter(uint8_t fifo, uint8_t n)
{
	const unsigned	base_addr = ipc_regs[fifo];
	uint32_t status_offset = FIFO_0_COUNTER;
	uint32_t status;

	switch (n) {
	case 0:
		status_offset = FIFO_0_COUNTER;
		break;
	case 1:
		status_offset = FIFO_1_COUNTER;
		break;
	case 2:
		status_offset = FIFO_2_COUNTER;
		break;
	case 3:
		status_offset = FIFO_3_COUNTER;
		break;
	default:
		pr_err("%s: Unsupported fifo number %d\n", __func__, n);
		return 0;
	}

	pr_debug("%s: reading status record %p\n", __func__,
		 (void *)(base_addr + status_offset));
	status = __raw_readl((void *)(base_addr + status_offset));
	return status;
}

uint32_t danipc_read_fifo_status(uint8_t fifo, uint8_t n)
{
	const unsigned	base_addr = ipc_regs[fifo];
	uint32_t status_offset = FIFO_0_STATUS;
	uint32_t status;

	switch (n) {
	case 0:
		status_offset = FIFO_0_STATUS;
		break;
	case 1:
		status_offset = FIFO_1_STATUS;
		break;
	case 2:
		status_offset = FIFO_2_STATUS;
		break;
	case 3:
		status_offset = FIFO_3_STATUS;
		break;
	default:
		pr_err("%s: Unsupported fifo number %d\n", __func__, n);
		return 0;
	}

	pr_debug("%s: reading status record %p\n", __func__,
		 (void *)(base_addr + status_offset));
	status = __raw_readl((void *)(base_addr + status_offset));
	return status;
}

uint32_t danipc_read_fifo_irq_status_raw(uint8_t fifo)
{
	const unsigned base_addr = ipc_regs[fifo];
	uint32_t irq_status =
		__raw_readl((void *)(base_addr + CDU_INT0_RAW_STATUS_F0));
	return irq_status;
}

uint32_t danipc_read_fifo_irq_mask(uint8_t fifo)
{
	const unsigned base_addr = ipc_regs[fifo];
	uint32_t irq_mask = __raw_readl((void *)(base_addr + CDU_INT0_MASK_F0));
	return irq_mask;
}

uint32_t danipc_read_fifo_irq_enable(uint8_t fifo)
{
	const unsigned	base_addr = ipc_regs[fifo];
	uint32_t irq_enable	  =
		__raw_readl((void *)(base_addr + CDU_INT0_ENABLE_F0));
	return irq_enable;
}

uint32_t danipc_read_fifo_irq_status(uint8_t fifo)
{
	const unsigned	base_addr = ipc_regs[fifo];
	uint32_t irq_status	  =
		__raw_readl((void *)(base_addr + CDU_INT0_STATUS_F0));
	return irq_status;
}

void danipc_disable_irq(struct danipc_fifo *fifo, uint32_t prio)
{
	const unsigned	base_addr = ipc_regs[fifo->node_id];
	uint32_t int_val = IPC_INTR(int_enable_prio[prio]);
	uint32_t *reg_prio;

	if (prio >= max_ipc_prio || fifo == NULL)
		return;

	if (prio == 0 &&
	    fifo->owner_type == DANIPC_FIFO_OWNER_TYPE_CDEV) {
		pr_err("chardev and low priority FIFO interrupt not supported");
		return;
	}

	if (fifo->owner_type == DANIPC_FIFO_OWNER_TYPE_CDEV)
		int_val |= IPC_INTR(IPC_INT0_FIFO_AF) << FIFO_SHIFT(2);

	reg_prio = reg_prio_map[prio];

	/* Clear, disable and mask all interrupts from this CDU */
	__raw_writel_no_log(int_val,
			    (void *)(base_addr + reg_prio[CDU_INT_CLEAR_F0]));
	__raw_writel_no_log(0,
			    (void *)(base_addr + reg_prio[CDU_INT_ENABLE_F0]));
	__raw_writel_no_log(int_val,
			    (void *)(base_addr + reg_prio[CDU_INT_MASK_F0]));

	/* Route interrupts from TCSR to APPS (relevant to APPS-FIFO) */
	/* TBD: makesure apps_ipc_mux is incremented by 4 bytes */
	__raw_writel_no_log(0, &((uint32_t *)apps_ipc_mux)[fifo->idx]);
}

static void remap_agent_table(struct danipc_drvr *drv)
{
	agent_table = ioremap_nocache(drv->res_start[AGENT_TABLE_RES],
				      drv->res_len[AGENT_TABLE_RES]);
	if (!agent_table) {
		pr_err("%s: cannot remap IPC global agent table\n", __func__);
		BUG();
	}
}

static void unmap_agent_table(void)
{
	if (agent_table) {
		iounmap(agent_table);
		agent_table = NULL;
	}
}

static void prepare_node(const int cpuid)
{
	struct ipc_to_virt_map	*map;

	ipc_regs[cpuid] = (uintptr_t)ioremap_nocache(ipc_regs_phys[cpuid],
							ipc_regs_len[cpuid]);
	map = &ipc_to_virt_map[cpuid][ipc_trns_prio_0];
	atomic_set(&map->pending_skbs, 0);

	map = &ipc_to_virt_map[cpuid][ipc_trns_prio_1];
	atomic_set(&map->pending_skbs, 0);
}

static void prepare_nodes(void)
{
	int		n;

	for (n = 0; n < PLATFORM_MAX_NUM_OF_NODES; n++)
		if (ipc_regs_len[n])
			prepare_node(n);
}

static void unmap_nodes_memory(void)
{
	int		n;

	for (n = 0; n < PLATFORM_MAX_NUM_OF_NODES; n++)
		if (ipc_regs[n]) {
			iounmap((void __iomem *)ipc_regs[n]);
			ipc_regs[n] = 0;
		}
}

static void remap_apps_ipc_mux(struct danipc_drvr *drv)
{
	apps_ipc_mux = ioremap_nocache(
				drv->res_start[KRAIT_IPC_MUX_RES],
				drv->res_len[KRAIT_IPC_MUX_RES]);
	if (!apps_ipc_mux) {
		pr_err("%s: cannot remap APPS IPC mux\n", __func__);
		BUG();
	}
}

static void unmap_apps_ipc_mux(void)
{
	if (apps_ipc_mux) {
		iounmap(apps_ipc_mux);
		apps_ipc_mux = NULL;
	}
}

void danipc_ll_init(struct danipc_drvr *drv)
{
	prepare_nodes();
	remap_agent_table(drv);
	remap_apps_ipc_mux(drv);
	memset(agent_table, 0, drv->res_len[AGENT_TABLE_RES]);
	map_ipc_buffers(drv);
}

void danipc_ll_cleanup(struct danipc_drvr *drv)
{
	unmap_apps_ipc_mux();
	unmap_ipc_to_virt_map(drv);
	unmap_agent_table();
	unmap_nodes_memory();
	free_ipc_buffers();
}
