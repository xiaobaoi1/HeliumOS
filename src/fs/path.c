#include <path.h>
#include <task.h>
#include <errno.h>
#include <string.h>
#include <printf.h>
#include <stddef.h>

/* 判断字符是否合法的路径分隔符 */
static inline int is_sep(char c) {
    return c == '/' || c == '\\';
}

/*
 * 拆分卷名前缀
 * 输入格式：<name>:<rest>
 * 例如 "SYS:/BOOT/FOO" → name="SYS", rest="/BOOT/FOO"
 */
int split_volume_prefix(const char *input, char *vol_name, int name_size,
                        const char **inner) {
    if (!input || !vol_name || !inner) return -1;

    int i = 0;
    while (input[i] && input[i] != ':' && i < name_size - 1) {
        vol_name[i] = input[i];
        i++;
    }

    if (input[i] != ':') return -1;      /* 没有冒号，不是绝对路径 */
    vol_name[i] = '\0';

    *inner = input + i + 1;              /* 跳过 ':' */
    return 0;
}

/*
 * 原地规范化路径
 * 处理 . / .. / 多重斜杠，不允许跨越卷根
 * 输入输出都是同一个 buffer
 */
void normalize_path(char *path) {
    if (!path) return;

    /* 分割成段 */
    char segments[32][64];      /* 最多 32 层，每层 63 字符 */
    int  depth = 0;

    int i = 0;
    while (path[i]) {
        /* 跳过所有分隔符 */
        while (path[i] && is_sep(path[i])) i++;
        if (!path[i]) break;

        /* 取一个段 */
        char tok[64];
        int  t = 0;
        while (path[i] && !is_sep(path[i]) && t < 63) {
            tok[t++] = path[i++];
        }
        tok[t] = '\0';

        if (strcmp(tok, ".") == 0) {
            /* 忽略 */
        } else if (strcmp(tok, "..") == 0) {
            if (depth > 0) depth--;
            /* 已经在根，保持在根 */
        } else {
            if (depth < 32) {
                /* 拷贝到 segments[depth] */
                int j;
                for (j = 0; tok[j] && j < 63; j++) {
                    segments[depth][j] = tok[j];
                }
                segments[depth][j] = '\0';
                depth++;
            }
        }
    }

    /* 重新拼装 */
    char result[PATH_MAX_LEN];
    int  r = 0;
    if (depth == 0) {
        result[r++] = '/';
    } else {
        for (int k = 0; k < depth; k++) {
            result[r++] = '/';
            for (int j = 0; segments[k][j] && r < PATH_MAX_LEN - 1; j++) {
                result[r++] = segments[k][j];
            }
        }
    }
    result[r] = '\0';

    /* 拷回 path */
    for (int k = 0; k <= r; k++) {
        path[k] = result[k];
    }
}

/*
 * 解析路径
 * - 如果输入是 "NAME:..." 形式的绝对路径，直接使用
 * - 否则按当前进程的 cwd 拼接成绝对路径
 * - 最后做一次规范化
 */
int resolve_path(const char *input, struct resolved_path *out) {
    if (!input || !out) return EINVAL;

    char vol_name[VOL_NAME_LEN];
    const char *inner;

    if (split_volume_prefix(input, vol_name, sizeof(vol_name), &inner) == 0) {
        /* 情况 1：绝对路径 */
        struct volume *v = volume_lookup(vol_name);
        if (!v) return ENOENT;

        out->vol = v;
        /* 拷贝卷内路径，截断保护 */
        int i = 0;
        while (inner[i] && i < PATH_MAX_LEN - 1) {
            out->path[i] = inner[i];
            i++;
        }
        out->path[i] = '\0';

        /* 如果内部路径不是以 '/' 开头，补一个 */
        if (out->path[0] != '/') {
            for (int j = i; j >= 0; j--) {
                out->path[j+1] = out->path[j];
            }
            out->path[0] = '/';
        }
    } else {
        /* 情况 2：相对路径，基于 cwd */
        struct task *cur = get_current_task();

        struct volume *v;
        const char *base_path;

        if (cur && cur->cwd_volume[0] != '\0') {
            /* 有 cwd，按 cwd 拼接 */
            v = volume_lookup(cur->cwd_volume);
            if (!v) return ENOENT;
            base_path = cur->cwd_path;
        } else {
            /* 没有 cwd（内核早期 / 未设置），回退到默认卷根 */
            v = volume_get_default();
            if (!v) return ENOENT;
            base_path = "/";
        }

        out->vol = v;

        /* 拼接 base_path + '/' + input */
        int len = 0;
        for (int i = 0; base_path[i] && len < PATH_MAX_LEN - 1; i++) {
            out->path[len++] = base_path[i];
        }
        if (len == 0 || out->path[len-1] != '/') {
            if (len < PATH_MAX_LEN - 1) out->path[len++] = '/';
        }
        for (int i = 0; input[i] && len < PATH_MAX_LEN - 1; i++) {
            out->path[len++] = input[i];
        }
        out->path[len] = '\0';
    }

    /* 规范化（处理 . / .. / //） */
    normalize_path(out->path);
    return OK;
}