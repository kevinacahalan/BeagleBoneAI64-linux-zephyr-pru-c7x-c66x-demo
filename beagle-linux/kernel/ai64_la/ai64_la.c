// SPDX-License-Identifier: MIT
/*
 * /dev/ai64-la: the logic analyzer's ring (zephyr/include/ai64_la.h), for
 * ai64 la (linux-app/la_src.c) to map read-only.
 *
 * The overlay reserves the ring no-map, so Linux has no mapping of it and
 * /dev/mem maps it as Device memory: every load a bus transaction of its
 * own, about 170 MB/s. Here it maps as Normal memory instead, so loads go
 * in bursts. Offset 0 maps it cached: the reader cleans and invalidates
 * each line (DC CIVAC, which EL0 may do) before it reads it, since PRU0's
 * XFR2VBUS writes DDR behind the caches. Offset AI64_LA_RING_SIZE maps it
 * Normal non-cacheable, with nothing to invalidate.
 */
#include <linux/fs.h>
#include <linux/ioport.h>
#include <linux/miscdevice.h>
#include <linux/mm.h>
#include <linux/module.h>

/* As ai64_la.h has them. */
#define RING_ADDR 0xac000000UL
#define RING_SIZE 0x02000000UL

static int ai64_la_mmap(struct file *f, struct vm_area_struct *vma)
{
	unsigned long len = vma->vm_end - vma->vm_start;
	unsigned long off = vma->vm_pgoff << PAGE_SHIFT;
	pgprot_t prot;

	if (vma->vm_flags & (VM_WRITE | VM_EXEC))
		return -EPERM;
	if (off == 0)
		prot = vma->vm_page_prot;
	else if (off == RING_SIZE)
		prot = pgprot_writecombine(vma->vm_page_prot);
	else
		return -EINVAL;
	if (len > RING_SIZE)
		return -EINVAL;
	vm_flags_clear(vma, VM_MAYWRITE | VM_MAYEXEC);
	vm_flags_set(vma, VM_IO | VM_DONTEXPAND | VM_DONTDUMP);
	return remap_pfn_range(vma, vma->vm_start, RING_ADDR >> PAGE_SHIFT, len, prot);
}

static const struct file_operations ai64_la_fops = {
	.owner = THIS_MODULE,
	.mmap = ai64_la_mmap,
};

static struct miscdevice ai64_la_dev = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "ai64-la",
	.fops = &ai64_la_fops,
	.mode = 0400,
};

static int __init ai64_la_init(void)
{
	/* The overlay's reservation, or this would map RAM Linux uses. */
	if (region_intersects(RING_ADDR, RING_SIZE, IORESOURCE_SYSTEM_RAM, IORES_DESC_NONE) !=
	    REGION_DISJOINT) {
		pr_err("ai64_la: %#lx is System RAM: is the ai64-demo overlay applied?\n", RING_ADDR);
		return -ENODEV;
	}
	return misc_register(&ai64_la_dev);
}

static void __exit ai64_la_exit(void)
{
	misc_deregister(&ai64_la_dev);
}

module_init(ai64_la_init);
module_exit(ai64_la_exit);
MODULE_DESCRIPTION("The ai64 demo's logic analyzer ring, mapped for reading");
MODULE_LICENSE("Dual MIT/GPL");
