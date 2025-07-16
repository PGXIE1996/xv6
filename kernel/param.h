#define NPROC       64                  // 最大进程数量
#define NCPU        8                   // 最大CPU数
#define NOFILE      16                  // 每个进程打开的文件数
#define NFILE       100                 // 系统打开文件数
#define NINODE      50                  // 最大活动的inode数
#define NDEV        10                  // 最大设备数
#define ROOTDEV     1                   // 文件系统根设备数量
#define MAXARG      32                  // 最大exec参数
#define MAXOPBLOCKS 10                  // 系统调用最大操作磁盘块数
#define LOGSIZE     (MAXOPBLOCKS * 3)   // 最大磁盘日志块
#define NBUF        (MAXOPBLOCKS * 3)   // 缓冲层数据块
#define FSSIZE      2000                // 文件系统最大块数
#define MAXPATH     128                 // 路径最长名字