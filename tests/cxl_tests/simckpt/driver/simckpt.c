// SPDX-License-Identifier: GPL-2.0
/*
 * simckpt.ko - kernel driver for the gem5 SimCkptDevice PCI device.
 *
 * Reference implementation. The device is a bare BAR0 register file plus
 * DMA-addressable submission/completion rings in host memory; the driver only
 * needs to (1) map BAR0 into user space, (2) pin user buffers so the device
 * can DMA to/from them, and (3) forward doorbell/submit/wait operations to the
 * BAR registers. The actual chunk scheduling lives in user space (ckptd /
 * libckpt).
 *
 * Build note: the guest kernel is a custom 6.12.0+ image whose headers are not
 * shipped in the disk image (only 4.15 headers are present), so this module is
 * provided as a reference and cannot be built/loaded in the current image
 * without the matching kernel source. The same ABI is exercised end-to-end by
 * the user-space backend of libckpt (via /dev/mem), see libckpt.c.
 */
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/cdev.h>
#include <linux/fs.h>
#include <linux/device.h>
#include <linux/mm.h>
#include <linux/uaccess.h>
#include <linux/slab.h>
#include <linux/pagemap.h>
#include <linux/highmem.h>
#include <linux/dma-mapping.h>
#include <linux/scatterlist.h>
#include <asm/io.h>

#include "simckpt_uapi.h"

#define DRV_NAME "simckpt"
#define PCI_VENDOR_ID_SIMCKPT 0x8086
#define PCI_DEVICE_ID_SIMCKPT 0x9090

static dev_t simckpt_devt;
static struct class *simckpt_class;
static int simckpt_major;

struct simckpt_dev {
    struct pci_dev *pdev;
    void __iomem *bar0;
    resource_size_t bar0_phys;
    resource_size_t bar0_len;
    struct cdev cdev;
};

/* A registered (pinned + DMA-mapped) user buffer. */
struct simckpt_reg {
    struct list_head node;
    void *buf;              /* user virtual address                     */
    size_t len;
    unsigned int nr_pages;
    struct page **pages;
    struct sg_table sgt;
    dma_addr_t dma_addr;    /* first-segment DMA (physical) address      */
};

/* --- BAR register accessors ------------------------------------------- */
static inline void bar_wq(struct simckpt_dev *dev, u64 off, u64 val)
{
    writeq(val, dev->bar0 + off);
}

static inline u64 bar_rq(struct simckpt_dev *dev, u64 off)
{
    return readq(dev->bar0 + off);
}

/* --- file operations --------------------------------------------------- */
static int simckpt_open(struct inode *inode, struct file *filp)
{
    struct simckpt_dev *dev =
        container_of(inode->i_cdev, struct simckpt_dev, cdev);
    filp->private_data = dev;
    return 0;
}

static int simckpt_mmap(struct file *filp, struct vm_area_struct *vma)
{
    struct simckpt_dev *dev = filp->private_data;
    unsigned long vsize = vma->vm_end - vma->vm_start;
    unsigned long pfn = dev->bar0_phys >> PAGE_SHIFT;

    if (vsize > dev->bar0_len)
        return -EINVAL;

    vma->vm_page_prot = pgprot_noncached(vma->vm_page_prot);
    return remap_pfn_range(vma, vma->vm_start, pfn, vsize,
                           vma->vm_page_prot);
}

/* Pin a user buffer and return its DMA address. */
static long simckpt_register(struct simckpt_dev *dev, void __user *arg)
{
    struct simckpt_register_arg a;
    struct simckpt_reg *reg;
    unsigned long start, nr_pages;
    int i, ret;

    if (copy_from_user(&a, arg, sizeof(a)))
        return -EFAULT;

    if (!a.buf || !a.len)
        return -EINVAL;

    start = (unsigned long)a.buf & PAGE_MASK;
    nr_pages = (PAGE_ALIGN((unsigned long)a.buf + a.len) - start) >> PAGE_SHIFT;

    reg = kzalloc(sizeof(*reg), GFP_KERNEL);
    if (!reg)
        return -ENOMEM;

    reg->buf = (void *)start;
    reg->len = a.len;
    reg->nr_pages = nr_pages;
    reg->pages = kcalloc(nr_pages, sizeof(struct page *), GFP_KERNEL);
    if (!reg->pages) {
        kfree(reg);
        return -ENOMEM;
    }

    ret = pin_user_pages_fast(start, nr_pages, FOLL_WRITE, reg->pages);
    if (ret != (int)nr_pages) {
        ret = ret > 0 ? -EFAULT : ret;
        goto err_unpin;
    }

    ret = sg_alloc_table_from_pages(&reg->sgt, reg->pages, nr_pages,
                                    (unsigned long)a.buf & ~PAGE_MASK,
                                    a.len, GFP_KERNEL);
    if (ret)
        goto err_unpin;

    ret = dma_map_sg(&dev->pdev->dev, reg->sgt.sgl, reg->sgt.nents,
                     DMA_BIDIRECTIONAL);
    if (ret <= 0) {
        ret = -ENOMEM;
        goto err_free_sgt;
    }

    reg->dma_addr = sg_dma_address(reg->sgt.sgl);

    a.dma_addr = reg->dma_addr;
    if (copy_to_user(arg, &a, sizeof(a))) {
        ret = -EFAULT;
        goto err_unmap;
    }

    return 0;

err_unmap:
    dma_unmap_sg(&dev->pdev->dev, reg->sgt.sgl, reg->sgt.nents,
                 DMA_BIDIRECTIONAL);
err_free_sgt:
    sg_free_table(&reg->sgt);
err_unpin:
    unpin_user_pages(reg->pages, reg->nr_pages);
    kfree(reg->pages);
    kfree(reg);
    return ret;
}

static long simckpt_submit(struct simckpt_dev *dev, void __user *arg)
{
    struct simckpt_submit_arg a;

    if (copy_from_user(&a, arg, sizeof(a)))
        return -EFAULT;
    if (!a.sq_base || !a.cq_base || !a.sq_depth || !a.cq_depth)
        return -EINVAL;

    bar_wq(dev, SIMCKPT_REG_SQ_BASE, a.sq_base);
    bar_wq(dev, SIMCKPT_REG_SQ_DEPTH, a.sq_depth);
    bar_wq(dev, SIMCKPT_REG_CQ_BASE, a.cq_base);
    bar_wq(dev, SIMCKPT_REG_CQ_DEPTH, a.cq_depth);
    bar_wq(dev, SIMCKPT_REG_INTR_EN, (a.flags & 1) ? 1 : 0);
    /* reset rings + counters before a fresh submission */
    bar_wq(dev, SIMCKPT_REG_CTRL, 0x2);
    bar_wq(dev, SIMCKPT_REG_SQ_DOORBELL, a.sq_tail);

    return 0;
}

static long simckpt_wait(struct simckpt_dev *dev, void __user *arg)
{
    u64 count, seen;
    u64 start = 0;

    if (copy_from_user(&count, arg, sizeof(count)))
        return -EFAULT;

    /* Simple busy-wait on the COMPLETED counter (interrupt + waitqueue is a
     * later optimization; ckptd samples counters for backpressure anyway). */
    for (;;) {
        seen = bar_rq(dev, SIMCKPT_REG_COMPLETED);
        if (seen >= count)
            break;
        cpu_relax();
    }

    (void)start;
    return 0;
}

static long simckpt_get_counters(struct simckpt_dev *dev, void __user *arg)
{
    struct simckpt_counters c = {
        .completed   = bar_rq(dev, SIMCKPT_REG_COMPLETED),
        .intr_posted = bar_rq(dev, SIMCKPT_REG_INTR_POSTED),
        .errors      = bar_rq(dev, SIMCKPT_REG_ERRORS),
        .sq_head     = bar_rq(dev, SIMCKPT_REG_SQ_HEAD),
        .cq_tail     = bar_rq(dev, SIMCKPT_REG_CQ_TAIL),
    };

    if (copy_to_user(arg, &c, sizeof(c)))
        return -EFAULT;
    return 0;
}

static long simckpt_ioctl(struct file *filp, unsigned int cmd,
                          unsigned long arg)
{
    struct simckpt_dev *dev = filp->private_data;

    switch (cmd) {
    case SIMCKPT_IOC_REGISTER:
        return simckpt_register(dev, (void __user *)arg);
    case SIMCKPT_IOC_UNREGISTER:
        return -ENOSYS; /* reference: full impl keeps a registry to unpin */
    case SIMCKPT_IOC_SUBMIT:
        return simckpt_submit(dev, (void __user *)arg);
    case SIMCKPT_IOC_WAIT:
        return simckpt_wait(dev, (void __user *)arg);
    case SIMCKPT_IOC_GET_COUNTERS:
        return simckpt_get_counters(dev, (void __user *)arg);
    default:
        return -ENOTTY;
    }
}

static const struct file_operations simckpt_fops = {
    .owner          = THIS_MODULE,
    .open           = simckpt_open,
    .mmap           = simckpt_mmap,
    .unlocked_ioctl = simckpt_ioctl,
};

/* --- PCI driver -------------------------------------------------------- */
static int simckpt_probe(struct pci_dev *pdev,
                         const struct pci_device_id *id)
{
    struct simckpt_dev *dev;
    int ret, i;

    ret = pci_enable_device_mem(pdev);
    if (ret)
        return ret;

    ret = dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(64));
    if (ret) {
        pci_disable_device(pdev);
        return ret;
    }

    dev = kzalloc(sizeof(*dev), GFP_KERNEL);
    if (!dev) {
        pci_disable_device(pdev);
        return -ENOMEM;
    }
    dev->pdev = pdev;
    pci_set_drvdata(pdev, dev);

    dev->bar0_phys = pci_resource_start(pdev, 0);
    dev->bar0_len = pci_resource_len(pdev, 0);
    dev->bar0 = pci_iomap(pdev, 0, 0);
    if (!dev->bar0) {
        ret = -ENOMEM;
        goto err_disable;
    }

    cdev_init(&dev->cdev, &simckpt_fops);
    dev->cdev.owner = THIS_MODULE;
    ret = cdev_add(&dev->cdev, MKDEV(simckpt_major, 0), 1);
    if (ret)
        goto err_iounmap;

    device_create(simckpt_class, &pdev->dev,
                  MKDEV(simckpt_major, 0), dev, "simckpt0");

    dev_info(&pdev->dev, "simckpt BAR0 phys=0x%llx len=0x%llx\n",
             (unsigned long long)dev->bar0_phys,
             (unsigned long long)dev->bar0_len);

    return 0;

err_iounmap:
    pci_iounmap(pdev, dev->bar0);
err_disable:
    pci_disable_device(pdev);
    kfree(dev);
    return ret;
}

static void simckpt_remove(struct pci_dev *pdev)
{
    struct simckpt_dev *dev = pci_get_drvdata(pdev);

    device_destroy(simckpt_class, MKDEV(simckpt_major, 0));
    cdev_del(&dev->cdev);
    pci_iounmap(pdev, dev->bar0);
    pci_disable_device(pdev);
    kfree(dev);
}

static const struct pci_device_id simckpt_ids[] = {
    { PCI_DEVICE(PCI_VENDOR_ID_SIMCKPT, PCI_DEVICE_ID_SIMCKPT) },
    { 0, }
};
MODULE_DEVICE_TABLE(pci, simckpt_ids);

static struct pci_driver simckpt_driver = {
    .name     = DRV_NAME,
    .id_table = simckpt_ids,
    .probe    = simckpt_probe,
    .remove   = simckpt_remove,
};

static int __init simckpt_init(void)
{
    int ret;

    ret = alloc_chrdev_region(&simckpt_devt, 0, 1, DRV_NAME);
    if (ret)
        return ret;
    simckpt_major = MAJOR(simckpt_devt);

    simckpt_class = class_create(DRV_NAME);
    if (IS_ERR(simckpt_class)) {
        unregister_chrdev_region(simckpt_devt, 1);
        return PTR_ERR(simckpt_class);
    }

    ret = pci_register_driver(&simckpt_driver);
    if (ret) {
        class_destroy(simckpt_class);
        unregister_chrdev_region(simckpt_devt, 1);
        return ret;
    }
    return 0;
}

static void __exit simckpt_exit(void)
{
    pci_unregister_driver(&simckpt_driver);
    class_destroy(simckpt_class);
    unregister_chrdev_region(simckpt_devt, 1);
}

module_init(simckpt_init);
module_exit(simckpt_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("SimCXL");
MODULE_DESCRIPTION("SimCkptDevice checkpoint DMA engine driver");
