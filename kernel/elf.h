// Format of an ELF executable file

#define ELF_MAGIC 0x464C457FU   // "\x7FELF" in little endian

// File header
struct elfhdr
{
    uint   magic;     // 魔数，用于验证ELF文件格式，必须等于ELF_MAGIC（0x464C457F）。
    uchar  elf[12];   // ELF 标识信息（位宽/字节序/版本等）
    ushort type;      // 文件类型（1=可重定位, 2=可执行, 3=共享库）
    ushort machine;   // 目标架构（0xF3=RISC-V）
    uint   version;   // ELF文件版本（通常为1）
    uint64 entry;     // 程序入口点虚拟地址
    uint64 phoff;     // 程序头表（Program Header Table）的文件偏移量
    uint64 shoff;     // 节头表（Section Header Table）的文件偏移量
    uint   flags;     // 处理器特定标志
    ushort ehsize;    // 文件头的大小（字节），通常为64（64位ELF）或52（32位ELF）
    ushort phentsize;   // 程序头表中每个条目的大小（字节），通常为56（64位ELF）
    ushort phnum;       // 程序头表中的条目数，表示有多少程序段需要加载
    ushort shentsize;   // 节头表中每个条目的大小
    ushort shnum;       // 节头表中的条目数
    ushort shstrndx;    // 节头表中字符串表（section name string table）的索引
};

// ELF程序头（Program Header）的结构
struct proghdr
{
    uint32 type;     // 段类型（1=可加载段）
    uint32 flags;    // 段标志（读/写/执行权限）
    uint64 off;      // 段在文件中的偏移量
    uint64 vaddr;    // 段在内存中的虚拟地址
    uint64 paddr;    // 物理地址（通常与 vaddr 相同）
    uint64 filesz;   // 段在文件中的大小（字节），即需要从文件中读取的数据量。
    uint64 memsz;    // 段在内存中的大小（字节），可能大于filesz，差值部分填充为0
    uint64 align;    // 段的对齐要求（通常是页面大小，如4KB），确保vaddr和off满足对齐约束
};

// 定义程序头类型，表示该段需要加载到内存
#define ELF_PROG_LOAD 1

// 定义程序段的权限标志：
#define ELF_PROG_FLAG_EXEC  1
#define ELF_PROG_FLAG_WRITE 2
#define ELF_PROG_FLAG_READ  4
