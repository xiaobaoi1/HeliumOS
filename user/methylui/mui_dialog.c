#include "methylui/mui_dialog.h"
#include "methylui/mui_clip.h"
#include "syscall.h"
#include "string.h"
#include "stdio.h"

#define DIALOG_BG     0x1E1E2E
#define DIALOG_BORDER 0x6A8AC0
#define DIALOG_TITLE  0x2A2A3E
#define DIALOG_TEXT   0xFFFFFF
#define DIALOG_DIM    0x8888AA
#define DIALOG_ERROR  0xFF6666

static void dialog_join(const char *dir, const char *name, char *out, int out_size) {
    int n = dir ? (int)strlen(dir) : 0;
    if (n > 0 && dir[n - 1] == '\\')
        snprintf(out, out_size, "%s%s", dir, name);
    else
        snprintf(out, out_size, "%s\\%s", dir ? dir : "", name);
}

/* 只允许文件名本身，不允许路径分隔符/FAT 非法字符 */
static int dialog_name_valid(const char *name) {
    if (!name || !name[0]) return 0;
    if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) return 0;
    for (const char *p = name; *p; p++) {
        char c = *p;
        if (c == '\\' || c == '/' || c == ':' || c == '*' || c == '?' ||
            c == '"' || c == '<' || c == '>' || c == '|')
            return 0;
    }
    return 1;
}

static int dialog_exists(const char *path) {
    int fd = fs_open(path, FS_O_RDONLY);
    if (fd >= 0) { fs_close(fd); return 1; }
    int dd = fs_opendir(path);
    if (dd >= 0) { fs_closedir(dd); return 1; }
    return 0;
}

void mui_dialog_init(struct mui_dialog *d) {
    if (!d) return;
    memset(d, 0, sizeof(*d));
    strcpy(d->title, "Rename");
    mui_textbox_init(&d->input, 0, 0, 320);
    d->input.base.focused = 0;
}

void mui_dialog_open_rename(struct mui_dialog *d, const char *dir,
                            const char *old_name) {
    if (!d) return;
    strncpy(d->dir, dir ? dir : "", sizeof(d->dir) - 1);
    d->dir[sizeof(d->dir) - 1] = '\0';
    strncpy(d->old_name, old_name ? old_name : "", sizeof(d->old_name) - 1);
    d->old_name[sizeof(d->old_name) - 1] = '\0';

    strcpy(d->title, "Rename");
    d->error[0] = '\0';
    d->result = 0;
    d->active = 1;
    d->esc_state = 0;
    d->esc_tick = 0;

    mui_textbox_set_text(&d->input, d->old_name);
    d->input.base.w = 320;
    d->input.base.h = MUI_FONT_H + 8;
    d->input.base.focused = 1;
    d->input.base.dirty = 1;
}

void mui_dialog_close(struct mui_dialog *d) {
    if (!d) return;
    d->active = 0;
    d->esc_state = 0;
    d->input.base.focused = 0;
}

int mui_dialog_hit(struct mui_dialog *d, int x, int y) {
    if (!d || !d->active) return 0;
    return x >= d->x && x < d->x + d->w &&
           y >= d->y && y < d->y + d->h;
}

static int dialog_commit(struct mui_dialog *d) {
    char text[MUI_TEXTBOX_MAX_LEN + 1];
    strncpy(text, mui_textbox_get_text(&d->input), sizeof(text) - 1);
    text[sizeof(text) - 1] = '\0';

    int start = 0;
    while (text[start] == ' ' || text[start] == '\t') start++;
    int end = (int)strlen(text);
    while (end > start && (text[end - 1] == ' ' || text[end - 1] == '\t')) end--;

    char name[MUI_TEXTBOX_MAX_LEN + 1];
    int n = end - start;
    if (n < 0) n = 0;
    if (n > (int)sizeof(name) - 1) n = sizeof(name) - 1;
    memcpy(name, text + start, n);
    name[n] = '\0';

    if (!dialog_name_valid(name)) {
        strcpy(d->error, "Invalid file name");
        return 0;
    }

    char old_path[320], new_path[320];
    dialog_join(d->dir, d->old_name, old_path, sizeof(old_path));
    dialog_join(d->dir, name, new_path, sizeof(new_path));

    if (strcmp(old_path, new_path) == 0) {
        d->result = 1;
        mui_dialog_close(d);
        return 1;
    }
    if (dialog_exists(new_path)) {
        strcpy(d->error, "Already exists");
        return 0;
    }
    if (fs_rename(old_path, new_path) < 0) {
        strcpy(d->error, "Rename failed");
        return 0;
    }

    d->error[0] = '\0';
    d->result = 1;
    mui_dialog_close(d);
    return 1;
}

int mui_dialog_key(struct mui_dialog *d, int key, uint32_t now) {
    if (!d || !d->active) return 0;

    /* 方向键等是 ESC [ X 序列：左/右移动文本光标，Home/End 到首/尾 */
    if (d->esc_state == 2) {
        if (key == '~' || (key >= 'A' && key <= 'Z')) {
            d->esc_state = 0;
            if (key == 'D')      mui_textbox_move_cursor(&d->input, -1);
            else if (key == 'C') mui_textbox_move_cursor(&d->input, 1);
            else if (key == 'H') mui_textbox_cursor_home(&d->input);
            else if (key == 'F') mui_textbox_cursor_end(&d->input);
            d->input.base.dirty = 1;
        }
        return 1;
    }
    if (d->esc_state == 1) {
        if (key == '[') {
            d->esc_state = 2;
            return 1;
        }
        /* ESC 后面不是 '['：按 Esc 取消，并吞掉当前键 */
        d->result = -1;
        mui_dialog_close(d);
        return 1;
    }

    if (key == 27) {                     /* Esc：等 tick 判断是否单键 */
        d->esc_state = 1;
        d->esc_tick = now;
        return 1;
    }
    if (key == '\r' || key == '\n') {    /* Enter */
        dialog_commit(d);
        return 1;
    }

    if (d->input.base.on_key)
        d->input.base.on_key(&d->input.base, key);
    d->input.base.dirty = 1;
    return 1;
}

int mui_dialog_tick(struct mui_dialog *d, uint32_t now) {
    if (!d || !d->active) return 0;
    if (d->esc_state == 1 && (uint32_t)(now - d->esc_tick) >= 60) {
        d->result = -1;
        mui_dialog_close(d);
        return 1;
    }
    return 0;
}

int mui_dialog_paint(struct mui_dialog *d, struct mui_surface *s,
                     int host_w, int host_h) {
    if (!d || !d->active || !s) return 0;

    int w = 380, h = 120;
    if (w > host_w - 16) w = host_w - 16;
    if (h > host_h - 16) h = host_h - 16;
    if (w < 80) w = host_w;
    if (h < 60) h = host_h;
    if (w <= 0 || h <= 0) return 0;

    d->w = w; d->h = h;
    d->x = (host_w - w) / 2;
    d->y = (host_h - h) / 2;

    mui_clip_set(s, d->x, d->y, w, h);

    mui_fill(s, d->x, d->y, w, h, DIALOG_BG);
    mui_border(s, d->x, d->y, w, h, DIALOG_BORDER);
    mui_fill(s, d->x + 1, d->y + 1, w - 2, 22, DIALOG_TITLE);
    mui_text(s, d->x + 8, d->y + 4, d->title, DIALOG_TEXT, DIALOG_TITLE);

    int tw = w - 16;
    if (tw < MUI_FONT_W * 4) tw = MUI_FONT_W * 4;
    d->input.base.w = tw;
    d->input.base.h = MUI_FONT_H + 8;
    d->input.base.focused = 1;
    d->input.base.dirty = 1;
    mui_widget_paint(&d->input.base, s, d->x + 8, d->y + 30);

    if (d->error[0])
        mui_text(s, d->x + 8, d->y + 60, d->error, DIALOG_ERROR, DIALOG_BG);

    mui_text(s, d->x + 8, d->y + h - 22,
             "Enter = Rename    Esc = Cancel", DIALOG_DIM, DIALOG_BG);

    mui_clip_reset(s);
    return 1;
}
