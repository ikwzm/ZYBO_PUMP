/*
 * pump_drv.c
 *
 * Copyright (C) 2014-2015 Ichiro Kawazome
 * All rights reserved.
 * 
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 * 
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT 
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 * 
 */
#include <linux/cdev.h>
#include <linux/clk.h>
#include <linux/dma-mapping.h>
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/ioport.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/sched.h>
#include <linux/device.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/sysctl.h>
#include <linux/types.h>
#include <linux/uaccess.h>
#include <linux/scatterlist.h>
#include <linux/pagemap.h>
#include <linux/list.h>
#include <linux/spinlock.h>
#include <linux/version.h>
#include <asm/page.h>
#include <asm/byteorder.h>

#include "pump_proc.h"

#define DRIVER_NAME        "pump"
#define DEVICE_NAME_FORMAT "pump%d"

#define PUMP_DEBUG         1
#define PUMP_SG_PACK_MAX   ((0xFFFFFFFF & PAGE_MASK) >> PAGE_SHIFT)

#define PUMP_TIMEOUT_DEF   (10*60*1000)
#define PUMP_TIMEOUT_MAX   (10*60*1000)

#if     (LINUX_VERSION_CODE >= 0x030B00)
#define USE_DEV_GROUPS      1
#else
#define USE_DEV_GROUPS      0
#endif

#if     (PUMP_DEBUG == 1)
#define PUMP_DEBUG_CHECK(this,debug) (this->debug)
#else
#define PUMP_DEBUG_CHECK(this,debug) (0)
#endif

#define PUMP_XFER_AXI_CACHE (0x03 <<  0)
#define PUMP_XFER_AXI_USER  (0x01 <<  4)
#define PUMP_XFER_AXI_SPEC  (1    <<  9)
#define PUMP_XFER_AXI_SAFE  (1    << 10)

#define PUMP_XFER_AXI_MODE  (PUMP_XFER_AXI_USER | PUMP_XFER_AXI_CACHE)
#define PUMP_LINK_AXI_MODE  (PUMP_XFER_AXI_USER | PUMP_XFER_AXI_CACHE)

/**
 *  Register read/write access routines
 */
#define regs_read(offset)       le32_to_cpu(ioread32(offset))

static struct class*  pump_sys_class     = NULL;
static dev_t          pump_device_number = 0;

/**
 * struct pump_driver_data - Device driver structure
 */
struct pump_driver_data {
    struct device*          dev;
    struct cdev             cdev;
    dev_t                   device_number;
    struct mutex            sem;
    struct resource*        core_regs_res;
    struct resource*        proc_regs_res;
    struct resource*        irq_res;
    bool                    is_open;
    int                     direction;
    void __iomem*           core_regs_addr;
    void __iomem*           proc_regs_addr;
    int                     irq;
    unsigned int            page_nums;
    struct page**           page_list;
    struct sg_table         sg_table;
    unsigned int            sg_nums;
    struct list_head        pump_buf_list;
    struct pump_proc_data   pump_proc_data;
    wait_queue_head_t       wait_queue;
    unsigned long           limit_size;
    unsigned long           timeout_msec;
    unsigned long           usec_buffer_setup;
    unsigned long           usec_buffer_release;
    unsigned long           usec_pump_run;
#if (PUMP_DEBUG == 1)
    bool                    debug_phase;
    bool                    debug_op_table;
    bool                    debug_sg_table;
    bool                    debug_interrupt;
#endif   
};

#define DEF_ATTR_SHOW(__attr_name, __format, __value) \
static ssize_t pump_show_ ## __attr_name(struct device *dev, struct device_attribute *attr, char *buf) \
{ \
    ssize_t status; \
    struct pump_driver_data* this = dev_get_drvdata(dev); \
    if (mutex_lock_interruptible(&this->sem) != 0) \
        return -ERESTARTSYS; \
    status = sprintf(buf, __format, (__value)); \
    mutex_unlock(&this->sem); \
    return status; \
}

#define DEF_ATTR_SET(__attr_name, __min, __max, __pre_action, __post_action) \
static ssize_t pump_set_ ## __attr_name(struct device *dev, struct device_attribute *attr, const char *buf, size_t size) \
{ \
    ssize_t       status; \
    unsigned long value;  \
    struct pump_driver_data* this = dev_get_drvdata(dev);              \
    if (0 != mutex_lock_interruptible(&this->sem)){return -ERESTARTSYS;}     \
    if (0 != (status = kstrtoul(buf, 10, &value))) {           goto failed;} \
    if ((value < __min) || (__max < value)) {status = -EINVAL; goto failed;} \
    if (0 != (status = __pre_action )) {                       goto failed;} \
    this->__attr_name = value;                                               \
    if (0 != (status = __post_action)) {                       goto failed;} \
    status = size;                                                           \
  failed:                                                                    \
    mutex_unlock(&this->sem);                                                \
    return status;                                                           \
}

DEF_ATTR_SHOW(direction           , "%d\n" , this->direction);
DEF_ATTR_SHOW(dma_direction       , "%s\n" , (this->direction) ? "DMA_TO_DEVICE" : "DMA_FROM_DEVICE");
DEF_ATTR_SHOW(limit_size          , "%lu\n", this->limit_size);
DEF_ATTR_SHOW(timeout_msec        , "%lu\n", this->timeout_msec);
DEF_ATTR_SHOW(usec_buffer_setup   , "%lu\n", this->usec_buffer_setup);
DEF_ATTR_SHOW(usec_buffer_release , "%lu\n", this->usec_buffer_release);
DEF_ATTR_SHOW(usec_pump_run       , "%lu\n", this->usec_pump_run);
DEF_ATTR_SET( limit_size          , 0, 0xFFFFFFFF      , 0, 0);
DEF_ATTR_SET( timeout_msec        , 0, PUMP_TIMEOUT_MAX, 0, 0);

#if (PUMP_DEBUG == 1)
DEF_ATTR_SHOW(debug_phase         , "%d\n", this->debug_phase    );
DEF_ATTR_SHOW(debug_op_table      , "%d\n", this->debug_op_table );
DEF_ATTR_SHOW(debug_sg_table      , "%d\n", this->debug_sg_table );
DEF_ATTR_SHOW(debug_interrupt     , "%d\n", this->debug_interrupt);
DEF_ATTR_SET( debug_phase         , 0, 1, 0, 0);
DEF_ATTR_SET( debug_op_table      , 0, 1, 0, 0);
DEF_ATTR_SET( debug_sg_table      , 0, 1, 0, 0);
DEF_ATTR_SET( debug_interrupt     , 0, 1, 0, 0);
#endif

static struct device_attribute pump_device_attrs[] = {
  __ATTR(direction           , 0644, pump_show_direction           , NULL),
  __ATTR(dma_direction       , 0644, pump_show_dma_direction       , NULL),
  __ATTR(limit_size          , 0644, pump_show_limit_size          , pump_set_limit_size     ),
  __ATTR(timeout_msec        , 0644, pump_show_timeout_msec        , pump_set_timeout_msec   ),
  __ATTR(usec_buffer_setup   , 0644, pump_show_usec_buffer_setup   , NULL),
  __ATTR(usec_buffer_release , 0644, pump_show_usec_buffer_release , NULL),
  __ATTR(usec_pump_run       , 0644, pump_show_usec_pump_run       , NULL),
#if (PUMP_DEBUG == 1)
  __ATTR(debug_phase         , 0644, pump_show_debug_phase         , pump_set_debug_phase    ),
  __ATTR(debug_sg_table      , 0644, pump_show_debug_sg_table      , pump_set_debug_sg_table ),
  __ATTR(debug_op_table      , 0644, pump_show_debug_op_table      , pump_set_debug_op_table ),
  __ATTR(debug_interrupt     , 0644, pump_show_debug_interrupt     , pump_set_debug_interrupt),
#endif
  __ATTR_NULL,
};

#if (USE_DEV_GROUPS == 1)

static struct attribute *pump_attrs[] = {
  &(pump_device_attrs[ 0].attr),
  &(pump_device_attrs[ 1].attr),
  &(pump_device_attrs[ 2].attr),
  &(pump_device_attrs[ 3].attr),
  &(pump_device_attrs[ 4].attr),
  &(pump_device_attrs[ 5].attr),
  &(pump_device_attrs[ 6].attr),
#if (PUMP_DEBUG == 1)
  &(pump_device_attrs[ 7].attr),
  &(pump_device_attrs[ 8].attr),
  &(pump_device_attrs[ 9].attr),
  &(pump_device_attrs[10].attr),
#endif
  NULL
};
static struct attribute_group  pump_attr_group = {
  .attrs = pump_attrs
};
static const struct attribute_group* pump_attr_groups[] = {
  &pump_attr_group,
  NULL
};

#define SET_SYS_CLASS_ATTRIBUTES(sys_class) {(sys_class)->dev_groups = pump_attr_groups; }
#else
#define SET_SYS_CLASS_ATTRIBUTES(sys_class) {(sys_class)->dev_attrs  = pump_device_attrs;}
#endif

/**
 * pump_alloc_pages_from_user_buffer()
 */
static int  pump_alloc_pages_from_user_buffer(struct pump_driver_data* this, char __user* buff, size_t count)
{
    int           result        = 0;
    int           dma_direction = (this->direction) ? DMA_TO_DEVICE : DMA_FROM_DEVICE;
    int           page_write    = (dma_direction == DMA_FROM_DEVICE) ? 1 : 0;
    unsigned long start_addr    = (unsigned long)(buff);
    unsigned long page_start    = ((start_addr        ) & PAGE_MASK);
    unsigned long page_first    = ((start_addr        ) & PAGE_MASK) >> PAGE_SHIFT;
    unsigned long page_last     = ((start_addr+count-1) & PAGE_MASK) >> PAGE_SHIFT;
    unsigned int  n_pages       = page_last - page_first + 1;

    if (PUMP_DEBUG_CHECK(this,debug_phase))
        dev_info(this->dev, "pump_alloc_pages_from_user_buffer(buff=%pK,count=%d)\n", buff, count);

    this->page_list = kzalloc(n_pages * sizeof(struct page*), GFP_KERNEL);
    if (IS_ERR_OR_NULL(this->page_list)) {
        result = PTR_ERR(this->page_list);
        this->page_list = NULL;
        goto failed;
    }
    
    down_read(&current->mm->mmap_sem);
    result = get_user_pages(
        current         ,       /* struct task_struct    */
        current->mm     ,       /* struct mm_struct      */
        page_start      ,       /* buffer page start     */
        n_pages         ,       /* buffer page number    */
        page_write      ,       /* page write mapping    */
        0               ,       /* page force mapping    */
        this->page_list ,       /* struct page **pages   */
        NULL                    /* struct vm_area_struct */
    );
    up_read(&current->mm->mmap_sem);
    
    if (result != n_pages) {
        this->page_nums = (result > 0) ? result : 0;
        result = (result < 0) ? result : -EINVAL;
        goto failed;
    }
    else {
        this->page_nums = result;
    }
    
    if (PUMP_DEBUG_CHECK(this,debug_phase))
        dev_info(this->dev, "pump_alloc_pages_from_user_buffer() => success\n");
    return 0;

 failed:
    if (PUMP_DEBUG_CHECK(this,debug_phase))
        dev_info(this->dev, "pump_alloc_pages_from_user_buffer() => error(%d)\n", result);
    return result;
}

/**
 * pump_free_sg_table()
 */
static void pump_free_sg_table(struct pump_driver_data* this)
{
    if (PUMP_DEBUG_CHECK(this,debug_phase))
        dev_info(this->dev, "pump_free_sg_table()\n");

#ifdef ARCH_HAS_SG_CHAIN
    sg_free_table(&this->sg_table);
#else
    if (this->sg_table.sgl != NULL) {
        kfree(this->sg_table.sgl);
        this->sg_table.sgl        = NULL;
        this->sg_table.nents      = 0;
        this->sg_table.orig_nents = 0;
    }
#endif
}

/**
 * pump_alloc_sg_table_from_pages()
 */
static int  pump_alloc_sg_table_from_pages(struct pump_driver_data* this, char __user* buff, size_t count)
{
    int           result      = 0;
    unsigned long page_offset = (((unsigned long)(buff)) & ~PAGE_MASK);
    size_t        remain_size = count;
    int           dma_direction = (this->direction) ? DMA_TO_DEVICE : DMA_FROM_DEVICE;

    if (PUMP_DEBUG_CHECK(this,debug_phase))
        dev_info(this->dev, "pump_alloc_sg_table_from_pages(buff=%pK,count=%d)\n", buff, count);

    if (NULL == this->page_list) {
        this->sg_nums = 0;
        goto success;
    }

#ifdef ARCH_HAS_SG_CHAIN
    if (PUMP_DEBUG_CHECK(this,debug_phase))
        dev_info(this->dev, "sg_alloc_table_from_pages()\n")
    result = sg_alloc_table_from_pages(
        &this->sg_table,     /* struct sg_table *sgt */
        this->page_list,     /* struct page **pages  */
        this->page_nums,     /* unsigned int n_pages */
        page_offset    ,     /* unsigned long offset */
        remain_size    ,     /* unsigned long size   */
        GFP_KERNEL           /* gfp_t gfp_mask       */
    );
    if (result) {
        this->sg_nums = 0;
        goto failed;
    }
#else
    {
        struct scatterlist* sg;
        int                 sg_count;
        unsigned int        sg_nums;
        unsigned int        curr_page;
        unsigned int        next_page;
#if (PUMP_SG_PACK_MAX > 0)
        unsigned int        packed_page_nums = 1;
        sg_nums = 1;
        for (next_page = 1; next_page < this->page_nums; ++next_page) {
            if ((packed_page_nums >= PUMP_SG_PACK_MAX) || 
                (page_to_pfn(this->page_list[next_page]) != page_to_pfn(this->page_list[next_page-1]) + 1)) {
                ++sg_nums;
                packed_page_nums = 1;
            } else {
                ++packed_page_nums;
            }
        }
#else
        sg_nums = this->page_nums;
#endif
        if (PUMP_DEBUG_CHECK(this,debug_phase))
            dev_info(this->dev, "sg_table.sgl = kmalloc(%d*%d)\n", sg_nums, sizeof(struct scatterlist));
        this->sg_table.sgl = kmalloc(sg_nums * sizeof(struct scatterlist), GFP_KERNEL);
	if (IS_ERR_OR_NULL(this->sg_table.sgl)) {
            result = PTR_ERR(this->sg_table.sgl);
            this->sg_table.sgl        = NULL;
            this->sg_table.nents      = 0;
            this->sg_table.orig_nents = 0;
            this->sg_nums             = 0;
            return result;
        }
        sg_init_table(this->sg_table.sgl, sg_nums);
        this->sg_table.nents      = sg_nums;
        this->sg_table.orig_nents = sg_nums;
        curr_page = 0;
        for_each_sg(this->sg_table.sgl, sg, this->sg_table.nents, sg_count) {
            size_t page_size;
            size_t xfer_size;
#if (PUMP_SG_PACK_MAX > 0)
            unsigned int packed_page_nums = 1;
            for (next_page = curr_page + 1; next_page < this->page_nums; ++next_page) {
                if ((packed_page_nums >= PUMP_SG_PACK_MAX) || 
                    (page_to_pfn(this->page_list[next_page]) != page_to_pfn(this->page_list[next_page-1]) + 1)) {
                    break;
                } else {
                    ++packed_page_nums;
                }
            }
#else
            next_page = curr_page + 1;
#endif
            page_size    = ((next_page - curr_page) << PAGE_SHIFT) - page_offset;
            xfer_size    = min(remain_size, page_size);
            sg_set_page(sg, this->page_list[curr_page], xfer_size, page_offset);
            remain_size -= xfer_size;
	    page_offset  = 0;
	    curr_page    = next_page;
	}
    }
#endif

    this->sg_nums = dma_map_sg(this->dev, this->sg_table.sgl, this->sg_table.nents, dma_direction);

    if (0 == this->sg_nums) {
        pump_free_sg_table(this);
        result = -ENOMEM;
        goto failed;
    }

  success:
    if (PUMP_DEBUG_CHECK(this,debug_phase))
        dev_info(this->dev, "pump_alloc_sg_table_from_pages() => success\n");
    return 0;

  failed:
    if (PUMP_DEBUG_CHECK(this,debug_phase))
        dev_info(this->dev, "pump_alloc_sg_table_from_pages() => error(%d)\n", result);
    return result;
}


/**
 * pump_buffer_setup()
 */
static int  pump_buffer_setup(
    struct pump_driver_data* this, 
    char __user*             buff, 
    size_t*                  xfer_size, 
    bool                     xfer_first, 
    bool                     xfer_last
)
{
    int  result = 0;
    u64  start_time;

    if (PUMP_DEBUG_CHECK(this,debug_phase))
        dev_info(this->dev, "pump_buffer_setup(%pK,%d)\n", buff, *xfer_size);
    /*
     *
     */
    start_time = get_jiffies_64();
    /*
     * user buffer to page_list
     */
    result = pump_alloc_pages_from_user_buffer(this, buff, *xfer_size);
    if (result) 
        goto failed;
    /* pump_debug_pages(this); */
    /*
     * page_list to sg_table
     */
    result = pump_alloc_sg_table_from_pages(this, buff, *xfer_size);
    if (result) 
        goto failed;
    /* pump_debug_sg_table(this); */
    /*
     * sg_table to pump_buf_list
     */
    result = pump_proc_add_buf_list_from_sg(
        &this->pump_proc_data, /* struct pump_proc_data*  this       */
        &this->pump_buf_list , /* struct list_head*       buf_list   */
        this->sg_table.sgl   , /* struct scatterlist*     sg_list    */
        this->sg_nums        , /* unsigned int            sg_nums    */
        xfer_first           , /* bool                    xfer_first */
        xfer_last            , /* bool                    xfer_last  */
        PUMP_XFER_AXI_MODE     /* unsigned int            xfer_mode  */
    );
    /*
     *
     */
    this->usec_buffer_setup += jiffies_to_usecs((unsigned long)(get_jiffies_64() - start_time));
    /*
     *
     */
    if (PUMP_DEBUG_CHECK(this,debug_op_table))
        pump_proc_debug_buf_list(&this->pump_proc_data, &this->pump_buf_list);

    if (PUMP_DEBUG_CHECK(this,debug_phase))
        dev_info(this->dev, "pump_buffer_setup() => success\n");
    return 0;

 failed:
    if (PUMP_DEBUG_CHECK(this,debug_phase))
        dev_info(this->dev, "pump_buffer_setup() => error(%d)\n", result);
    return result;
}

/**
 * pump_buffer_release()
 */
static void pump_buffer_release(struct pump_driver_data* this)
{
    int  i;
    int  dma_direction = (this->direction) ? DMA_TO_DEVICE : DMA_FROM_DEVICE;
    u64  start_time;

    if (PUMP_DEBUG_CHECK(this,debug_phase))
        dev_info(this->dev, "pump_buffer_release()\n");

    start_time = get_jiffies_64();

    pump_proc_clear_buf_list(&this->pump_proc_data, &this->pump_buf_list);

    if (this->sg_nums != 0) {
        dma_unmap_sg(this->dev, this->sg_table.sgl, this->sg_table.nents, dma_direction);
        pump_free_sg_table(this);
        this->sg_nums = 0;
    }

    if (this->page_list != NULL) {
        if (dma_direction == DMA_FROM_DEVICE) {
            for (i = 0; i < this->page_nums; i++) {
                if (!PageReserved(this->page_list[i]))
                    SetPageDirty(this->page_list[i]);
                page_cache_release(this->page_list[i]);
            }
        } else {
            for (i = 0; i < this->page_nums; i++) {
                page_cache_release(this->page_list[i]);
            }
        }
        kfree(this->page_list);
        this->page_list = NULL;
        this->page_nums = 0;
    }
    this->usec_buffer_release += jiffies_to_usecs((unsigned long)(get_jiffies_64() - start_time));
}

/**
 * pump_open() - The is the driver open function.
 * @inode:	Pointer to the inode structure of this device.
 * @file:	Pointer to the file structure.
 * returns:	Success or error status.
 */
static int pump_open(struct inode *inode, struct file *file)
{
    struct pump_driver_data* driver_data;
    int status = 0;

    driver_data = container_of(inode->i_cdev, struct pump_driver_data, cdev);
    file->private_data   = driver_data;
    driver_data->is_open = 1;
    driver_data->usec_buffer_setup   = 0;
    driver_data->usec_buffer_release = 0;
    driver_data->usec_pump_run       = 0;

    return status;
}

/**
 * pump_release() - The is the driver release function.
 * @inode:	Pointer to the inode structure of this device.
 * @file:	Pointer to the file structure.
 * returns:	Success.
 */
static int pump_release(struct inode *inode, struct file *file)
{
    struct pump_driver_data* this = file->private_data;

    this->is_open = 0;

    return 0;
}

/**
 *
 */
static void pump_done_work(struct pump_driver_data* this)
{
    wake_up_interruptible(&this->wait_queue);
}

/**
 * pump_irq() - The main interrupt handler.
 * @irq:	The interrupt number.
 * @data:	Pointer to the driver data structure.
 * returns: IRQ_HANDLED after the interrupt is handled.
 **/
static irqreturn_t pump_irq(int irq, void *data)
{
    struct pump_driver_data* this = data;
    return pump_proc_irq(&this->pump_proc_data);
}

/**
 * pump_read() - The is the driver read function.
 * @file:	Pointer to the file structure.
 * @buff:	Pointer to the user buffer.
 * @count:	The number of bytes to be written.
 * @ppos:	Pointer to the offset value
 * returns:	Success or error status.
 */
static ssize_t pump_read(struct file* file, char __user* buff, size_t count, loff_t* ppos)
{
    struct pump_driver_data* this       = file->private_data;
    int                      result     = 0;
    int                      status     = 0;
    size_t                   xfer_size  = 0;
    bool                     xfer_first = (*ppos == 0) ? 1 : 0;
    bool                     xfer_last;
    u64                      start_time;
    /*
     *
     */
    if (mutex_lock_interruptible(&this->sem))
        return -ERESTARTSYS;
    /*
     * limit_sizeを越える読み出しは、EOF(result=0)を返す.
     */
    if (*ppos >= this->limit_size) {
        result = 0;
        goto return_unlock;
    }
    /*
     *
     */
    if (*ppos + count >= this->limit_size) {
        xfer_last = 1;
        xfer_size = this->limit_size - *ppos;
    }
    else {
        xfer_last = 0;
        xfer_size = count;
    }
    /*
     *
     */
    status = pump_buffer_setup(
                 this        , /* struct pump_driver_data* this       */
                 buff        , /* char __user*             buff       */
                 &xfer_size  , /* size_t*                  xfer_size  */
                 xfer_first  , /* bool                     xfer_first */
                 xfer_last     /* bool                     xfer_last  */
    );
    if (status != 0) {
        result = status;
        goto return_unlock;
    }
    /*
     *
     */
    start_time = get_jiffies_64();
    status = pump_proc_start(&this->pump_proc_data, &this->pump_buf_list);
    if (status != 0) {
        result = status;
        goto return_release;
    }
    status = wait_event_interruptible_timeout(
                 this->wait_queue                    , /* wait_queue_head_t wq */
                 (this->pump_proc_data.status != 0)  , /* bool condition       */
                 msecs_to_jiffies(this->timeout_msec)  /* long timeout         */
             );
    if (status == 0) {
        pump_proc_stop(&this->pump_proc_data);
        result = -ETIMEDOUT;
        goto return_release;
    }
    this->usec_pump_run += jiffies_to_usecs((unsigned long)(get_jiffies_64() - start_time));
    if (0) {
        dev_info(this->dev, "STAT=%08X\n", this->pump_proc_data.status);
        dev_info(this->dev, "CORE=%08X,%08X,%08X\n", 
                 regs_read(this->core_regs_addr+ 0),
                 regs_read(this->core_regs_addr+ 8),
                 regs_read(this->core_regs_addr+12));
        dev_info(this->dev, "PROC=%08X,%08X,%08X\n", 
                 regs_read(this->proc_regs_addr+ 0),
                 regs_read(this->proc_regs_addr+ 8),
                 regs_read(this->proc_regs_addr+12));
    }
    /*
     *
     */
    *ppos += xfer_size;
    result = xfer_size;
    /*
     *
     */
 return_release:
    pump_buffer_release(this);
 return_unlock:
    mutex_unlock(&this->sem);
    return result;
}

/**
 * pump_write() - The is the driver write function.
 * @file:	Pointer to the file structure.
 * @buff:	Pointer to the user buffer.
 * @count:	The number of bytes to be written.
 * @ppos:	Pointer to the offset value
 * returns:	Success or error status.
 */
static ssize_t pump_write(struct file* file, const char __user* buff, size_t count, loff_t* ppos)
{
    struct pump_driver_data* this       = file->private_data;
    int                      result     = 0;
    int                      status     = 0;
    size_t                   xfer_size  = count;
    bool                     xfer_first = (*ppos == 0) ? 1 : 0;
    bool                     xfer_last;
    u64                      start_time;
    /*
     *
     */
    if (mutex_lock_interruptible(&this->sem))
        return -ERESTARTSYS;
    /*
     * limit_sizeを越える書き込みは、書いたフリをする.
     */
    if (*ppos >= this->limit_size) {
        *ppos += count;
        result = count;
        goto return_unlock;
    }
    /*
     *
     */
    if (*ppos + count >= this->limit_size) {
        xfer_last = 1;
        xfer_size = this->limit_size - *ppos;
    }
    else {
        xfer_last = 0;
        xfer_size = count;
    }
    /*
     *
     */
    status = pump_buffer_setup(
                 this        , /* struct pump_driver_data* this       */
                 buff        , /* char __user*             buff       */
                 &xfer_size  , /* size_t*                  xfer_size  */
                 xfer_first  , /* bool                     xfer_first */
                 xfer_last     /* bool                     xfer_last  */
    );
    if (status != 0) {
        result = status;
        goto return_unlock;
    }
    /*
     *
     */
    start_time = get_jiffies_64();
    status = pump_proc_start(&this->pump_proc_data, &this->pump_buf_list);
    if (status != 0) {
        result = status;
        goto return_release;
    }
    status = wait_event_interruptible_timeout(
                 this->wait_queue                    , /* wait_queue_head_t wq */
                 (this->pump_proc_data.status != 0)  , /* bool condition       */
                 msecs_to_jiffies(this->timeout_msec)  /* long timeout         */
             );
    if (status == 0) {
        pump_proc_stop(&this->pump_proc_data);
        result = -ETIMEDOUT;
        goto return_release;
    }
    this->usec_pump_run += jiffies_to_usecs((unsigned long)(get_jiffies_64() - start_time));
    if (0) {
        dev_info(this->dev, "STAT=%08X\n", this->pump_proc_data.status);
        dev_info(this->dev, "CORE=%08X,%08X,%08X\n", 
                 regs_read(this->core_regs_addr+ 0),
                 regs_read(this->core_regs_addr+ 8),
                 regs_read(this->core_regs_addr+12));
        dev_info(this->dev, "PROC=%08X,%08X,%08X\n", 
                 regs_read(this->proc_regs_addr+ 0),
                 regs_read(this->proc_regs_addr+ 8),
                 regs_read(this->proc_regs_addr+12));
    }
    /*
     *
     */
    *ppos += xfer_size;
    result = xfer_size;
    /*
     *
     */
 return_release:
    pump_buffer_release(this);
 return_unlock:
    mutex_unlock(&this->sem);
    return result;
}

/**
 *
 */
static const struct file_operations pump_driver_intake_fops = {
    .owner   = THIS_MODULE,
    .open    = pump_open,
    .release = pump_release,
    .write   = pump_write,
};
static const struct file_operations pump_driver_outlet_fops = {
    .owner   = THIS_MODULE,
    .open    = pump_open,
    .release = pump_release,
    .read    = pump_read,
};

/**
 * pump_driver_probe() -  Probe call for the device.
 *
 * @pdev:	handle to the platform device structure.
 * Returns 0 on success, negative error otherwise.
 *
 * It does all the memory allocation and registration for the device.
 */
static int pump_driver_probe(struct platform_device *pdev)
{
    struct pump_driver_data*    this     = NULL;
    int                         result   = 0;
    unsigned int                done     = 0;
    const unsigned int          DONE_ADD_CHRDEV             = (1 <<  0);
    const unsigned int          DONE_GET_CORE_REGS_RESOUCE  = (1 <<  1);
    const unsigned int          DONE_REQ_CORE_REGS_REGION   = (1 <<  2);
    const unsigned int          DONE_MAP_CORE_REGS_ADDR     = (1 <<  3);
    const unsigned int          DONE_GET_PROC_REGS_RESOUCE  = (1 <<  4);
    const unsigned int          DONE_REQ_PROC_REGS_REGION   = (1 <<  5);
    const unsigned int          DONE_MAP_PROC_REGS_ADDR     = (1 <<  6);
    const unsigned int          DONE_DEVICE_CREATE          = (1 <<  7);
    const unsigned int          DONE_GET_IRQ_RESOUCE        = (1 <<  8);
    const unsigned int          DONE_IRQ_REQUEST            = (1 <<  9);
    const unsigned int          DONE_PUMP_PROC_SETUP        = (1 << 10);
    unsigned long               core_regs_addr = 0L;
    unsigned long               core_regs_size = 0L;
    unsigned long               proc_regs_addr = 0L;
    unsigned long               proc_regs_size = 0L;
    char*                       device_name;

#if (PUMP_DEBUG == 1)
    dev_info(&pdev->dev, "PUMP Driver probe start\n");
#endif
    /*
     * create (pump_driver_data*)this.
     */
    {
        this = kzalloc(sizeof(*this), GFP_KERNEL);
        if (IS_ERR_OR_NULL(this)) {
            dev_err(&pdev->dev, "couldn't allocate device private record\n");
            result = PTR_ERR(this);
            this = NULL;
            goto failed;
        }
        dev_set_drvdata(&pdev->dev, this);
    }
    /*
     * get device number
     */
    {
        u32 minor_number;
        int status;
        status = of_property_read_u32(pdev->dev.of_node, "minor-number", &minor_number);
        if (status != 0) {
            dev_err(&pdev->dev, "invalid property minor number\n");
            result = -ENODEV;
            goto failed;
        }
        this->device_number = MKDEV(MAJOR(pump_device_number), MINOR(minor_number));
    }
    /*
     * get direction to make this->direction
     */
    {
        u32 direction;
        int status;          
        status = of_property_read_u32(pdev->dev.of_node, "direction", &direction);
        if ((status != 0) || (direction > 1)) {
            dev_err(&pdev->dev, "invalid property direction \n");
            result = -ENODEV;
            goto failed;
        }
        this->direction = direction;
    }
    /*
     * device create to this->dev and device_name
     */
    {
        this->dev = device_create(
                        pump_sys_class           , /* struct class*  class   */
                        NULL                     , /* struct device* parent  */
                        this->device_number      , /* dev_t          devt    */
                        (void *)this             , /* void*          drvdata */
                        DEVICE_NAME_FORMAT       , /* const char*    fmt     */
                        MINOR(this->device_number) /* ...                    */
                    );
        if (IS_ERR_OR_NULL(this->dev)) {
            dev_err(&pdev->dev, "device_create() failed\n");
            result = PTR_ERR(this->dev);
            this->dev = NULL;
            goto failed;
        }
        device_name = dev_name(this->dev);
        dma_set_coherent_mask(this->dev, 0xFFFFFFFF);
        done |= DONE_DEVICE_CREATE;
    }
    /*
     * get core register resouce and ioremap to this->core_regs_addr
     */
    {
        this->core_regs_res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
        if (this->core_regs_res == NULL) {
            dev_err(&pdev->dev, "invalid register address\n");
            result = -ENODEV;
            goto failed;
        }
        done |= DONE_GET_CORE_REGS_RESOUCE;
        core_regs_addr = this->core_regs_res->start;
        core_regs_size = this->core_regs_res->end - this->core_regs_res->start + 1;

        if (request_mem_region(core_regs_addr, core_regs_size, device_name) == NULL) {
            dev_err(&pdev->dev, "couldn't lock memory region at %pr\n", this->core_regs_res);
            result = -EBUSY;
            goto failed;
        }
        done |= DONE_REQ_CORE_REGS_REGION;

        this->core_regs_addr = ioremap_nocache(core_regs_addr, core_regs_size);
        if (this->core_regs_addr == NULL) {
            dev_err(&pdev->dev, "ioremap(%pr) failed\n", this->core_regs_res);
            result = -ENODEV;
            goto failed;
        }
        done |= DONE_MAP_CORE_REGS_ADDR;
    }
    /*
     * get proc register resouce and ioremap to this->prco_regs_addr
     */
    {
        this->proc_regs_res = platform_get_resource(pdev, IORESOURCE_MEM, 1);
        if (this->proc_regs_res != NULL) {
            proc_regs_addr = this->proc_regs_res->start;
            proc_regs_size = this->proc_regs_res->end - this->proc_regs_res->start + 1;

            if (request_mem_region(proc_regs_addr, proc_regs_size, device_name) == NULL) {
                dev_err(&pdev->dev, "couldn't lock memory region at %pr\n", this->proc_regs_res);
                result = -EBUSY;
                goto failed;
            }
            done |= DONE_REQ_PROC_REGS_REGION;

            this->proc_regs_addr = ioremap_nocache(proc_regs_addr, proc_regs_size);
            if (this->proc_regs_addr == NULL) {
                dev_err(&pdev->dev, "ioremap(%pr) failed\n", this->proc_regs_res);
                result = -ENODEV;
                goto failed;
            }
            done |= DONE_MAP_PROC_REGS_ADDR;
        }
        done |= DONE_GET_PROC_REGS_RESOUCE;
    }
    /*
     * get interrupt number to this->irq
     */
    {
        this->irq_res = platform_get_resource(pdev, IORESOURCE_IRQ, 0);
        if (this->irq_res == NULL) {
            dev_err(&pdev->dev, "interrupt not found\n");
            result = -ENODEV;
            goto failed;
        }
        done |= DONE_GET_IRQ_RESOUCE;

        this->irq = this->irq_res->start;

        if (request_irq(this->irq, pump_irq, IRQF_DISABLED | IRQF_SHARED, device_name, this) != 0) {
            dev_err(&pdev->dev, "request_irq(%pr) failed\n", this->irq_res);
            result = -EBUSY;
            goto failed;
        }
        done |= DONE_IRQ_REQUEST;
    }
    /*
     * add chrdev.
     */
    {
        if (this->direction == 0)
            cdev_init(&this->cdev, &pump_driver_outlet_fops);
        else
            cdev_init(&this->cdev, &pump_driver_intake_fops);
        this->cdev.owner = THIS_MODULE;
        if (cdev_add(&this->cdev, this->device_number, 1) != 0) {
            dev_err(&pdev->dev, "cdev_add() failed\n");
            result = -ENODEV;
            goto failed;
        }
        done |= DONE_ADD_CHRDEV;
    }
    /*
     *
     */
    this->limit_size   = 0xFFFFFFFF;
    this->timeout_msec = PUMP_TIMEOUT_DEF;
    this->usec_buffer_setup   = 0;
    this->usec_buffer_release = 0;
    this->usec_pump_run       = 0;
    mutex_init(&this->sem);
    INIT_LIST_HEAD(&this->pump_buf_list);
    init_waitqueue_head(&this->wait_queue);

#if (PUMP_DEBUG == 1)
    this->debug_phase     = 0;
    this->debug_op_table  = 0;
    this->debug_sg_table  = 0;
    this->debug_interrupt = 0;
#endif   
    /*
     *
     */
    {
        int status;
        status = pump_proc_setup(
                     &this->pump_proc_data, /* struct pump_proc_data* this        */
                     this->dev            , /* struct device*         dev         */
                     this->direction      , /* int                    direction   */
                     this->proc_regs_addr , /* void __iomem*          regs_addr   */
                     pump_done_work       , /* void                   (*done_func)*/
                     (void*)this            /* void*                  done_arg    */
                 );
        this->pump_proc_data.link_mode = PUMP_LINK_AXI_MODE;
        done |= DONE_PUMP_PROC_SETUP;
    }
    /*
     *
     */
#if (PUMP_DEBUG == 1)
    {
        dev_info(this->dev, "driver installed\n");
        dev_info(this->dev, "private record     = %pK (%dbytes)\n", this, sizeof(*this));
        dev_info(this->dev, "direction          = %d\n"           , this->direction);
        dev_info(this->dev, "major number       = %d\n"           , MAJOR(this->device_number));
        dev_info(this->dev, "minor number       = %d\n"           , MINOR(this->device_number));
        dev_info(this->dev, "core regs resource = %pr\n"          , this->core_regs_res );
        dev_info(this->dev, "core regs address  = %pK\n"          , this->core_regs_addr);
    }
    if (this->proc_regs_res != NULL) {
        dev_info(this->dev, "proc regs resource = %pr\n"          , this->proc_regs_res );
        dev_info(this->dev, "proc regs address  = %pK\n"          , this->proc_regs_addr);
    }
    { 
        dev_info(this->dev, "irq resource       = %pr\n"          , this->irq_res  );
    }
#endif
    return 0;

 failed:
    if (done & DONE_PUMP_PROC_SETUP     ) { pump_proc_cleanup(&this->pump_proc_data);}
    if (done & DONE_IRQ_REQUEST         ) { free_irq(this->irq, this); }
    if (done & DONE_DEVICE_CREATE       ) { device_destroy(pump_sys_class, this->device_number);}
    if (done & DONE_MAP_CORE_REGS_ADDR  ) { iounmap(this->core_regs_addr); }
    if (done & DONE_REQ_CORE_REGS_REGION) { release_mem_region(core_regs_addr, core_regs_size);}
    if (done & DONE_MAP_PROC_REGS_ADDR  ) { iounmap(this->proc_regs_addr); }
    if (done & DONE_REQ_PROC_REGS_REGION) { release_mem_region(proc_regs_addr, proc_regs_size);}
    if (done & DONE_ADD_CHRDEV          ) { cdev_del(&this->cdev); }
    if (this != NULL)                     { kfree(this); }
    return result;
}


/**
 * pump_driver_remove() -  Remove call for the device.
 *
 * @pdev:	handle to the platform device structure.
 * Returns 0 or error status.
 *
 * Unregister the device after releasing the resources.
 */
static int pump_driver_remove(struct platform_device *pdev)
{
    struct pump_driver_data* this  = dev_get_drvdata(&pdev->dev);

    if (!this)
        return -ENODEV;

    pump_proc_clear_buf_list(&this->pump_proc_data, &this->pump_buf_list);
    pump_proc_cleanup(&this->pump_proc_data);

    device_destroy(pump_sys_class, this->device_number);

    free_irq(this->irq, this);

    if (this->core_regs_addr != NULL) {
        unsigned long regs_addr = this->core_regs_res->start;
        unsigned long regs_size = this->core_regs_res->end - this->core_regs_res->start + 1;
        iounmap(this->core_regs_addr);
        release_mem_region(regs_addr, regs_size);
    }

    if (this->proc_regs_addr != NULL) {
        unsigned long regs_addr = this->proc_regs_res->start;
        unsigned long regs_size = this->proc_regs_res->end - this->proc_regs_res->start + 1;
        iounmap(this->proc_regs_addr);
        release_mem_region(regs_addr, regs_size);
    }

    cdev_del(&this->cdev);

    kfree(this);

    dev_set_drvdata(&pdev->dev, NULL);

#if (PUMP_DEBUG == 1)
    dev_info(&pdev->dev, "driver unloaded\n");
#endif
    return 0;
}

/**
 * Open Firmware Device Identifier Matching Table
 */
static struct of_device_id pump_of_match[] = {
    { .compatible = "ikwzm,pump-0.70.a", },
    { /* end of table */}
};

MODULE_DEVICE_TABLE(of, pump_of_match);

/**
 * Platform Driver Structure
 */
static struct platform_driver pump_platform_driver = {
    .probe  = pump_driver_probe,
    .remove = pump_driver_remove,
    .driver = {
        .owner = THIS_MODULE,
        .name  = DRIVER_NAME,
        .of_match_table = pump_of_match,
    },
};

/**
 * pump_module_init()
 */
static int __init pump_module_init(void)
{
    int                result = 0;
    unsigned int       done   = 0;
    const unsigned int DONE_ALLOC_CHRDEV    = (1 << 0);
    const unsigned int DONE_CREATE_CLASS    = (1 << 1);
    const unsigned int DONE_REGISTER_DRIVER = (1 << 2);

    result = alloc_chrdev_region(&pump_device_number, 0, 0, DRIVER_NAME);
    if (result != 0) {
        printk(KERN_ERR "%s: couldn't allocate device major number\n", DRIVER_NAME);
        goto failed;
    }
    done |= DONE_ALLOC_CHRDEV;

    pump_sys_class = class_create(THIS_MODULE, DRIVER_NAME);
    if (IS_ERR_OR_NULL(pump_sys_class)) {
        printk(KERN_ERR "%s: couldn't create sys class\n", DRIVER_NAME);
        result = PTR_ERR(pump_sys_class);
        pump_sys_class = NULL;
        goto failed;
    }
    SET_SYS_CLASS_ATTRIBUTES(pump_sys_class);

    done |= DONE_CREATE_CLASS;

    result = platform_driver_register(&pump_platform_driver);
    if (result) {
        printk(KERN_ERR "%s: couldn't register platform driver\n", DRIVER_NAME);
        goto failed;
    }
    done |= DONE_REGISTER_DRIVER;

    return 0;

 failed:
    if (done & DONE_REGISTER_DRIVER){platform_driver_unregister(&pump_platform_driver);}
    if (done & DONE_CREATE_CLASS   ){class_destroy(pump_sys_class);}
    if (done & DONE_ALLOC_CHRDEV   ){unregister_chrdev_region(pump_device_number, 0);}

    return result;
}

/**
 * pump_module_exit()
 */
static void __exit pump_module_exit(void)
{
    platform_driver_unregister(&pump_platform_driver);
    class_destroy(pump_sys_class);
    unregister_chrdev_region(pump_device_number, 0);
}


module_init(pump_module_init);
module_exit(pump_module_exit);

MODULE_AUTHOR("ikwzm");
MODULE_DESCRIPTION("PUMP Driver");
MODULE_LICENSE("GPL");

