#ifndef VOLUME_H
#define VOLUME_H

#include <stdint.h>

#define VOL_NAME_LEN    8      /* 卷名最长 7 字符 + '\0' */
#define VOL_MAX         8      /* 最多 8 个卷 */

/* 文件系统类型 */
#define VOL_FS_NONE     0
#define VOL_FS_FAT32    1
#define VOL_FS_RAMFS    2      /* 预留 */
#define VOL_FS_ISO9660  3      /* 预留 */

struct volume {
    char     name[VOL_NAME_LEN];  /* "SYS", "DATA", "TMP" */
    uint8_t  used;
    uint8_t  fs_type;             /* VOL_FS_* */
    uint32_t start_lba;           /* 分区起始 LBA */
    uint32_t sector_count;        /* 分区扇区数 */
    void    *fs_private;          /* 具体文件系统的上下文（FAT32 BPB 等） */
};

/* 目录项（所有文件系统通用） */
#define DIRENT_NAME_MAX  256
struct dirent {
    char     name[DIRENT_NAME_MAX];
    uint8_t  attributes;         /* bit4 = 目录 */
    uint8_t  reserved[3];
    uint32_t size;
};

/* 初始化卷子系统 */
void volume_init(void);

/* 注册一个卷，成功返回 0，失败返回负错误码 */
int volume_register(const char *name, uint8_t fs_type,
                    uint32_t start_lba, uint32_t sector_count,
                    void *fs_private);

/* 按名字查找卷，失败返回 NULL */
struct volume *volume_lookup(const char *name);

/* 列出所有已注册的卷，返回实际数量 */
int volume_list(struct volume **out, int max);

/* 获取默认卷（SYS:），失败返回 NULL */
struct volume *volume_get_default(void);

#endif