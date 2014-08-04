/*
 * pump_proc.c
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
#include "pump_proc.h"

#include <linux/slab.h>
#include <linux/dma-mapping.h>
#include <asm/byteorder.h>

/******************************************************************************
 * Pump Processor Registers
 ******************************************************************************
 *           31            24              16               8               0
 *           +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 * Addr=0x00 |                       Address[31:00]                          |
 *           +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 * Addr=0x04 |                       Address[63:31]                          |
 *           +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 * Addr=0x08 |                       Reserve[31:00]                          |
 *           +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 * Addr=0x0C |  Control[7:0] |  Status[7:0]  |          Mode[15:00]          |
 *           +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 ******************************************************************************/
#define PUMP_PROC_REGS_ADDR_LO     (0x00)
#define PUMP_PROC_REGS_ADDR_HI     (0x04)
#define PUMP_PROC_REGS_RESERVE     (0x08)
#define PUMP_PROC_REGS_CTRL_STAT   (0x0C)
/******************************************************************************
 * Pump Processor Mode Register
 ******************************************************************************
 * Mode[00]    = 1:オペレーション終了時(Status[0]='1')に割り込みを発生する.
 * Mode[01]    = 1:Operation Code を読み込んだ時(Status[1]='1')に割り込みを発生する.
 * Mode[07:04] = AXI4 Master Read I/F のキャッシュモードを指定する.
 * Mode[12:08] = AXI4 Master Read I/F の ARUSER/AWUSER の値を指定する.
 ******************************************************************************/
#define PUMP_PROC_REGS_IE_DONE     (0x00000001)
#define PUMP_PROC_REGS_IE_FETCH    (0x00000002)
#define PUMP_PROC_REGS_MODE_MASK   (0x0000FFF0)
#define PUMP_PROC_REGS_MODE_POS    4
/******************************************************************************
 * Pump Processor Status Register
 ******************************************************************************
 * Status[0]   = 1:Endフラグ付きのオペレーションコードを処理し終えた事をを示す.
 * Status[1]   = 1:Fetchフラグ付きのオペレーションコードを読み込んだ事を示す.
 * Status[2]   = 1:不正なオペレーションコードを読み込んだ事を示す.
 * Status[3]   = 1:オペレーションコード読み込み時にエラーが発生した事を示す.
 * Status[4]   = 1:転送時にエラーが発生した事を示す.
 ******************************************************************************/
#define PUMP_PROC_REGS_STAT_POS    16
#define PUMP_PROC_REGS_STAT_ALL    (0x00FF0000)
#define PUMP_PROC_REGS_STAT_DONE   (0x00010000)
#define PUMP_PROC_REGS_STAT_FETCH  (0x00020000)
#define PUMP_PROC_REGS_STAT_OERR   (0x00040000)
#define PUMP_PROC_REGS_STAT_FERR   (0x00080000)
#define PUMP_PROC_REGS_STAT_MERR   (0x00100000)
/******************************************************************************
 * Pump Processor Control Register
 ******************************************************************************
 * Control[4]  = 1:オペレーションを開始する.     0:意味無し.
 * Control[5]  = 1:オペレーションを中止する.     0:意味無し.
 * Control[6]  = 1:オペレーションを一時中断する. 0:オペレーションを再開する.
 * Control[7]  = 1:モジュールをリセットする.     0:リセットを解除する.
 ******************************************************************************/
#define PUMP_PROC_REGS_CTRL_START  (0x10000000)
#define PUMP_PROC_REGS_CTRL_STOP   (0x20000000)
#define PUMP_PROC_REGS_CTRL_PAUSE  (0x40000000)
#define PUMP_PROC_REGS_CTRL_RESET  (0x80000000)

/******************************************************************************
 *  Register read/write access routines
 ******************************************************************************/
#define regs_write(offset, val)	__raw_writel(cpu_to_le32(val), offset)
#define regs_read(offset)       le32_to_cpu(__raw_readl(offset))

/******************************************************************************
 * Operation Code Foramt
 ******************************************************************************
 *           31            24              16               8               0
 *           +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *     +0x00 |                                                               |
 *           +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *     +0x04 |                                                               |
 *           +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *     +0x08 |                                                               |
 *           +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *     +0x0C | TYPE  |D|F|                                                   |
 *           +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 ******************************************************************************
 * TYPE        = オペレーションコードのタイプ.
 * Done        = このビットが'1'の場合、オペレーション終了時にオペレーションプ
 *               ロセッサのStatus[0]をセットすることを指定する.
 * Fetch       = このビットが'1'の場合、オペレーションコード読み込み時にオペレ
 *               ーションプロセッサのStatus[1]をセットすることを指定する.
 ******************************************************************************/
struct opecode { u32 code[4]; };
#define PUMP_PROC_OPECODE_TYPE_POS        28
#define PUMP_PROC_OPECODE_TYPE_MASK       (0xF0000000)
#define PUMP_PROC_OPECODE_DONE_MASK       (0x08000000)
#define PUMP_PROC_OPECODE_FETCH_MASK      (0x04000000)
static inline u32  opecode_command(unsigned int type,bool done,bool fetch)
{
    return (u32)(((type << PUMP_PROC_OPECODE_TYPE_POS) & PUMP_PROC_OPECODE_TYPE_MASK) | 
                 ((done ) ? PUMP_PROC_OPECODE_DONE_MASK  : 0) | 
                 ((fetch) ? PUMP_PROC_OPECODE_FETCH_MASK : 0));
}
/******************************************************************************
 * Operation Code(NONE)
 ******************************************************************************
 *           31            24              16               8               0
 *           +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *     +0x00 |                       Reserve[31:00]                          |
 *           +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *     +0x04 |                       Reserve[63:31]                          |
 *           +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *     +0x08 |                       Reserve[95:64]                          |
 *           +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *     +0x0C |0|0|0|0|D|F|           Reserve[121:96]                         |
 *           +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 ******************************************************************************
 * TYPE        = "0000"
 * Done        = このビットが'1'の場合、オペレーション終了時にオペレーションプ
 *               ロセッサのStatus[0]をセットすることを指定する.
 * Fetch       = このビットが'1'の場合、オペレーションコード読み込み時にオペレ
 *               ーションプロセッサのStatus[1]をセットすることを指定する.
 ******************************************************************************/
#define PUMP_PROC_OPECODE_NONE_TYPE       (0x0)
static inline void set_none_opecode(
    struct opecode*      op_ptr, 
    bool                 fetch ,
    bool                 done
){
    op_ptr->code[0] = 0x00000000;
    op_ptr->code[1] = 0x00000000;
    op_ptr->code[2] = 0x00000000;
    op_ptr->code[3] = cpu_to_le32(opecode_command(PUMP_PROC_OPECODE_NONE_TYPE, done, fetch));
}
/******************************************************************************
 * Operation Code(LINK)
 ******************************************************************************
 *           31            24              16               8               0
 *           +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *     +0x00 |                       Address[31:00]                          |
 *           +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *     +0x04 |                       Address[63:31]                          |
 *           +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *     +0x08 |                       Reserve[31:00]                          |
 *           +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *     +0x0C |1|1|0|1|D|F|L|F|0|0|0|0|0|0|0|0|          Mode[11:00]  |0|0|IE |
 *           +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 ******************************************************************************
 * TYPE        = "1101"
 * Done        = このビットが'1'の場合、オペレーション終了時にオペレーションプ
 *               ロセッサのStatus[0]をセットすることを指定する.
 * Fetch       = このビットが'1'の場合、オペレーションコード読み込み時にオペレ
 *               ーションプロセッサのStatus[1]をセットすることを指定する.
 * Mode[11]    = 1:AXI4 Master I/F をセイフティモードで動かすことを示す.
 * Mode[10]    = 1:AXI4 Master I/F を投機モードで動かすことを示す.
 * Mode[09]    = 1:AXI4 Master I/F をアドレス固定モードにすることを示す.
 * Mode[07:04] = AXI4 Master I/F の ARUSER/AWUSER の値を指定する.
 * Mode[03:00] = AXI4 Master I/F のキャッシュモードを指定する.
 * IE[1]       = 1:エラー発生時(Status[1]='1')に割り込みを発生する.
 * IE[0]       = 1:転送終了時(Status[0]='1')に割り込みを発生する.
 * Address     = オペレーションコードフェッチアドレス.
 ******************************************************************************/
#define PUMP_PROC_OPECODE_LINK_TYPE       (0xD)
#define PUMP_PROC_OPECODE_LINK_MODE_MASK  (0x0000FFF0)
#define PUMP_PROC_OPECODE_LINK_MODE_POS   4
#define PUMP_PROC_OPECODE_IE_ERROR        (0x00000002)
#define PUMP_PROC_OPECODE_IE_DONE         (0x00000001)
static inline void set_link_opecode(
    struct opecode*      op_ptr   , 
    bool                 fetch    ,
    bool                 done     ,
    dma_addr_t           addr     , 
    unsigned int         xfer_mode
){
    op_ptr->code[0] = cpu_to_le32((addr >>  0) & 0xFFFFFFFF);
    op_ptr->code[1] = cpu_to_le32((addr >> 32) & 0xFFFFFFFF);
    op_ptr->code[2] = 0x00000000;
    op_ptr->code[3] = cpu_to_le32(opecode_command(PUMP_PROC_OPECODE_LINK_TYPE, done, fetch) |
                                  ((xfer_mode << PUMP_PROC_OPECODE_LINK_MODE_POS) & PUMP_PROC_OPECODE_LINK_MODE_MASK) |
                                  PUMP_PROC_OPECODE_IE_DONE
                      );
}
/******************************************************************************
 * Operation Code(XFER)
 ******************************************************************************
 *           31            24              16               8               0
 *           +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *     +0x00 |                       Address[31:00]                          |
 *           +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *     +0x04 |                       Address[63:31]                          |
 *           +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *     +0x08 |                          Size[31:00]                          |
 *           +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *     +0x0C |1|1|0|0|D|F|L|F|0|0|0|0|0|0|0|0|       Mode[11:00]     |0|0|0|0|
 *           +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 ******************************************************************************
 * TYPE        = "1100"
 * Done        = このビットが'1'の場合、オペレーション終了時にオペレーションプ
 *               ロセッサのStatus[0]をセットすることを指定する.
 * Fetch       = このビットが'1'の場合、オペレーションコード読み込み時にオペレ
 *               ーションプロセッサのStatus[1]をセットすることを指定する.
 * Last        = 1:連続したトランザクションの開始を指定する.
 * First       = 1:連続したトランザクションの終了を指定する.
 * Mode[11]    = 1:AXI4 Master I/F をセイフティモードで動かすことを示す.
 * Mode[10]    = 1:AXI4 Master I/F を投機モードで動かすことを示す.
 * Mode[09]    = 1:AXI4 Master I/F をアドレス固定モードにすることを示す.
 * Mode[07:04] = AXI4 Master I/F の ARUSER/AWUSER の値を指定する.
 * Mode[03:00] = AXI4 Master I/F のキャッシュモードを指定する.
 * Size[31:00] = 転送サイズ.
 * Address     = 転送開始アドレス.
 ******************************************************************************/
#define PUMP_PROC_OPECODE_XFER_TYPE       (0xC)
#define PUMP_PROC_OPECODE_XFER_FIRST_MASK (0x02000000)
#define PUMP_PROC_OPECODE_XFER_LAST_MASK  (0x01000000)
#define PUMP_PROC_OPECODE_XFER_MODE_MASK  (0x0000FFF0)
#define PUMP_PROC_OPECODE_XFER_MODE_POS   4
static inline void set_xfer_opecode(
    struct opecode*      op_ptr    , 
    bool                 fetch     ,
    bool                 done      ,
    bool                 xfer_first,
    bool                 xfer_last ,
    dma_addr_t           addr      , 
    unsigned int         size      , 
    unsigned int         xfer_mode
){
    op_ptr->code[0] = cpu_to_le32((addr >>  0) & 0xFFFFFFFF);
    op_ptr->code[1] = cpu_to_le32((addr >> 32) & 0xFFFFFFFF);
    op_ptr->code[2] = cpu_to_le32(size);
    op_ptr->code[3] = cpu_to_le32(opecode_command(PUMP_PROC_OPECODE_XFER_TYPE, done, fetch) |
                                  ((xfer_first) ? PUMP_PROC_OPECODE_XFER_FIRST_MASK : 0   ) |
                                  ((xfer_last ) ? PUMP_PROC_OPECODE_XFER_LAST_MASK  : 0   ) |
                                  ((xfer_mode << PUMP_PROC_OPECODE_XFER_MODE_POS) & PUMP_PROC_OPECODE_XFER_MODE_MASK)
                      );
}
static unsigned int fill_xfer_opecodes(
    struct opecode*      op_ptr    ,
    struct scatterlist*  sg_list   , 
    unsigned int         sg_nums   , 
    bool                 xfer_first, 
    bool                 xfer_last ,
    unsigned int         xfer_mode
)
{
    struct scatterlist*  curr_sg;
    int                  sg_index;
    unsigned int         op_count;
    dma_addr_t           dma_address;
    unsigned int         dma_length;
    bool                 dma_last;

    if (IS_ERR_OR_NULL(op_ptr) || IS_ERR_OR_NULL(sg_list) || (sg_nums < 1)) {
        return 0;
    }

    op_count = 0;
    for_each_sg(sg_list, curr_sg, sg_nums, sg_index) {
        dma_address = sg_dma_address(curr_sg);
        dma_length  = sg_dma_len(curr_sg);
        dma_last    = (sg_index >= sg_nums-1) ? xfer_last : 0;
        set_xfer_opecode(
            op_ptr     ,   /* struct opecode* op_ptr     */
            0          ,   /* bool            fetch      */
            0          ,   /* bool            done       */
            xfer_first ,   /* bool            xfer_first */
            dma_last   ,   /* bool            xfer_last  */ 
            dma_address,   /* dma_addr_t      addr       */
            dma_length ,   /* unsigned int    size       */ 
            xfer_mode      /* unsigned int    mode       */
        );
        op_count++;
        op_ptr++;
        xfer_first  = 0;
    }
    return op_count;
}
/******************************************************************************
 * Operation Code Table
 *****************************************************************************/
struct opecode_table {
    struct list_head     list;
    struct opecode*      op_ptr;
    size_t               op_bytes;
    dma_addr_t           dma_addr;
    unsigned int         op_nums;
};
#define OPECODE_TABLE_MAX_ENTRIES (PAGE_SIZE /sizeof(struct opecode))
static void free_opecode_table(struct device* dev, struct list_head* table_list)
{
    if (!list_empty(table_list)) {
        struct opecode_table* curr_table;
        struct list_head*     next_head;
        struct list_head*     curr_head;
        list_for_each_safe(curr_head, next_head, table_list) {
            curr_table = list_entry(curr_head, struct opecode_table, list);
            if (curr_table->op_ptr != NULL) {
                dma_free_coherent(
                    dev                 , /* struct deivce* dev  */  
                    curr_table->op_bytes, /* size_t size         */  
                    curr_table->op_ptr  , /* void* vaddr         */  
                    curr_table->dma_addr  /* dma_addr_t dma_addr */  
                );
            }
            list_del(curr_head);
            kfree(curr_table);
        }
    }
}
static int alloc_opecode_table_from_sg(
    struct device*      dev       ,
    struct list_head*   table_list,
    struct scatterlist* sg_list   , 
    unsigned int        sg_nums   , 
    bool                xfer_first, 
    bool                xfer_last ,
    unsigned int        xfer_mode ,
    unsigned int        link_mode ,
    unsigned int        debug
)
{
    LIST_HEAD(new_table_list);
    int result = 0;

    if (sg_nums > 0) {
        struct opecode_table*  curr_table;
        struct scatterlist*    curr_sg;
        struct scatterlist*    sg_start;
        unsigned int           sg_count;
        int                    sg_index;
        bool                   curr_sg_is_last;

        curr_table   = NULL;
        sg_start     = sg_list;
        sg_count     = 0;

        for_each_sg(sg_list, curr_sg, sg_nums, sg_index) {
            if (curr_table == NULL) {
                curr_table = kzalloc(sizeof(struct opecode_table), GFP_KERNEL);
                if (IS_ERR_OR_NULL(curr_table)) {
                    result = PTR_ERR(curr_table);
                    goto failed;
                }
                INIT_LIST_HEAD(&curr_table->list);
                list_add_tail(&curr_table->list, &new_table_list);
                sg_start = curr_sg;
                sg_count = 0;
            }
            sg_count++;
            curr_sg_is_last = (sg_index >= sg_nums-1) ? 1 : 0;

            if ((curr_sg_is_last) ||
                (sg_count >= OPECODE_TABLE_MAX_ENTRIES-1)) {
                struct opecode* op_ptr;
                bool xfer_last_table = (curr_sg_is_last) ? xfer_last : 0;
                curr_table->op_bytes = (sg_count+1) * sizeof(struct opecode);
                if (debug & PUMP_PROC_DEBUG_PHASE) 
                    dev_info(dev, "dma_alloc_coherent(%d)\n", curr_table->op_bytes);
                op_ptr = dma_alloc_coherent(
                    dev                  , /* struct device* dev   */ 
                    curr_table->op_bytes , /* size_t size          */ 
                    &curr_table->dma_addr, /* dma_addr_t* dma_addr */ 
                    GFP_KERNEL             /* int flag             */ 
                );
                if (debug & PUMP_PROC_DEBUG_PHASE) 
                    dev_info(dev, "dma_alloc_coherent => %pK\n", op_ptr);
                if (IS_ERR_OR_NULL(op_ptr)) {
                    result = PTR_ERR(op_ptr);
                    goto failed;
                }
                if (debug & PUMP_PROC_DEBUG_PHASE) 
                    dev_info(dev, "fill_xfer_opecodes(%d)\n", sg_count);
                curr_table->op_ptr  = op_ptr;
                curr_table->op_nums = fill_xfer_opecodes(
                    curr_table->op_ptr   , /* struct opecode*      op_ptr     */
                    sg_start             , /* struct scatterlist*  sg_list    */
                    sg_count             , /* unsigned int         sg_nums    */
                    xfer_first           , /* bool                 xfer_first */
                    xfer_last_table      , /* bool                 xfer_last  */
                    xfer_mode              /* unsigned int         xfer_mode  */
                );
                if (debug & PUMP_PROC_DEBUG_PHASE) 
                    dev_info(dev, "fill_xfer_opecodes => %d\n", curr_table->op_nums);
                xfer_first = 0;
                curr_table = NULL;
            }
        }
    }

    if (!list_empty(&new_table_list)) {
        struct list_head* curr_head;
        list_for_each(curr_head, &new_table_list) {
            struct opecode_table* curr_table;
            struct opecode_table* next_table;
            bool                  curr_table_is_last;
            struct opecode*       last_op_ptr;
            curr_table  = list_entry(curr_head, struct opecode_table, list);
            if (curr_table->op_ptr != NULL) {
                last_op_ptr = &curr_table->op_ptr[curr_table->op_nums];
                if (list_is_last(curr_head, &new_table_list)) {
                    curr_table_is_last = 1;
                } else {
                    next_table = list_entry(curr_head->next, struct opecode_table, list);
                    curr_table_is_last = (next_table->op_nums == 0) ? 1 : 0;
                }
                if (curr_table_is_last) {
                    set_none_opecode(
                        last_op_ptr         ,  /* struct opecode* op_ptr */
                        0                   ,  /* bool            fetch  */
                        1                      /* bool            done   */
                    );
                    curr_table->op_nums++;
                } else {
                    set_link_opecode(
                        last_op_ptr         ,  /* struct opecode* op_ptr */
                        0                   ,  /* bool            fetch  */
                        0                   ,  /* bool            done   */
                        next_table->dma_addr,  /* dma_addr_t      addr   */
                        link_mode              /* unsigned int    mode   */
                    );
                    curr_table->op_nums++;
                }
            }
        }
    }

    list_splice(&new_table_list, table_list->prev);

    return 0;

  failed:
    free_opecode_table(dev, &new_table_list);
    return result;
}

/**
 *
 */
void pump_proc_debug_opecode_table(struct pump_proc_data* this)
{
    struct list_head*     curr_head;
    struct opecode_table* curr_table;
    unsigned int          t;
    unsigned int          i;
    t = 0;
    list_for_each(curr_head, &this->op_list) {
        curr_table = list_entry(curr_head, struct opecode_table, list);
        dev_info(this->dev, "ope_table(%d) num=%d ptr=%pK size=%d dma_addr=%08X\n", 
                 t, 
                 curr_table->op_nums, 
                 curr_table->op_ptr, 
                 curr_table->op_bytes, 
                 curr_table->dma_addr
        );
        for (i = 0; i < curr_table->op_nums; i++) {
            dev_info(this->dev, "ope(%d) code=%08X addr=%08X size=%d\n", 
                     i, 
                     curr_table->op_ptr[i].code[3],
                     curr_table->op_ptr[i].code[0],
                     curr_table->op_ptr[i].code[2]
            );
        }
        t++;
    }
}

/**
 *
 */
int  pump_proc_add_opecode_table_from_sg(
    struct pump_proc_data*  this      ,
    struct scatterlist*     sg_list   , 
    unsigned int            sg_nums   , 
    bool                    xfer_first, 
    bool                    xfer_last ,
    unsigned int            xfer_mode
)
{
    int status;
    status = alloc_opecode_table_from_sg(
        this->dev       , /* struct device*      dev        */
        &this->op_list  , /* struct list_head*   table_list */
        sg_list         , /* struct scatterlist* sg_list    */
        sg_nums         , /* unsigned int        sg_nums    */
        xfer_first      , /* bool                xfer_first */
        xfer_last       , /* bool                xfer_last  */
        xfer_mode       , /* unsigned int        xfer_mode  */
        this->link_mode , /* unsigned int        link_mode  */
        this->debug       /* unsigned int        debug      */
    );
    return status;
}

/**
 *
 */
void pump_proc_clear_opcode_table(struct pump_proc_data* this)
{
    free_opecode_table(this->dev, &this->op_list);
}

/**
 *
 */
int  pump_proc_start(struct pump_proc_data* this)
{
    unsigned long         irq_flags;
    struct opecode_table* opecode_table;
    volatile u32          ctrl_stat_regs;

    if (list_empty(&this->op_list))
        return -EINVAL;

    opecode_table  = list_first_entry(&this->op_list, struct opecode_table, list);
    ctrl_stat_regs = PUMP_PROC_REGS_CTRL_START | 
                     ((this->link_mode << PUMP_PROC_REGS_MODE_POS) & PUMP_PROC_REGS_MODE_MASK) |
                     PUMP_PROC_REGS_IE_DONE;

    spin_lock_irqsave(&this->irq_lock, irq_flags);
    this->status = 0;
    regs_write(this->regs_addr+PUMP_PROC_REGS_ADDR_LO  , opecode_table->dma_addr);
    regs_write(this->regs_addr+PUMP_PROC_REGS_ADDR_HI  , 0);
    regs_write(this->regs_addr+PUMP_PROC_REGS_RESERVE  , 0);
    regs_write(this->regs_addr+PUMP_PROC_REGS_CTRL_STAT, ctrl_stat_regs);

    ctrl_stat_regs = regs_read(this->regs_addr+PUMP_PROC_REGS_CTRL_STAT);

    spin_unlock_irqrestore(&this->irq_lock, irq_flags);

    return 0;
}

/**
 * 
 */
int pump_proc_stop(struct pump_proc_data* this)
{
    volatile u32  ctrl_stat_regs;
    unsigned long irq_flags;

    spin_lock_irqsave(&this->irq_lock, irq_flags);

    regs_write(this->regs_addr+PUMP_PROC_REGS_CTRL_STAT, PUMP_PROC_REGS_CTRL_STOP);

    ctrl_stat_regs = regs_read(this->regs_addr+PUMP_PROC_REGS_CTRL_STAT);

    spin_unlock_irqrestore(&this->irq_lock, irq_flags);

    return 0;
}

/**
 *
 */
irqreturn_t pump_proc_irq(struct pump_proc_data* this)
{
    volatile u32 ctrl_stat_regs;

    if (this->debug & PUMP_PROC_DEBUG_IRQ)
        dev_info(this->dev, "pump_proc_irq(this=%pK)\n", this);

    spin_lock(&this->irq_lock);

    ctrl_stat_regs = regs_read(this->regs_addr+PUMP_PROC_REGS_CTRL_STAT);

    if ((ctrl_stat_regs & PUMP_PROC_REGS_STAT_ALL) != 0) {
        this->status   |= ((ctrl_stat_regs & PUMP_PROC_REGS_STAT_ALL) >> PUMP_PROC_REGS_STAT_POS);
        ctrl_stat_regs &= ~PUMP_PROC_REGS_STAT_ALL;
        regs_write(this->regs_addr+PUMP_PROC_REGS_CTRL_STAT,ctrl_stat_regs);
        schedule_work(&this->irq_work);
    }

    spin_unlock(&this->irq_lock);

    if (this->debug & PUMP_PROC_DEBUG_IRQ)
        dev_info(this->dev, "pump_proc_irq() => success\n");

    return IRQ_HANDLED;
}

/**
 *
 */
static void pump_proc_irq_work(struct work_struct* work)
{
    struct pump_proc_data* this = container_of(work, struct pump_proc_data, irq_work);

    if (this->debug & PUMP_PROC_DEBUG_IRQ)
        dev_info(this->dev, "pump_proc_irq_work(this=%pK)\n", this);

    if (this->done_func != NULL) {
        this->done_func(this->done_arg);
    }

    if (this->debug & PUMP_PROC_DEBUG_IRQ)
        dev_info(this->dev, "pump_proc_irq_work() => success\n");
}

/**
 *
 */
int pump_proc_setup(
    struct pump_proc_data* this     ,
    struct device*         dev      ,
    int                    direction,
    void __iomem*          regs_addr,
    void                   (*done_func)(void* done_args),
    void*                  done_arg
)
{
    this->dev       = dev;
    this->direction = direction;
    this->regs_addr = regs_addr;
    this->status    = 0;
    this->link_mode = 0;
    this->done_func = done_func;
    this->done_arg  = done_arg;
    this->debug     = 0;
    spin_lock_init(&this->irq_lock);
    INIT_LIST_HEAD(&this->op_list);
    INIT_WORK(&this->irq_work, pump_proc_irq_work);
    return 0;
}
/**
 *
 */
int pump_proc_cleanup(struct pump_proc_data* this)
{
    cancel_work_sync(&this->irq_work);
    free_opecode_table(this->dev, &this->op_list);
    return 0;
}

