#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "param.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "fs.h"
#include "buf.h"

// Simple logging that allows concurrent FS system calls.
//
// A log transaction contains the updates of multiple FS system
// calls. The logging system only commits when there are
// no FS system calls active. Thus there is never
// any reasoning required about whether a commit might
// write an uncommitted system call's updates to disk.
//
// A system call should call begin_op()/end_op() to mark
// its start and end. Usually begin_op() just increments
// the count of in-progress FS system calls and returns.
// But if it thinks the log is close to running out, it
// sleeps until the last outstanding end_op() commits.
//
// The log is a physical re-do log containing disk blocks.
// The on-disk log format:
//   header block, containing block #s for block A, B, C, ...
//   block A
//   block B
//   block C
//   ...
// Log appends are synchronous.

/*
**********************************************************************************************************************
xv6
的日志系统旨在保证文件系统操作的原子性，即一个事务（由多个文件系统调用组成）要么全部完成，要么完全不生效，即使系统崩溃也能恢复到一致状态。日志记录了修改的磁盘块，在事务提交（commit）时将这些块写入磁盘的最终位置，并在崩溃后通过恢复机制（recover_from_log）重放日志。

日志的格式：

日志头块（header block）：记录事务中涉及的块号（block[LOGSIZE]）和块数量（n）。
日志数据块：存储修改后的磁盘块内容，紧跟头块。
日志操作是同步的，确保数据在写入日志时已持久化到磁盘。
**********************************************************************************************************************
*/

// Contents of the header block, used for both the on-disk header block
// and to keep track in memory of logged block# before commit.
// 日志头块（header block）：记录事务中涉及的块号（block[LOGSIZE]）和块数量（n）。
struct logheader
{
    int n;                // 当前日志中有效块的数量
    int block[LOGSIZE];   // 每个日志块对应的实际磁盘块号
};

// 日志数据块：存储修改后的磁盘块内容，紧跟头块。
struct log
{
    struct spinlock  lock;          // 互斥锁
    int              start;         // 日志区起始扇区号
    int              size;          // 日志区总块数
    int              outstanding;   // 未提交的事务数量
    int              committing;    // 是否正在提交事务（0/1）
    int              dev;           // 设备号
    struct logheader lh;            // 日志头（内存缓存）
};
struct log log;

static void recover_from_log(void);
static void commit();

// 初始化日志系统。
void initlog(int dev, struct superblock* sb)
{
    if (sizeof(struct logheader) >= BSIZE)
        panic("initlog: too big logheader");

    initlock(&log.lock, "log");
    log.start = sb->logstart;
    log.size  = sb->nlog;
    log.dev   = dev;
    recover_from_log();
}

// Copy committed blocks from log to their home location
// 将日志中的块内容复制到它们的最终磁盘位置（“home location”）。
static void install_trans(int recovering)
{
    int tail;   // 循环索引，用于遍历日志块

    // 遍历日志中所有待应用的块
    for (tail = 0; tail < log.lh.n; tail++)
    {
        // 1. 读取磁盘日志块 (lbuf = log block)，读取到内存cache中
        struct buf* lbuf = bread(log.dev, log.start + tail + 1);
        // 2. 读取目标磁盘块 (dbuf = destination block)，读取到内存cache中
        struct buf* dbuf = bread(log.dev, log.lh.block[tail]);
        // 3. 将日志块数据复制到目标块，此时修改仍在内存，未落盘
        memmove(dbuf->data, lbuf->data, BSIZE);
        // 4. 将修改后的目标块写回磁盘
        bwrite(dbuf);
        // 5. 非恢复模式：解固定目标块
        if (recovering == 0)
            bunpin(dbuf);
        brelse(lbuf);
        brelse(dbuf);
    }
}

// Read the log header from disk into the in-memory log header
// 从磁盘读取日志头块到内存中的 log.lh。
static void read_head(void)
{
    // 1. 读取日志头块：从磁盘读取日志区的第一个块（log.start位置）
    struct buf* buf = bread(log.dev, log.start);
    // 2. 将缓冲区数据转换为日志头结构
    struct logheader* lh = (struct logheader*)(buf->data);
    // 3. 复制日志块数量
    log.lh.n = lh->n;
    // 4. 循环复制所有日志条目
    for (int i = 0; i < log.lh.n; i++)
    {
        // 复制每个日志块对应的实际磁盘块号
        log.lh.block[i] = lh->block[i];
    }
    // 5. 释放缓冲区
    brelse(buf);
}

// Write in-memory log header to disk.
// This is the true point at which the
// current transaction commits.
// 将内存中的日志头（log.lh）写入磁盘的头块。
static void write_head(void)
{
    // 1. 读取日志头块到缓冲区
    struct buf* buf = bread(log.dev, log.start);
    // 2. 将缓冲区映射到日志头结构
    struct logheader* hb = (struct logheader*)(buf->data);
    // 3. 复制内存中的日志块数量到磁盘日志头
    hb->n = log.lh.n;
    // 4. 复制内存中的块映射数组到磁盘日志头
    for (int i = 0; i < log.lh.n; i++)
    {
        hb->block[i] = log.lh.block[i];
    }
    // 5. 将修改后的缓冲区写回磁盘
    bwrite(buf);
    brelse(buf);
}

// 从日志中恢复文件系统状态（用于系统启动后的崩溃恢复）。
static void recover_from_log(void)
{
    // 1. 读取磁盘上的日志头信息
    read_head();
    // 2. 如果存在已提交但未应用的日志，将其应用到文件系统
    // 参数1表示处于恢复模式
    install_trans(1);
    // 3. 在内存中标记日志为空
    log.lh.n = 0;
    // 4. 将空日志头写回磁盘，清除日志
    write_head();
}

// called at the start of each FS system call.
// 标记文件系统调用的开始，确保日志空间足够。
void begin_op(void)
{
    // 获取日志锁（保证互斥访问）
    acquire(&log.lock);
    // 循环直到满足执行条件
    while (1)
    {
        // 情况1：日志系统正在提交事务
        if (log.committing)
        {
            // 休眠等待提交完成
            sleep(&log, &log.lock);
        }
        // 情况2：日志空间可能不足
        else if (log.lh.n + (log.outstanding + 1) * MAXOPBLOCKS > LOGSIZE)
        {
            // 休眠等待空间释放
            sleep(&log, &log.lock);
        }
        // 情况3：满足执行条件
        else
        {
            log.outstanding += 1;   // 增加未完成事务计数
            release(&log.lock);     // 释放日志锁
            break;                  // 退出循环
        }
    }
}

// called at the end of each FS system call.
// commits if this was the last outstanding operation.
// 标记文件系统调用的结束，如果是最后一个操作则触发提交。
void end_op(void)
{
    // 提交标志，1表示需要提交事务
    int do_commit = 0;
    // 1. 获取日志
    acquire(&log.lock);
    // 2. 减少未完成事务计数
    log.outstanding -= 1;
    // 3. 安全检查：确保当前没有正在提交的事务
    if (log.committing)
        panic("log.committing");
    // 4. 判断是否需要提交事务
    if (log.outstanding == 0)
    {
        do_commit      = 1;
        log.committing = 1;
    }

    else
    {
        // begin_op() may be waiting for log space,
        // and decrementing log.outstanding has decreased
        // the amount of reserved space.
        // 5. 如果不是最后一个事务，唤醒可能等待的进程
        wakeup(&log);
    }
    // 6. 释放日志锁
    release(&log.lock);
    // 7. 如果需要提交，则执行提交操作
    if (do_commit)
    {

        // 注意：提交时不持有锁（因为commit可能睡眠）
        commit();   // 执行事务提交
        // 提交完成后更新状态
        acquire(&log.lock);
        log.committing = 0;   // 清除提交标志
        wakeup(&log);         // 唤醒等待提交完成的进程
        release(&log.lock);
    }
}

// Copy modified blocks from cache to log.
// 将修改的块从缓存复制到磁盘日志区域。
static void write_log(void)
{
    // 循环索引，用于遍历日志块
    int tail;
    // 遍历所有需要写入日志的块
    for (tail = 0; tail < log.lh.n; tail++)
    {
        // 1. 分配日志块缓冲区（目标位置）
        struct buf* to = bread(log.dev, log.start + tail + 1);
        // 2. 获取源数据缓冲区（修改后的数据）
        struct buf* from = bread(log.dev, log.lh.block[tail]);
        // 3. 数据复制：从缓存块复制到日志块
        memmove(to->data, from->data, BSIZE);
        // 4. 将日志块从内存写入磁盘（日志块）
        bwrite(to);
        brelse(from);
        brelse(to);
    }
}

// 提交事务，将日志内容持久化到磁盘。
static void commit()
{
    // 检查是否有需要提交的日志块
    if (log.lh.n > 0)
    {
        // 阶段1: 将修改的数据块写入磁盘的日志区域
        write_log();
        // 阶段2: 写日志头（标记事务已提交）
        write_head();
        // 阶段3: 将日志中的修改应用到实际的文件系统位置
        install_trans(0);
        // 阶段4: 清理日志
        log.lh.n = 0;
        write_head();
    }
}

// Caller has modified b->data and is done with the buffer.
// Record the block number and pin in the cache by increasing refcnt.
// commit()/write_log() will do the disk write.
//
// log_write() replaces bwrite(); a typical use is:
//   bp = bread(...)
//   modify bp->data[]
//   log_write(bp)
//   brelse(bp)
// 写内存中的日志头log.lh
void log_write(struct buf* b)
{
    int i;
    // 1. 获取日志锁（保证互斥访问）
    acquire(&log.lock);
    // 2. 安全检查：日志空间是否充足
    if (log.lh.n >= LOGSIZE || log.lh.n >= log.size - 1)
        panic("too big a transaction");
    // 3. 安全检查：是否在有效事务中
    if (log.outstanding < 1)
        panic("log_write outside of trans");
    // 4. 检查日志是否已包含该块（日志吸收优化）
    for (i = 0; i < log.lh.n; i++)
    {
        if (log.lh.block[i] == b->blockno)   // 块已存在日志中
            break;
    }
    // 5. 记录块号到日志头
    log.lh.block[i] = b->blockno;
    // 6. 如果是新块，添加到日志
    if (i == log.lh.n)
    {
        bpin(b);      // 固定缓冲区（增加引用计数）
        log.lh.n++;   // 增加日志块计数
    }
    // 7. 释放日志锁
    release(&log.lock);
}
