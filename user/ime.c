#include "ime.h"
#include "syscall.h"
#include "stdio.h"
#include "string.h"
#include "wm_proto.h"

void ime_init(struct ime_ctx *ctx) {
    ctx->active = 0;
    ctx->pinyin_len = 0;
    ctx->pinyin[0] = '\0';
    ctx->show_hint = 0;
}

/* 构建候选词提示（利用外部传入的 buf，避免使用静态全局变量） */
static void ime_format_hint(struct ime_ctx *ctx, char *out, int size) {
    if (!ctx->show_hint || !ctx->active) {
        out[0] = '\0';
        return;
    }
    
    if (ctx->pinyin_len == 0) {
        snprintf(out, size, "IME: 拼音输入");
        return;
    }
    
    int n = snprintf(out, size, "IME: %s", ctx->pinyin);
    const struct ime_word *word = ime_lookup_pinyin(ctx->pinyin);
    if (word) {
        for (int i = 0; i < 3 && word->cand[i]; i++) {
            if (n >= size - 1) break;
            n += snprintf(out + n, size - n, " %d:%s", i + 1, word->cand[i]);
        }
    }
}

const char* ime_get_hint(struct ime_ctx *ctx) {
    static char hint_buf[128];
    ime_format_hint(ctx, hint_buf, sizeof(hint_buf));
    return hint_buf;
}

/* 受保护的安全插入，防止 MUI 内部对多字节字符越界处理导致崩溃 */
static void safe_textbox_insert(struct mui_textbox *tb, const char *s) {
    if (!tb || !s) return;
    int slen = strlen(s);
    if (slen == 0) return;

    /* 防止长度溢出，假设 MUI 文本框最大长度宏定义在头文件中 */
    if (tb->len + slen >= MUI_TEXTBOX_MAX_LEN) return;

    int tail = tb->len - tb->cursor;
    if (tail > 0) {
        memmove(tb->text + tb->cursor + slen, tb->text + tb->cursor, tail);
    }
    memcpy(tb->text + tb->cursor, s, slen);
    tb->len += slen;
    tb->cursor += slen;
    tb->text[tb->len] = '\0';
    tb->base.dirty = 1;
}

int ime_process_key(struct ime_ctx *ctx, int key, unsigned int mods, 
                    struct mui_textbox *target, 
                    ime_commit_cb commit_cb, void *userdata) {
    if (key == ' ' && (mods & WM_MOD_CTRL)) {
        if (!target && !commit_cb) return 0;
        ctx->active = !ctx->active;
        ctx->pinyin_len = 0;
        ctx->pinyin[0] = '\0';
        ctx->show_hint = ctx->active;
        return 1;
    }

    if (!ctx->active) return 0;

    /* 字母键转为小写进入拼音缓冲 */
    if (key >= 'A' && key <= 'Z') key = key - 'A' + 'a';
    if (key >= 'a' && key <= 'z') {
        if (ctx->pinyin_len < (int)sizeof(ctx->pinyin) - 1) {
            ctx->pinyin[ctx->pinyin_len++] = (char)key;
            ctx->pinyin[ctx->pinyin_len] = '\0';
        }
        ctx->show_hint = 1;
        return 1;
    }

    /* 退格删拼音，如果没拼音了不拦截，交由文本框处理 */
    if (key == '\b' || key == 127) {
        if (ctx->pinyin_len == 0) return 0;
        ctx->pinyin[--ctx->pinyin_len] = '\0';
        ctx->show_hint = 1;
        return 1;
    }

    /* 空格或回车：上屏拼音 */
    if (key == ' ' || key == '\r' || key == '\n') {
        if (ctx->pinyin_len == 0) return 0;
        const struct ime_word *word = ime_lookup_pinyin(ctx->pinyin);
        if (word && word->cand[0]) {
            /* 改用安全插入 */
            if (target) safe_textbox_insert(target, word->cand[0]);
            if (commit_cb) commit_cb(word->cand[0], userdata);
            ctx->pinyin_len = 0;
            ctx->pinyin[0] = '\0';
            ctx->show_hint = 0;
        }
        return 1;
    }

    /* 数字键选词 */
    if (key >= '1' && key <= '9') {
        if (ctx->pinyin_len == 0) return 0;
        const struct ime_word *word = ime_lookup_pinyin(ctx->pinyin);
        int pick = key - '1';
        if (word && pick < 5 && word->cand[pick]) {
            /* 改用安全插入 */
            if (target) safe_textbox_insert(target, word->cand[pick]);
            if (commit_cb) commit_cb(word->cand[pick], userdata);
            ctx->pinyin_len = 0;
            ctx->pinyin[0] = '\0';
            ctx->show_hint = 0;
        }
        return 1;
    }

    if (key == 27) {
        if (!ctx->pinyin_len && !ctx->show_hint) return 0;
        ctx->pinyin_len = 0;
        ctx->pinyin[0] = '\0';
        ctx->show_hint = 0;
        return 1;
    }

    return 0;
}