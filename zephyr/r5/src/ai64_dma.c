/* SPDX-License-Identifier: MIT */
/*
 * R5F0_0's DMA: one memory-to-memory channel of the main NAVSS UDMA.
 *
 * Zephyr has no UDMA driver for J721E, so this file does what Linux's and
 * U-Boot's k3-udma drivers do for a memcpy channel, polled:
 *
 * - From the device manager, over TI-SCI (Zephyr's driver): this host's
 *   ranges of UDMAP transmit and receive channels and of general-purpose
 *   rings; then ring configs for the channel's transmit ring (ring n for
 *   channel n) and a completion ring, the channel pair's configs as block
 *   copy channels (TI-SCI channel type 12), and the PSI-L pairing that
 *   joins the transmit thread to the receive thread.
 * - Then, with no more TI-SCI: each copy is one CPPI5 TR descriptor with
 *   one type-15 TR (source and destination, up to 64 KiB here). The core
 *   writes its address into the transmit ring and rings the ring's
 *   doorbell; the UDMA reads the descriptor, moves the bytes, and returns
 *   the descriptor to the completion ring, which the core polls.
 * - ai64_dma_copy_bell() puts a second TR after the copy, which writes a
 *   word: a doorbell for whoever waits on the copy. Every TR has the WAIT
 *   flag, so the second only starts once the first one's writes have
 *   landed, and the word is never seen before the bytes it stands for.
 *
 * The rings and the descriptor live in the non-cacheable 4 KiB that
 * boards/ai64_ipc.dtsi leaves unused (the slot for core 0 to itself), so
 * neither side needs cache work on them.
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/firmware/tisci/tisci.h>
#include <zephyr/sys/sys_io.h>
#include <zephyr/sys/barrier.h>
#include <zephyr/logging/log.h>
#include <errno.h>
#include <string.h>

#include "ai64_dma.h"
#include "ai64_shm.h"

LOG_MODULE_REGISTER(ai64_dma, LOG_LEVEL_INF);

/* TI-SCI device IDs (k3-j721e-main.dtsi). */
#define NAVSS_ID   199u /* main NAVSS: PSI-L pairing */
#define RINGACC_ID 211u
#define UDMAP_ID   212u

/* Resource subtypes. */
#define RA_GP_RINGS     0x01u
#define UDMAP_RX_CHAN   0x0au
#define UDMAP_TX_CHAN   0x0du

/* Realtime registers: one 4 KiB page per ring and per channel. */
#define RING_RT(n)      (0x3c000000u + (n) * 0x1000u)
#define RING_RT_DB      0x10u
#define RING_RT_OCC     0x18u
#define TCHAN_RT(n)     (0x35000000u + (n) * 0x1000u)
#define RCHAN_RT(n)     (0x34000000u + (n) * 0x1000u)
#define CHAN_RT_CTL     0x0u
#define CHAN_RT_CTL_EN  BIT(31)

/* TI-SCI values the Zephyr header leaves out (TI's ti_sci_protocol.h). */
#define RING_ALL_NO_ORDER    0x1fu
#define RING_MODE_RING       0u
#define RING_ELSIZE_8        1u
#define CH_CHAN_TYPE_VALID   BIT(2)
#define CH_FETCH_SIZE_VALID  BIT(3)
#define CH_CQ_QNUM_VALID     BIT(4)
#define CHAN_TYPE_BCOPY_PBRR 12u

/* PSI-L thread IDs of main UDMAP channel n. */
#define PSIL_SRC(n) (0x1000u + (n))
#define PSIL_DST(n) (0x9000u + (n))

#define RING_ELEMS 16u

/* CPPI5 TR descriptor: a 64-byte header record, one 64-byte TR, and its response. */
struct tr15 {
	uint32_t flags;
	uint16_t icnt0, icnt1;
	uint64_t addr;
	int32_t dim1;
	uint16_t icnt2, icnt3;
	int32_t dim2, dim3;
	uint32_t reserved;
	int32_t ddim1;
	uint64_t daddr;
	int32_t ddim2, ddim3;
	uint16_t dicnt0, dicnt1, dicnt2, dicnt3;
} __packed __aligned(64);

#define MAX_TRS 2u

struct trdesc {
	uint32_t info0, info1, info2, tag;
	uint8_t pad[48];
	struct tr15 tr[MAX_TRS];
	uint32_t response[MAX_TRS];
} __aligned(64);

BUILD_ASSERT(sizeof(struct tr15) == 64, "a type-15 TR is 64 bytes");

#define INFO0_TYPE_TR       (3u << 30)
#define INFO1_RECSIZE_64B   (2u << 24)
#define INFO1_PKTID(id)     ((uint32_t)(id) << 14)
#define INFO1_FLOWID_NONE   0x3fffu
#define TR_TYPE15           15u
#define TR_WAIT             BIT(5)
#define TR_CSF_SUPR_EVT     (BIT(2) << 24)
#define TR_CSF_EOP          (BIT(7) << 24)

/* The non-cacheable slot: descriptor, the two rings, and a doorbell's word. */
struct dma_mem {
	struct trdesc desc;
	uint64_t tx_ring[RING_ELEMS];
	uint64_t cq_ring[RING_ELEMS];
	uint32_t bell;
};

#define DMA_MEM_NODE DT_NODELABEL(ai64_dma_mem)
BUILD_ASSERT(DT_REG_SIZE(DMA_MEM_NODE) >= sizeof(struct dma_mem), "the DMA slot holds it");
static volatile struct dma_mem *const mem = (volatile struct dma_mem *)DT_REG_ADDR(DMA_MEM_NODE);

static const struct device *const dmsc = DEVICE_DT_GET(DT_NODELABEL(dmsc));
static K_MUTEX_DEFINE(lock);
static uint16_t chan, cq_ring;
static uint32_t tx_widx, cq_ridx;
static bool ready;

static int setup(void)
{
	struct tisci_msg_rm_udmap_tx_ch_cfg tx = {0};
	struct tisci_msg_rm_udmap_rx_ch_cfg rx = {0};
	uint16_t t0 = 0, tn = 0, r0 = 0, rn = 0, g0 = 0, gn = 0;
	int ret;

	ret = tisci_cmd_get_resource_range(dmsc, UDMAP_ID, UDMAP_TX_CHAN, &t0, &tn);
	ret = ret ? ret : tisci_cmd_get_resource_range(dmsc, UDMAP_ID, UDMAP_RX_CHAN, &r0, &rn);
	ret = ret ? ret : tisci_cmd_get_resource_range(dmsc, RINGACC_ID, RA_GP_RINGS, &g0, &gn);
	if (ret) {
		LOG_ERR("dma: no UDMA resources for this host (%d)", ret);
		return ret;
	}
	/* A pair needs the same number from both ranges. */
	if (r0 > t0 || r0 + rn <= t0 || tn == 0 || gn == 0) {
		LOG_ERR("dma: tx %u+%u and rx %u+%u share no channel", t0, tn, r0, rn);
		return -ENODEV;
	}
	chan = t0;
	cq_ring = g0;

	/*
	 * A pairing the last run left: undone (a NACK just means there was
	 * none). Its channel is off already (ai64_dma_stop() at shutdown).
	 * The channel's realtime registers are not touched before TI-SCI has
	 * configured it for this host: an R5F write there before that is
	 * refused, and the abort it leaves pending keeps the core out of WFI,
	 * so the kernel cannot stop it ("can't stop rproc: -110").
	 */
	(void)tisci_cmd_rm_psil_unpair(dmsc, NAVSS_ID, PSIL_SRC(chan), PSIL_DST(chan));

	memset((void *)mem, 0, sizeof(*mem));
	ret = tisci_cmd_ring_config(dmsc, RING_ALL_NO_ORDER, RINGACC_ID, chan,
				    (uint32_t)(uintptr_t)mem->tx_ring, 0, RING_ELEMS, RING_MODE_RING,
				    RING_ELSIZE_8, 0);
	ret = ret ? ret : tisci_cmd_ring_config(dmsc, RING_ALL_NO_ORDER, RINGACC_ID, cq_ring,
						(uint32_t)(uintptr_t)mem->cq_ring, 0, RING_ELEMS,
						RING_MODE_RING, RING_ELSIZE_8, 0);
	if (ret) {
		LOG_ERR("dma: ring config failed (%d)", ret);
		return ret;
	}

	tx.valid_params = CH_CHAN_TYPE_VALID | CH_FETCH_SIZE_VALID | CH_CQ_QNUM_VALID;
	tx.nav_id = UDMAP_ID;
	tx.index = chan;
	tx.tx_chan_type = CHAN_TYPE_BCOPY_PBRR;
	tx.tx_fetch_size = 16u >> 2; /* the descriptor's first 16 bytes, in words */
	tx.txcq_qnum = cq_ring;
	ret = tisci_cmd_rm_udmap_tx_ch_cfg(dmsc, &tx);
	if (ret) {
		LOG_ERR("dma: tx channel %u config failed (%d)", chan, ret);
		return ret;
	}
	rx.valid_params = CH_CHAN_TYPE_VALID | CH_FETCH_SIZE_VALID | CH_CQ_QNUM_VALID;
	rx.nav_id = UDMAP_ID;
	rx.index = chan;
	rx.rx_chan_type = CHAN_TYPE_BCOPY_PBRR;
	rx.rx_fetch_size = 16u >> 2;
	rx.rxcq_qnum = cq_ring;
	ret = tisci_cmd_rm_udmap_rx_ch_cfg(dmsc, &rx);
	if (ret) {
		LOG_ERR("dma: rx channel %u config failed (%d)", chan, ret);
		return ret;
	}
	ret = tisci_cmd_rm_psil_pair(dmsc, NAVSS_ID, PSIL_SRC(chan), PSIL_DST(chan));
	if (ret) {
		LOG_ERR("dma: PSI-L pair %#x -> %#x failed (%d)", PSIL_SRC(chan), PSIL_DST(chan), ret);
		return ret;
	}
	sys_write32(CHAN_RT_CTL_EN, RCHAN_RT(chan) + CHAN_RT_CTL);
	sys_write32(CHAN_RT_CTL_EN, TCHAN_RT(chan) + CHAN_RT_CTL);
	tx_widx = 0;
	cq_ridx = 0;
	LOG_INF("dma: UDMA channel %u, transmit ring %u, completion ring %u", chan, chan, cq_ring);
	return 0;
}

void ai64_dma_stop(void)
{
	if (!ready) {
		return;
	}
	sys_write32(0, TCHAN_RT(chan) + CHAN_RT_CTL);
	sys_write32(0, RCHAN_RT(chan) + CHAN_RT_CTL);
	ready = false;
}

static void tr_fill(volatile struct tr15 *tr, uint64_t dst, uint64_t src, uint32_t len,
		    bool last)
{
	tr->flags = TR_TYPE15 | TR_WAIT | TR_CSF_SUPR_EVT | (last ? TR_CSF_EOP : 0u);
	tr->addr = src;
	tr->icnt0 = len;
	tr->icnt1 = 1;
	tr->icnt2 = 1;
	tr->icnt3 = 1;
	tr->dim1 = len;
	tr->daddr = dst;
	tr->dicnt0 = len;
	tr->dicnt1 = 1;
	tr->dicnt2 = 1;
	tr->dicnt3 = 1;
	tr->ddim1 = len;
}

/* One descriptor: the copy, and with bell the doorbell word after it. */
static int run(uint64_t dst, uint64_t src, uint32_t len, const uint64_t *bell, uint32_t value,
	       uint32_t *ns)
{
	volatile struct trdesc *d = &mem->desc;
	uint32_t ntr = bell ? 2u : 1u;
	uint32_t t0, waited;
	uint64_t got;
	int ret = 0;

	if (len == 0 || len > 0xffffu) {
		return -EINVAL;
	}
	k_mutex_lock(&lock, K_FOREVER);
	if (!ready) {
		ret = setup();
		ready = ret == 0;
	}
	if (ret) {
		goto out;
	}

	memset((void *)d, 0, sizeof(*d));
	d->info0 = INFO0_TYPE_TR | (ntr - 1u); /* the last TR's index */
	d->info1 = INFO1_RECSIZE_64B | INFO1_PKTID(0) | INFO1_FLOWID_NONE;
	d->info2 = cq_ring; /* return ring */
	tr_fill(&d->tr[0], dst, src, len, ntr == 1u);
	if (bell) {
		mem->bell = value;
		tr_fill(&d->tr[1], *bell, (uint64_t)(uintptr_t)&mem->bell, sizeof(uint32_t), true);
	}
	barrier_dmem_fence_full();

	t0 = sys_read32(AI64_GTC_ADDR + 8u);
	mem->tx_ring[tx_widx] = (uint64_t)(uintptr_t)d;
	tx_widx = (tx_widx + 1u) % RING_ELEMS;
	barrier_dmem_fence_full();
	sys_write32(1, RING_RT(chan) + RING_RT_DB);

	/* The descriptor comes back on the completion ring. */
	while ((sys_read32(RING_RT(cq_ring) + RING_RT_OCC) & 0x1fffffu) == 0) {
		waited = sys_read32(AI64_GTC_ADDR + 8u) - t0;
		if (waited > AI64_GTC_HZ / 10u) {
			LOG_ERR("dma: no completion in 100 ms");
			ready = false;
			ret = -ETIMEDOUT;
			goto out;
		}
	}
	waited = sys_read32(AI64_GTC_ADDR + 8u) - t0;
	got = mem->cq_ring[cq_ridx];
	cq_ridx = (cq_ridx + 1u) % RING_ELEMS;
	sys_write32((uint32_t)-1, RING_RT(cq_ring) + RING_RT_DB);
	if (got != (uint64_t)(uintptr_t)d) {
		LOG_ERR("dma: completion %#llx is not the descriptor", (unsigned long long)got);
		ret = -EIO;
		goto out;
	}
	if (ns) {
		*ns = waited * (1000000000u / AI64_GTC_HZ);
	}
out:
	k_mutex_unlock(&lock);
	return ret;
}

int ai64_dma_copy(uint64_t dst, uint64_t src, uint32_t len, uint32_t *ns)
{
	return run(dst, src, len, NULL, 0, ns);
}

int ai64_dma_copy_bell(uint64_t dst, uint64_t src, uint32_t len, uint64_t bell, uint32_t value,
		       uint32_t *ns)
{
	return run(dst, src, len, &bell, value, ns);
}
