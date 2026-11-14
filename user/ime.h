#ifndef IME_H
#define IME_H

#include <stdint.h>
#include "methylui/mui_textbox.h"
#include "ime_dict.h"

struct ime_ctx {
    int active;          /* 0 = 英文直通, 1 = 拼音输入 */
    int pinyin_len;
    char pinyin[32];
    int show_hint;       /* 是否显示候选框 */
};

void ime_init(struct ime_ctx *ctx);

/* 上屏回调：用于终端等非 mui_textbox 环境 */
typedef void (*ime_commit_cb)(const char *utf8, void *userdata);

/* 
 * 如果指定了 target，结果插入 target；
 * 如果指定了 commit_cb，结果通过回调传出（两者可同时为 NULL，但不会上屏）。
 */
int ime_process_key(struct ime_ctx *ctx, int key, unsigned int mods, 
                    struct mui_textbox *target, 
                    ime_commit_cb commit_cb, void *userdata);

const char* ime_get_hint(struct ime_ctx *ctx);

#endif