#ifndef PATH_H
#define PATH_H

#include <volume.h>

#define PATH_MAX_LEN    256

/* 解析结果 */
struct resolved_path {
    struct volume *vol;              /* 目标卷 */
    char           path[PATH_MAX_LEN]; /* 卷内绝对路径，以 '/' 开头 */
};

/* 解析路径（考虑 cwd 与相对路径），成功返回 0，失败返回负错误码 */
int resolve_path(const char *input, struct resolved_path *out);

/* 原地规范化：把 "/A/../B/./C" 变成 "/B/C"
 * 会处理 . / .. / 多重斜杠
 * 不允许跨越卷根（.. 到根时停止）
 */
void normalize_path(char *path);

/* 从完整路径中拆出卷名和剩余部分
 * 输入 "SYS:/BOOT/FOO"，输出 vol_name="SYS", inner="/BOOT/FOO"
 * 返回 0 表示成功，-1 表示不是绝对路径
 */
int split_volume_prefix(const char *input, char *vol_name, int name_size,
                        const char **inner);

#endif