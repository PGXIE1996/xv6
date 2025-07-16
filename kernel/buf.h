// 管理文件系统或块设备的缓冲区，用于处理从磁盘读取或写入的数据块
struct buf
{
    int  valid;              // 表示缓冲区是否包含从磁盘读取的有效数据。
    int  disk;               // 表示磁盘是否“拥有”该缓冲区。
    uint dev;                // 表示设备编号，用于标识缓冲区关联的磁盘设备。
    uint blockno;            // 表示缓冲区对应的磁盘块编号，指定该缓冲区存储的是磁盘上的哪个数据块。
    struct sleeplock lock;   // 一个睡眠锁（sleeplock），用于同步访问缓冲区。
    uint             refcnt;        // 引用计数，记录当前有多少进程或线程正在使用该缓冲区。
    struct buf*      prev;          // 指向实现LRU缓存的双向链表结构，帮助跟踪缓冲区的使用顺序。
    struct buf*      next;          // 指向下一个缓冲区的指针，同样用于LRU缓存链表。
    uchar            data[BSIZE];   // 实际存储数据的数组，BSIZE是块大小
};
