/*
 * pump_proc.h
 *
 * Copyright (C) 2014 Ichiro Kawazome
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
#ifndef _PUMP_PROC_H_
#define _PUMP_PROC_H_

#include <linux/types.h>
#include <linux/device.h>
#include <linux/scatterlist.h>
#include <linux/list.h>
#include <linux/spinlock.h>
#include <linux/workqueue.h>
#include <linux/kernel.h>
#include <linux/interrupt.h>

/**
 * struct pump_proc_data - Pump proc driver data structure
 *
 */
struct pump_proc_data {
    struct device*       dev;
    int                  direction;
    void __iomem*        regs_addr;
    spinlock_t           irq_lock;
    unsigned int         link_mode;
    unsigned int         status;
    struct list_head     op_list;
    bool                 irq_enable;
    struct work_struct   irq_work;
    void                 (*done_func)(void* done_arg);
    void*                done_arg;
    unsigned int         debug;
};

#define PUMP_PROC_DEBUG_PHASE (0x00000001)
#define PUMP_PROC_DEBUG_IRQ   (0x00000002)

int         pump_proc_setup(
                struct pump_proc_data* this     ,
                struct device*         dev      ,
                int                    direction,
                void __iomem*          regs_addr,
                void                   (*done_func)(void* done_args),
                void*                  done_arg
            );
int         pump_proc_cleanup(struct pump_proc_data* this);
irqreturn_t pump_proc_irq    (struct pump_proc_data* this);
int         pump_proc_start  (struct pump_proc_data* this);
int         pump_proc_stop   (struct pump_proc_data* this);
void        pump_proc_debug_opecode_table(struct pump_proc_data* this);
void        pump_proc_clear_opcode_table (struct pump_proc_data* this);
int         pump_proc_add_opecode_table_from_sg(
                struct pump_proc_data*  this      ,
                struct scatterlist*     sg_list   , 
                unsigned int            sg_nums   , 
                bool                    xfer_first, 
                bool                    xfer_last ,
                unsigned int            xfer_mode
            );
#endif
