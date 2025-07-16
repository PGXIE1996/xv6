// 磁盘文件系统格式：内核和用户程序都用这个头文件


#define ROOTINO 1      // ROOTINO 表示根目录的 inode 编号
#define BSIZE   1024   // 定义磁盘块的大小为 1024 字节

// Disk layout:
// [ boot block | super block | log | inode blocks | free bit map | data blocks]
// mkfs computes the super block and builds an initial file system. The
// super block describes the disk layout:
// 超级块结构
struct superblock
{
    uint magic;        // 魔数，固定为 FSMAGIC,用于验证文件系统格式
    uint size;         // 文件系统总块数
    uint nblocks;      // 数据块的数量
    uint ninodes;      // inode 的总数
    uint inodestart;   // inode 区域的起始块号
    uint nlog;         // 日志块的数量
    uint logstart;     // 日志区域的起始块号
    uint bmapstart;    // 空闲位图的起始块号
};

#define FSMAGIC 0x10203040   // 定义魔数，用于标识 xv6 文件系统，防止误识别其他文件系统格式

#define NDIRECT   12                       // 每个 inode 包含 12 个直接块地址，指向文件的数据块。
#define NINDIRECT (BSIZE / sizeof(uint))   // 定义间接块的指针数量
#define MAXFILE   (NDIRECT + NINDIRECT)    // 定义文件最大块数为 NDIRECT + NINDIRECT = 268 块

// On-disk inode structure
// struct dinode 定义了磁盘上 inode 的格式，存储文件或目录的元数据。
struct dinode
{
    short type;                 // 文件类型T_FILE、T_DIR、T_DEVICE、0（空闲 inode）
    short major;                // 主设备号 (T_DEVICE only)
    short minor;                // 次设备号 (T_DEVICE only)
    short nlink;                // inode 的硬链接计数
    uint  size;                 // 文件大小（字节）
    uint  addrs[NDIRECT + 1];   // 数据块地址数组，大小为 12 + 1 = 13
};

// 计算每个磁盘块可存储的 inode 数量（IPB=16）
#define IPB (BSIZE / sizeof(struct dinode))

// 计算给定 inode 号 i 所在的磁盘块号
#define IBLOCK(i, sb) ((i) / IPB + sb.inodestart)

// 计算每个位图块的位数（BPB=8192）
#define BPB (BSIZE * 8)

// 计算数据块 b 对应的位图块号。
#define BBLOCK(b, sb) ((b) / BPB + sb.bmapstart)

// 定义目录项中文件名的最大长度为 14 字节（不包括空终止符）。
#define DIRSIZ 14

// 定义目录项的格式，存储在目录文件的数据块中。
struct dirent
{
    ushort inum;
    char   name[DIRSIZ];
};
