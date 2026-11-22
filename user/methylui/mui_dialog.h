#ifndef METHYLUI_DIALOG_H
#define METHYLUI_DIALOG_H

#include "methylui/mui_surface.h"
#include "methylui/mui_textbox.h"

#define MUI_DIALOG_MODE_RENAME 0
#define MUI_DIALOG_MODE_TEXT   1

/* 轻量模态对话框：直接画在宿主 surface 上（不创建 WM 窗口）。
 * 当前提供 Rename 对话框：
 *   - mui_dialog_open_rename() 预填旧文件名
 *   - 宿主把 WM_EV_KEY 转给 mui_dialog_key()
 *   - Enter 在 MUI 内部完成路径拼接、存在性检查和 fs_rename()
 *   - Esc 取消
 * 宿主只需根据 active/result 决定刷新。
 */
struct mui_dialog {
    int active;
    int mode;                   /* MUI_DIALOG_MODE_* */
    int x, y, w, h;
    int result;                 /* 0 打开中；1 重命名成功；2 文本输入成功；-1 取消 */

    char title[32];
    char hint[64];
    char dir[256];
    char old_name[MUI_TEXTBOX_MAX_LEN + 1];
    char result_text[MUI_TEXTBOX_MAX_LEN + 1];
    char error[64];

    /* Esc 与方向键序列（ESC [ A/B/C/D）区分 */
    int esc_state;              /* 0 无；1 已收到 ESC；2 已收到 ESC [ */
    uint32_t esc_tick;

    struct mui_textbox input;
};

void mui_dialog_init(struct mui_dialog *d);
void mui_dialog_open_rename(struct mui_dialog *d, const char *dir,
                            const char *old_name);

/* 通用单行文本输入（用于自定义图标路径等）。成功后 result == 2，
 * 文本在 result_text 中。 */
void mui_dialog_open_text(struct mui_dialog *d, const char *title,
                          const char *hint, const char *initial);
void mui_dialog_close(struct mui_dialog *d);

/* 返回 1 表示按键被对话框消费。now 为单调毫秒计数（宿主 g_tick）。 */
int  mui_dialog_key(struct mui_dialog *d, int key, uint32_t now);

/* 宿主每轮调用：单独按 Esc 时，超过约 60ms 没有后续序列则取消。 */
int  mui_dialog_tick(struct mui_dialog *d, uint32_t now);

/* 在 surface 中央绘制。返回 1 表示画了。 */
int  mui_dialog_paint(struct mui_dialog *d, struct mui_surface *s,
                      int host_w, int host_h);

/* 点是否落在对话框内。 */
int  mui_dialog_hit(struct mui_dialog *d, int x, int y);

#endif
