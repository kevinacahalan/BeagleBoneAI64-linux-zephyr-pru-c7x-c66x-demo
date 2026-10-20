/* SPDX-License-Identifier: MIT */
/*
 * Zephyr on a BeagleBone AI-64 main-domain R5F core.
 *
 * Linux calls Zephyr functions over eRPC on RPMsg (channel "rpmsg-raw",
 * address 1024). On R5F0_0, armPulses and sendXy publish a command in the
 * DDR mailbox at AI64_SHM_ADDR; RTU0 polls that mailbox and hands the command
 * on to Tx_PRU0 or PRU0. spiWrite writes SPI7 on header P9. The other cores
 * answer say and report that they have no mailbox or SPI.
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/ipm.h>
#include <zephyr/drivers/spi.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/barrier.h>
#include <zephyr/cache.h>
#include <zephyr/sys/sys_io.h>
#include <zephyr/irq.h>
#include <zephyr/logging/log.h>

#include <errno.h>

#include <openamp/open_amp.h>
#include <metal/sys.h>
#include <metal/io.h>
#include <resource_table.h>
#include <addr_translation.h>

#include "ai64_shm.h"
#include "ai64_erpc.h"
#include "ai64_tisci.h"

LOG_MODULE_REGISTER(ai64_r5, LOG_LEVEL_INF);

#define RPMSG_CHAN_NAME "rpmsg-raw"
#define RPMSG_CHAN_ADDR 1024

#if CONFIG_IPM_MAX_DATA_SIZE > 0
#define IPM_SEND(dev, w, id, d, s) ipm_send(dev, w, id, d, s)
#else
#define IPM_SEND(dev, w, id, d, s) ipm_send(dev, w, id, NULL, 0)
#endif

#define SHM_NODE       DT_CHOSEN(zephyr_ipc_shm)
#define SHM_START_ADDR DT_REG_ADDR(SHM_NODE)
#define SHM_SIZE       DT_REG_SIZE(SHM_NODE)

#if !DT_HAS_CHOSEN(zephyr_ipc_shm)
#error "R5F board must choose zephyr,ipc_shm"
#endif

static const struct device *const ipm_handle = DEVICE_DT_GET(DT_CHOSEN(zephyr_ipc));

static metal_phys_addr_t shm_physmap = SHM_START_ADDR;
static metal_phys_addr_t rsc_tab_physmap;
static struct metal_io_region shm_io_data;
static struct metal_io_region rsc_io_data;
static struct metal_io_region *shm_io = &shm_io_data;
static struct metal_io_region *rsc_io = &rsc_io_data;
static struct rpmsg_virtio_device rvdev;
static struct rpmsg_device *rpdev;
static struct rpmsg_endpoint ept;
static void *rsc_table;

static K_SEM_DEFINE(mb_sem, 0, 1);

/*
 * Linux kicks this core through a mailbox FIFO. On J721E that mailbox
 * interrupt only reaches the R5F's VIM once the device manager routes it
 * through the NAVSS interrupt router, which ai64_route_mbox_irq() asks for
 * at boot. If that fails, the RPMsg thread polls the FIFO once a
 * millisecond instead and pops what it finds.
 */
#define IPC_NODE      DT_CHOSEN(zephyr_ipc)
#define MBOX_NODE     DT_PHANDLE_BY_NAME(IPC_NODE, mboxes, rx)
#define MBOX_BASE     DT_REG_ADDR(MBOX_NODE)
#define MBOX_RX_FIFO  DT_PHA_BY_NAME(IPC_NODE, mboxes, rx, channel)
#define MBOX_TX_FIFO  DT_PHA_BY_NAME(IPC_NODE, mboxes, tx, channel)
#define MBOX_MESSAGE(m)   (MBOX_BASE + 0x40u + 4u * (m))
#define MBOX_MSGSTATUS(m) (MBOX_BASE + 0xc0u + 4u * (m))

/*
 * TI's 6.18 kernel (the Yocto image) stops an R5F by sending RP_MBOX_SHUTDOWN
 * and waiting for the ACK and for the core to sit in WFI. The BeagleBoard
 * 6.18 Debian kernel sends nothing and just halts the core.
 */
#define RP_MBOX_SHUTDOWN     0xFFFFFF14u
#define RP_MBOX_SHUTDOWN_ACK 0xFFFFFF15u

static bool mbox_irq_routed;
static atomic_t shutdown_requested;

static FUNC_NORETURN void park_for_shutdown(void)
{
	unsigned int i;

	sys_write32(RP_MBOX_SHUTDOWN_ACK, MBOX_MESSAGE(MBOX_TX_FIFO));
	(void)arch_irq_lock();
	/* A pending interrupt, even a masked one, wakes WFI. Mask them at the VIM. */
	for (i = 0; i < CONFIG_NUM_IRQS; i++) {
		irq_disable(i);
	}
	for (;;) {
		__asm__ volatile("dsb; wfi" ::: "memory");
	}
}

/* Pop every queued kick. Returns how many there were. */
static uint32_t mbox_rx_drain(void)
{
	uint32_t n = 0;

	while (sys_read32(MBOX_MSGSTATUS(MBOX_RX_FIFO)) & 0x3fu) {
		if (sys_read32(MBOX_MESSAGE(MBOX_RX_FIFO)) == RP_MBOX_SHUTDOWN) {
			LOG_INF("shutdown request from Linux");
			park_for_shutdown();
		}
		n++;
	}
	return n;
}

static uint32_t shm_seq;

#if AI64_HAS_SHM
static volatile struct ai64_shm *const pru_shm =
	(volatile struct ai64_shm *)AI64_SHM_ADDR;
#endif

void ai64_shm_publish(const char *cmd)
{
#if !AI64_HAS_SHM
	ARG_UNUSED(cmd);
#else
	size_t n = strlen(cmd);

	if (n > sizeof(pru_shm->cmd) - 1) {
		n = sizeof(pru_shm->cmd) - 1;
	}
	memcpy((void *)pru_shm->cmd, cmd, n);
	((volatile char *)pru_shm->cmd)[n] = '\0';
	pru_shm->magic = AI64_SHM_MAGIC;
	barrier_dmem_fence_full();
	pru_shm->seq = ++shm_seq;
	barrier_dmem_fence_full();
	sys_cache_data_flush_range((void *)pru_shm, sizeof(*pru_shm));
	LOG_INF("shm seq %u cmd %s", shm_seq, cmd);
#endif
}

static void platform_ipm_callback(const struct device *dev, void *context,
				  uint32_t id, volatile void *data)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(context);
	ARG_UNUSED(id);

	/* The mailbox driver hands over the message it popped. */
	if (data && *(volatile uint32_t *)data == RP_MBOX_SHUTDOWN) {
		atomic_set(&shutdown_requested, 1);
	}
	k_sem_give(&mb_sem);
}

static int mailbox_notify(void *priv, uint32_t id)
{
	ARG_UNUSED(priv);
	IPM_SEND(ipm_handle, 0, id, &id, 4);
	return 0;
}

static int platform_init(void)
{
	struct metal_init_params metal_params = METAL_INIT_DEFAULTS;
	int rsc_size;
	int status;

	status = metal_init(&metal_params);
	if (status) {
		LOG_ERR("metal_init failed: %d", status);
		return -1;
	}

	metal_io_init(shm_io, (void *)SHM_START_ADDR, &shm_physmap, SHM_SIZE,
		      -1, 0, addr_translation_get_ops(shm_physmap));

	rsc_table_get(&rsc_table, &rsc_size);
	rsc_tab_physmap = (uintptr_t)rsc_table;
	metal_io_init(rsc_io, rsc_table, &rsc_tab_physmap, rsc_size, -1, 0, NULL);

	mbox_irq_routed = ai64_route_mbox_irq() == 0;

	if (!device_is_ready(ipm_handle)) {
		LOG_ERR("IPM device is not ready");
		return -1;
	}
	ipm_register_callback(ipm_handle, platform_ipm_callback, NULL);
	status = ipm_set_enabled(ipm_handle, 1);
	if (status) {
		LOG_ERR("ipm_set_enabled failed");
		return -1;
	}
	return 0;
}

static struct rpmsg_device *platform_create_rpmsg_vdev(void)
{
	struct fw_rsc_vdev_vring *vring_rsc;
	struct virtio_device *vdev;
	int ret;

	vdev = rproc_virtio_create_vdev(VIRTIO_DEV_DEVICE, VDEV_ID,
					rsc_table_to_vdev(rsc_table),
					rsc_io, NULL, mailbox_notify, NULL);
	if (!vdev) {
		LOG_ERR("failed to create vdev");
		return NULL;
	}

	rproc_virtio_wait_remote_ready(vdev);

	vring_rsc = rsc_table_get_vring0(rsc_table);
	ret = rproc_virtio_init_vring(vdev, 0, vring_rsc->notifyid,
				      (void *)vring_rsc->da, rsc_io,
				      vring_rsc->num, vring_rsc->align);
	if (ret) {
		LOG_ERR("failed to init vring 0");
		goto failed;
	}

	vring_rsc = rsc_table_get_vring1(rsc_table);
	ret = rproc_virtio_init_vring(vdev, 1, vring_rsc->notifyid,
				      (void *)vring_rsc->da, rsc_io,
				      vring_rsc->num, vring_rsc->align);
	if (ret) {
		LOG_ERR("failed to init vring 1");
		goto failed;
	}

	ret = rpmsg_init_vdev(&rvdev, vdev, NULL, shm_io, NULL);
	if (ret) {
		LOG_ERR("failed rpmsg_init_vdev");
		goto failed;
	}
	return rpmsg_virtio_get_rpmsg_device(&rvdev);

failed:
	rproc_virtio_remove_vdev(vdev);
	return NULL;
}

static int rx_cb(struct rpmsg_endpoint *ept_dev, void *data, size_t len,
		 uint32_t src, void *priv)
{
	ARG_UNUSED(ept_dev);
	ARG_UNUSED(src);
	ARG_UNUSED(priv);

	ai64_erpc_push(data, len);
	return RPMSG_SUCCESS;
}

int ai64_rpmsg_tx(const void *data, size_t len)
{
	int ret = rpmsg_send(&ept, (void *)data, len);

	return ret < 0 ? -1 : 0;
}

static void app_thread(void *a, void *b, void *c)
{
	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);

	ai64_erpc_server();
}

static void rpmsg_thread(void *a, void *b, void *c)
{
	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);

	if (platform_init()) {
		return;
	}
	rpdev = platform_create_rpmsg_vdev();
	if (!rpdev) {
		return;
	}

	if (rpmsg_create_ept(&ept, rpdev, RPMSG_CHAN_NAME, RPMSG_CHAN_ADDR,
			     RPMSG_ADDR_ANY, rx_cb, NULL)) {
		LOG_ERR("rpmsg_create_ept failed");
		return;
	}
	LOG_INF("rpmsg channel %s addr %d is up", RPMSG_CHAN_NAME, RPMSG_CHAN_ADDR);

	while (1) {
		int woke = k_sem_take(&mb_sem, mbox_irq_routed ? K_FOREVER : K_MSEC(1)) == 0;

		if (atomic_get(&shutdown_requested)) {
			LOG_INF("shutdown request from Linux");
			park_for_shutdown();
		}
		if (mbox_rx_drain() || woke) {
			rproc_virtio_notified(rvdev.vdev, VRING1_ID);
		}
	}
}

K_THREAD_DEFINE(rpmsg_tid, 4096, rpmsg_thread, NULL, NULL, NULL, K_PRIO_COOP(8), 0, 0);
K_THREAD_DEFINE(app_tid, 8192, app_thread, NULL, NULL, NULL, K_PRIO_COOP(7), 0, 0);

int ai64_spi7_write(const void *buf, size_t len)
{
#if !AI64_HAS_SPI7
	ARG_UNUSED(buf);
	ARG_UNUSED(len);
	return -ENOTSUP;
#else
	static const struct spi_config cfg = {
		.frequency = 1000000,
		.operation = SPI_OP_MODE_CONTROLLER | SPI_WORD_SET(8) | SPI_TRANSFER_MSB,
		.peripheral = 0,
	};
	const struct device *spi = DEVICE_DT_GET(DT_NODELABEL(main_spi7));
	const struct spi_buf txb = {
		.buf = (void *)buf,
		.len = len,
	};
	const struct spi_buf_set tx = {
		.buffers = &txb,
		.count = 1,
	};

	if (!buf || !len) {
		return -EINVAL;
	}
	if (!device_is_ready(spi)) {
		LOG_ERR("spi7 not ready");
		return -ENODEV;
	}
	return spi_write(spi, &cfg, &tx);
#endif
}

static void ai64_spi_hello(void)
{
	static const uint8_t hello[] = {'h', 'e', 'l', 'l', 'o'};
	int ret = ai64_spi7_write(hello, sizeof(hello));
	char msg[56];

	if (ret == -ENODEV) {
		ai64_shm_publish("spi hello not ready");
		return;
	}
	if (ret) {
		LOG_ERR("spi7 hello failed: %d", ret);
		snprintk(msg, sizeof(msg), "spi hello fail %d", ret);
		ai64_shm_publish(msg);
		return;
	}
	LOG_INF("spi7 hello wrote %u bytes", (unsigned int)sizeof(hello));
	ai64_shm_publish("spi hello ok");
}

int main(void)
{
	if (AI64_HAS_SHM) {
		ai64_shm_publish("hello from zephyr");
	}
	LOG_INF("ai64 " AI64_CORE " hello, shm seq %u", shm_seq);
	if (AI64_HAS_SPI7) {
		ai64_spi_hello();
	}
	return 0;
}
