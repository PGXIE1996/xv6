/*
********************************************************************************
**  描述了 xv6 文件系统的五层架构：
**      Blocks: 提供 balloc 和 bfree，管理磁盘块分配。
**      Log: 使用日志（log_write, initlog）确保操作原子性和崩溃恢复。
**      Files: 通过 ialloc, iupdate, readi, writei 管理文件内容和元数据。
**      Directories: 通过 dirlookup, dirlink 管理目录（存储 struct dirent）。
**      Names: 通过 namei, nameiparent 解析路径名。
**  这些层次从低到高构建了文件系统的功能，代码按此结构组织。
********************************************************************************
*/

#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "param.h"
#include "stat.h"
#include "spinlock.h"
#include "proc.h"
#include "sleeplock.h"
#include "fs.h"
#include "buf.h"
#include "file.h"

#define min(a, b) ((a) < (b) ? (a) : (b))

// 全局静态变量，存储文件系统的超级块
struct superblock sb;

// 从磁盘设备的块 1 读取超级块，存储到内存的sb。
static void readsb(int dev, struct superblock* sb)
{
    struct buf* bp;
    bp = bread(dev, 1);
    memmove(sb, bp->data, sizeof(*sb));
    brelse(bp);
}

// 初始化文件系统，读取超级块并验证，初始化日志系统。
void fsinit(int dev)
{
    readsb(dev, &sb);
    if (sb.magic != FSMAGIC)
        panic("invalid file system");
    initlog(dev, &sb);
}

// 将设备 dev 的块 bno 清零。并加入到日志块中
static void bzero(int dev, int bno)
{
    struct buf* bp;

    bp = bread(dev, bno);
    memset(bp->data, 0, BSIZE);
    log_write(bp);
    brelse(bp);
}

// 在设备 dev 上分配一个空闲块，返回块号；若无空闲块，返回 0。
static uint balloc(uint dev)
{
    int         b, bi, m;
    struct buf* bp;

    bp = 0;
    // 外层循环：以位图块为单位遍历整个磁盘（每个位图块管理 BPB=8192 个块）
    for (b = 0; b < sb.size; b += BPB)
    {
        // 1. 读取当前位图块（计算位图块号并读取）
        bp = bread(dev, BBLOCK(b, sb));
        // 内层循环：扫描当前位图块的每一位
        for (bi = 0; bi < BPB && b + bi < sb.size; bi++)
        {
            // 2. 计算当前位的掩码（确定在字节中的位置）
            m = 1 << (bi % 8);
            // 3. 检查当前位是否空闲（0 表示空闲）
            if ((bp->data[bi / 8] & m) == 0)
            {
                // 4. 标记位图为已使用（设置对应位）
                bp->data[bi / 8] |= m;
                // 5. 写回位图块（通过日志系统）
                log_write(bp);
                // 6. 释放位图缓冲区
                brelse(bp);
                // 7. 清零新分配的数据块
                bzero(dev, b + bi);
                return b + bi;
            }
        }
        // 当前位图块无空闲块，释放缓冲区
        brelse(bp);
    }
    // 所有块都已分配，报错
    printf("balloc: out of blocks\n");
    return 0;
}

// 释放设备 dev 的块 b，更新空闲位图。
static void bfree(int dev, uint b)
{
    struct buf* bp;
    int         bi, m;

    // 1. 读取包含目标块的位图块
    bp = bread(dev, BBLOCK(b, sb));
    // 2. 计算块在位图块中的位置
    bi = b % BPB;         // 在位图块内的相对位置（0-8191）
    m  = 1 << (bi % 8);   // 计算位掩码
    // 3. 安全检查：确保块当前是已分配状态
    if ((bp->data[bi / 8] & m) == 0)
        panic("freeing free block");
    // 4. 更新位图：标记块为空闲
    bp->data[bi / 8] &= ~m;
    // 5. 写回修改后的位图块（通过日志系统）
    log_write(bp);
    brelse(bp);
}

// 定义全局 itable，管理内存中的 inode 表
struct
{
    struct spinlock lock;
    struct inode    inode[NINODE];   // 最多50个活动的inodes
} itable;

// 初始化 inode 表
void iinit()
{
    int i = 0;

    initlock(&itable.lock, "itable");
    for (i = 0; i < NINODE; i++)
    {
        initsleeplock(&itable.inode[i].lock, "inode");
    }
}

static struct inode* iget(uint dev, uint inum);

// 在设备 dev 上分配一个空闲 inode，设置类型为 type，返回内存中的 inode。
struct inode* ialloc(uint dev, short type)
{
    int            inum;
    struct buf*    bp;
    struct dinode* dip;

    for (inum = 1; inum < sb.ninodes; inum++)
    {
        bp  = bread(dev, IBLOCK(inum, sb));
        dip = (struct dinode*)bp->data + inum % IPB;
        if (dip->type == 0)
        {   // a free inode
            memset(dip, 0, sizeof(*dip));
            dip->type = type;
            log_write(bp);   // mark it allocated on the disk
            brelse(bp);
            return iget(dev, inum);
        }
        brelse(bp);
    }
    printf("ialloc: no inodes\n");
    return 0;
}

// Copy a modified in-memory inode to disk.
// Must be called after every change to an ip->xxx field
// that lives on disk.
// Caller must hold ip->lock.
// 将内存 inode (ip) 的内容同步到磁盘。
void iupdate(struct inode* ip)
{
    struct buf*    bp;
    struct dinode* dip;

    bp         = bread(ip->dev, IBLOCK(ip->inum, sb));
    dip        = (struct dinode*)bp->data + ip->inum % IPB;
    dip->type  = ip->type;
    dip->major = ip->major;
    dip->minor = ip->minor;
    dip->nlink = ip->nlink;
    dip->size  = ip->size;
    memmove(dip->addrs, ip->addrs, sizeof(ip->addrs));
    log_write(bp);
    brelse(bp);
}

// Find the inode with number inum on device dev
// and return the in-memory copy. Does not lock
// the inode and does not read it from disk.
// 获取设备 dev 上编号为 inum 的内存 inode，不锁定，不从磁盘读取。
static struct inode* iget(uint dev, uint inum)
{
    struct inode *ip, *empty;

    acquire(&itable.lock);

    // Is the inode already in the table?
    empty = 0;
    for (ip = &itable.inode[0]; ip < &itable.inode[NINODE]; ip++)
    {
        if (ip->ref > 0 && ip->dev == dev && ip->inum == inum)
        {
            ip->ref++;
            release(&itable.lock);
            return ip;
        }
        if (empty == 0 && ip->ref == 0)   // Remember empty slot.
            empty = ip;
    }

    // Recycle an inode entry.
    if (empty == 0)
        panic("iget: no inodes");

    ip        = empty;
    ip->dev   = dev;
    ip->inum  = inum;
    ip->ref   = 1;
    ip->valid = 0;
    release(&itable.lock);

    return ip;
}

// Increment reference count for ip.
// Returns ip to enable ip = idup(ip1) idiom.
// 增加 inode 的引用计数，返回 inode。
struct inode* idup(struct inode* ip)
{
    acquire(&itable.lock);
    ip->ref++;
    release(&itable.lock);
    return ip;
}

// Lock the given inode.
// Reads the inode from disk if necessary.
// 锁定 inode，必要时从磁盘读取数据。
void ilock(struct inode* ip)
{
    struct buf*    bp;
    struct dinode* dip;

    if (ip == 0 || ip->ref < 1)
        panic("ilock");

    acquiresleep(&ip->lock);

    if (ip->valid == 0)
    {
        bp        = bread(ip->dev, IBLOCK(ip->inum, sb));
        dip       = (struct dinode*)bp->data + ip->inum % IPB;
        ip->type  = dip->type;
        ip->major = dip->major;
        ip->minor = dip->minor;
        ip->nlink = dip->nlink;
        ip->size  = dip->size;
        memmove(ip->addrs, dip->addrs, sizeof(ip->addrs));
        brelse(bp);
        ip->valid = 1;
        if (ip->type == 0)
            panic("ilock: no type");
    }
}

// Unlock the given inode.
// 解锁 inode
void iunlock(struct inode* ip)
{
    if (ip == 0 || !holdingsleep(&ip->lock) || ip->ref < 1)
        panic("iunlock");

    releasesleep(&ip->lock);
}

// Drop a reference to an in-memory inode.
// If that was the last reference, the inode table entry can
// be recycled.
// If that was the last reference and the inode has no links
// to it, free the inode (and its content) on disk.
// All calls to iput() must be inside a transaction in
// case it has to free the inode.
// 减少 inode 引用计数，若无引用且无链接，释放 inode。
void iput(struct inode* ip)
{
    acquire(&itable.lock);

    if (ip->ref == 1 && ip->valid && ip->nlink == 0)
    {
        // inode has no links and no other references: truncate and free.

        // ip->ref == 1 means no other process can have ip locked,
        // so this acquiresleep() won't block (or deadlock).
        acquiresleep(&ip->lock);

        release(&itable.lock);

        itrunc(ip);
        ip->type = 0;
        iupdate(ip);
        ip->valid = 0;

        releasesleep(&ip->lock);

        acquire(&itable.lock);
    }

    ip->ref--;
    release(&itable.lock);
}

// Common idiom: unlock, then put.
// 解锁 inode 并减少引用计数。
void iunlockput(struct inode* ip)
{
    iunlock(ip);
    iput(ip);
}

// Inode content
//
// The content (data) associated with each inode is stored
// in blocks on the disk. The first NDIRECT block numbers
// are listed in ip->addrs[].  The next NINDIRECT blocks are
// listed in block ip->addrs[NDIRECT].

// Return the disk block address of the nth block in inode ip.
// If there is no such block, bmap allocates one.
// returns 0 if out of disk space.
// 返回 inode ip 第 bn 个逻辑块的磁盘块号，必要时分配新块。
static uint bmap(struct inode* ip, uint bn)
{
    uint        addr, *a;
    struct buf* bp;

    if (bn < NDIRECT)
    {
        if ((addr = ip->addrs[bn]) == 0)
        {
            addr = balloc(ip->dev);
            if (addr == 0)
                return 0;
            ip->addrs[bn] = addr;
        }
        return addr;
    }
    bn -= NDIRECT;

    if (bn < NINDIRECT)
    {
        // Load indirect block, allocating if necessary.
        if ((addr = ip->addrs[NDIRECT]) == 0)
        {
            addr = balloc(ip->dev);
            if (addr == 0)
                return 0;
            ip->addrs[NDIRECT] = addr;
        }
        bp = bread(ip->dev, addr);
        a  = (uint*)bp->data;
        if ((addr = a[bn]) == 0)
        {
            addr = balloc(ip->dev);
            if (addr)
            {
                a[bn] = addr;
                log_write(bp);
            }
        }
        brelse(bp);
        return addr;
    }

    panic("bmap: out of range");
}

// Truncate inode (discard contents).
// Caller must hold ip->lock.
// 释放 inode ip 的所有数据块，清零大小。
void itrunc(struct inode* ip)
{
    int         i, j;
    struct buf* bp;
    uint*       a;

    for (i = 0; i < NDIRECT; i++)
    {
        if (ip->addrs[i])
        {
            bfree(ip->dev, ip->addrs[i]);
            ip->addrs[i] = 0;
        }
    }

    if (ip->addrs[NDIRECT])
    {
        bp = bread(ip->dev, ip->addrs[NDIRECT]);
        a  = (uint*)bp->data;
        for (j = 0; j < NINDIRECT; j++)
        {
            if (a[j])
                bfree(ip->dev, a[j]);
        }
        brelse(bp);
        bfree(ip->dev, ip->addrs[NDIRECT]);
        ip->addrs[NDIRECT] = 0;
    }

    ip->size = 0;
    iupdate(ip);
}

// Copy stat information from inode.
// Caller must hold ip->lock.
// 将 inode ip 的元数据复制到 struct stat（stat.h 定义）。
void stati(struct inode* ip, struct stat* st)
{
    st->dev   = ip->dev;
    st->ino   = ip->inum;
    st->type  = ip->type;
    st->nlink = ip->nlink;
    st->size  = ip->size;
}

// Read data from inode.
// Caller must hold ip->lock.
// If user_dst==1, then dst is a user virtual address;
// otherwise, dst is a kernel address.
// 从文件系统的索引节点（inode）读取数据到指定目标地址
int readi(struct inode* ip, int user_dst, uint64 dst, uint off, uint n)
{
    // tot：记录已读取的字节数。
    // m：每次循环读取的字节数（块内有效数据）。
    // bp：磁盘缓冲区（struct buf），用于读取磁盘块。
    uint        tot, m;
    struct buf* bp;

    if (off > ip->size || off + n < off)
        return 0;
    if (off + n > ip->size)
        n = ip->size - off;

    for (tot = 0; tot < n; tot += m, off += m, dst += m)
    {
        uint addr = bmap(ip, off / BSIZE);
        if (addr == 0)
            break;
        bp = bread(ip->dev, addr);
        m  = min(n - tot, BSIZE - off % BSIZE);
        if (either_copyout(user_dst, dst, bp->data + (off % BSIZE), m) == -1)
        {
            brelse(bp);
            tot = -1;
            break;
        }
        brelse(bp);
    }
    return tot;
}

// Write data to inode.
// Caller must hold ip->lock.
// If user_src==1, then src is a user virtual address;
// otherwise, src is a kernel address.
// Returns the number of bytes successfully written.
// If the return value is less than the requested n,
// there was an error of some kind.
// 用于将数据写入文件系统的索引节点（inode）
int writei(struct inode* ip, int user_src, uint64 src, uint off, uint n)
{
    uint        tot, m;
    struct buf* bp;

    if (off > ip->size || off + n < off)
        return -1;
    if (off + n > MAXFILE * BSIZE)
        return -1;

    for (tot = 0; tot < n; tot += m, off += m, src += m)
    {
        uint addr = bmap(ip, off / BSIZE);
        if (addr == 0)
            break;
        bp = bread(ip->dev, addr);
        m  = min(n - tot, BSIZE - off % BSIZE);
        if (either_copyin(bp->data + (off % BSIZE), user_src, src, m) == -1)
        {
            brelse(bp);
            break;
        }
        log_write(bp);
        brelse(bp);
    }

    if (off > ip->size)
        ip->size = off;

    // write the i-node back to disk even if the size didn't change
    // because the loop above might have called bmap() and added a new
    // block to ip->addrs[].
    iupdate(ip);

    return tot;
}

// Directories
// 比较两个文件名（最大 DIRSIZ = 14，fs.h 定义）。
int namecmp(const char* s, const char* t)
{
    return strncmp(s, t, DIRSIZ);
}

// Look for a directory entry in a directory.
// If found, set *poff to byte offset of entry.
// 用于在目录中查找指定文件/子目录，在给定的目录 inode 中搜索匹配的文件名
struct inode* dirlookup(struct inode* dp, char* name, uint* poff)
{
    uint          off, inum;
    struct dirent de;

    if (dp->type != T_DIR)
        panic("dirlookup not DIR");
    // 从偏移量 0 开始遍历整个目录文件（每次步进一个目录项大小 sizeof(de)）
    for (off = 0; off < dp->size; off += sizeof(de))
    {
        // readi 从目录 inode 读取一个目录项（struct dirent）到 de 结构体
        if (readi(dp, 0, (uint64)&de, off, sizeof(de)) != sizeof(de))
            panic("dirlookup read");
        if (de.inum == 0)
            continue;
        if (namecmp(name, de.name) == 0)
        {
            // entry matches path element
            if (poff)
                *poff = off;
            inum = de.inum;
            // 通过 iget() 获取并返回目标文件的 inode
            return iget(dp->dev, inum);
        }
    }
    return 0;
}

// 在目录 dp 中创建名为 name 的目录项，指向 inum，成功返回 0，失败返回 -1。
int dirlink(struct inode* dp, char* name, uint inum)
{
    int           off;
    struct dirent de;
    struct inode* ip;

    // 使用 dirlookup 检查目录中是否已存在同名文件
    if ((ip = dirlookup(dp, name, 0)) != 0)
    {
        iput(ip);
        return -1;
    }

    // 寻找空闲目录槽位
    for (off = 0; off < dp->size; off += sizeof(de))
    {
        if (readi(dp, 0, (uint64)&de, off, sizeof(de)) != sizeof(de))
            panic("dirlink read");
        if (de.inum == 0)
            break; 
    }

    strncpy(de.name, name, DIRSIZ);
    de.inum = inum;
    if (writei(dp, 0, (uint64)&de, off, sizeof(de)) != sizeof(de))
        return -1;

    return 0;
}

// Paths

// Copy the next path element from path into name.
// Return a pointer to the element following the copied one.
// The returned path has no leading slashes,
// so the caller can check *path=='\0' to see if the name is the last one.
// If no name to remove, return 0.
//
// Examples:
//   skipelem("a/bb/c", name) = "bb/c", setting name = "a"
//   skipelem("///a//bb", name) = "bb", setting name = "a"
//   skipelem("a", name) = "", setting name = "a"
//   skipelem("", name) = skipelem("////", name) = 0
// 用于从路径字符串中提取下一个路径分量（路径元素）
static char* skipelem(char* path, char* name)
{
    char* s;
    int   len;
    // 跳过所有连续的 /（如 ///a/b → a/b）
    while (*path == '/')
        path++;
    // 如果遇到字符串结束符 \0，说明没有更多路径分量，返回 NULL
    if (*path == 0)
        return 0;
    // 定位当前分量起止点，扫描直到遇到下一个 / 或字符串结尾
    s = path;
    while (*path != '/' && *path != 0)
        path++;
    // 计算分量长度
    len = path - s;
    // 截断到 DIRSIZ 长度（不添加终止符）
    if (len >= DIRSIZ)
        memmove(name, s, DIRSIZ);
    else
    {
        memmove(name, s, len);
        name[len] = 0;
    }
    // 跳过后续斜杠
    while (*path == '/')
        path++;
    return path;
}

// 将文件路径解析为对应的 inode
static struct inode* namex(char* path, int nameiparent, char* name)
{
    struct inode *ip, *next;

    if (*path == '/')
        ip = iget(ROOTDEV, ROOTINO);
    else
        ip = idup(myproc()->cwd);

    while ((path = skipelem(path, name)) != 0)
    {
        ilock(ip);
        if (ip->type != T_DIR)
        {
            iunlockput(ip);
            return 0;
        }
        if (nameiparent && *path == '\0')
        {
            // Stop one level early.
            iunlock(ip);
            return ip;
        }
        if ((next = dirlookup(ip, name, 0)) == 0)
        {
            iunlockput(ip);
            return 0;
        }
        iunlockput(ip);
        ip = next;
    }
    if (nameiparent)
    {
        iput(ip);
        return 0;
    }
    return ip;
}

// 常规路径解析
// 示例：namei("/a/b") → 返回 b 的 inode
struct inode* namei(char* path)
{
    char name[DIRSIZ];
    return namex(path, 0, name);
}

// 获取父目录
// nameiparent("/a/b", name) → 返回 a/ 的 inode，设置 name = "b"
struct inode* nameiparent(char* path, char* name)
{
    return namex(path, 1, name);
}
