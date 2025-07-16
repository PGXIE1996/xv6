// 表示一个打开的文件（或管道、设备），用于管理文件描述符的状态。
struct file
{
    enum
    {
        FD_NONE,
        FD_PIPE,
        FD_INODE,
        FD_DEVICE
    } type;              // 文件类型
    int           ref;   // 引用计数
    char          readable;
    char          writable;
    struct pipe*  pipe;    // 指向管道结构（定义在 pipe.h），仅对 FD_PIPE 有效。
    struct inode* ip;      // 指向内存中的 inode
    uint          off;     // 文件偏移量，仅对 FD_INODE 有效，记录读写位置。
    short         major;   // 主设备号，仅对 FD_DEVICE 有效，用于标识设备类型。
};

#define major(dev)  ((dev) >> 16 & 0xFFFF)      // 从设备号 dev 提取主设备号（高 16 位）
#define minor(dev)  ((dev) & 0xFFFF)            // 从设备号 dev 提取次设备号（低 16 位）
#define mkdev(m, n) ((uint)((m) << 16 | (n)))   // 根据主设备号 m 和次设备号 n 构造设备号

// in-memory copy of an inode
// 表示内存中的 inode，缓存磁盘上的 struct dinode（fs.h 定义），并附加管理字段。
struct inode
{
    uint             dev;     // 设备号，标识 inode 所在的设备
    uint             inum;    // inode 编号，唯一标识文件或目录
    int              ref;     // 引用计数，记录 inode 被多少文件描述符或目录引用
    struct sleeplock lock;    // 睡眠锁，保护以下字段的并发访问
    int              valid;   // 布尔值，1 表示 inode 已从磁盘读取，0 表示未初始化

    // 直接复制磁盘上的 struct dinode
    short type;
    short major;
    short minor;
    short nlink;
    uint  size;
    uint  addrs[NDIRECT + 1];
};

// map major device number to device functions.
// 定义设备驱动的功能表，映射主设备号到读写函数。
struct devsw
{
    int (*read)(int, uint64, int);
    int (*write)(int, uint64, int);
};

extern struct devsw devsw[];

// 定义控制台设备的主设备号为 1
#define CONSOLE 1
