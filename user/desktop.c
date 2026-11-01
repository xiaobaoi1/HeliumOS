#include "syscall.h"
#include "stdio.h"
#include "string.h"
#include "stdlib.h"
#include <stdint.h>
#include "wm_proto.h"
#include "wm_font.h"
#include "shm_layout.h"
#include "desktop_gfx.h"
#include "desktop_icons.h"
#include "desktop_wm.h"
#include "desktop_fs.h"
#include "methylui/mui_font.h"
#include "methylui/mui_surface.h"
#include "methylui/mui_clip.h"
#include "methylui/mui_list.h"
#include "methylui/mui_textbox.h"
#include "methylui/mui_scroll.h"
#include "methylui/mui_dialog.h"
#include "ime_dict.h"

#define PANEL_H     28
#define MENU_ITEM_H 24
#define MENU_PAD    4
#define MENU_W      160

#define COL_PANEL      0x2A2A3E
#define COL_PANEL_FG   0xCCCCCC
#define COL_MENU_BG    0x2A2A3E
#define COL_MENU_FG    0xFFFFFF
#define COL_MENU_HL    0x3A6EA5

#define WALL_TOP       0x303A3E
#define WALL_BOTTOM    0x102140

#define ROOT_PATH        "SYS:\\"
#define MAX_DESK_FILES   64
#define ICON_CELL_W      80
#define ICON_CELL_H      80
#define ICON_SIZE        48
#define ICON_PAD_X       16
#define ICON_PAD_Y       16

static int g_W, g_H;
int g_inbox_w = -1;
static uint32_t g_tick = 0;

static unsigned int *g_wall_pixels;
static struct shm_hdr *g_wall_hdr;
static char g_wall_shm[16];
static int g_wall_ev = -1;

static unsigned int *g_panel_pixels;
static struct shm_hdr *g_panel_hdr;
static char g_panel_shm[16];
static int g_panel_ev = -1;

static unsigned int *g_menu_pixels;
static struct shm_hdr *g_menu_hdr;
static char g_menu_shm[16];
static int g_menu_ev = -1;
static int g_menu_w, g_menu_h, g_menu_visible;

struct desktop_win {
    uint32_t id;
    uint32_t minimized;
    char     title[32];
};

static struct desktop_win g_win_list[8];
static int g_win_count = 0;

#define WIN_ITEM_X0   64
#define WIN_ITEM_W    100

struct fm_win;

static void spawn_term(int cw, int ch, int x, int y);
static void spawn_term_argv(int cw, int ch, int x, int y,
                            char *const extra[], const char *title);
static void open_source_file(const char *path);
static struct fm_win *open_folder_window(const char *path);
static void desktop_refresh_icons(void);
static void toggle_safeguard(void);
static void shm_publish(struct shm_hdr *h);
static void shm_mark_rect(struct shm_hdr *h, int x0, int y0, int x1, int y1);
static void open_context_menu(int x, int y);
static void open_context_item_menu(int file_idx, int x, int y);
/* ============================================================
 *                   桌面图标逻辑
 * ============================================================ */

static struct fs_entry_info g_files[MAX_DESK_FILES];
static int g_file_count = 0;
static int g_selected_file = -1;      /* 主选中项（打开/重命名用） */
static uint64_t g_sel_mask = 0;       /* 桌面多选位图 */
static int g_last_clicked_file = -1;
static uint32_t g_click_tick = 0;
static int g_desk_cols = 1;
static struct mui_dialog g_desk_dialog;   /* 桌面图标 Rename 对话框 */

static int desk_sel_count(void) {
    int n = 0;
    for (int i = 0; i < g_file_count; i++)
        if (g_sel_mask & (1ull << i)) n++;
    return n;
}

static int desk_is_selected(int idx) {
    if (idx < 0 || idx >= g_file_count) return 0;
    return (g_sel_mask >> idx) & 1;
}

static int desk_sel_first(void) {
    for (int i = 0; i < g_file_count; i++)
        if (g_sel_mask & (1ull << i)) return i;
    return -1;
}

static void desk_sel_clear(void) {
    g_sel_mask = 0;
    g_selected_file = -1;
}

static void desk_sel_only(int idx) {
    if (idx < 0 || idx >= g_file_count) {
        desk_sel_clear();
        return;
    }
    g_sel_mask = 1ull << idx;
    g_selected_file = idx;
}

static void desk_sel_toggle(int idx) {
    if (idx < 0 || idx >= g_file_count) return;
    g_sel_mask ^= (1ull << idx);
    if (g_sel_mask & (1ull << idx))
        g_selected_file = idx;
    else if (g_selected_file == idx)
        g_selected_file = desk_sel_first();
    if (g_sel_mask == 0) g_selected_file = -1;
}

static int desk_collect_paths(char paths[][256], int max) {
    int n = 0;
    for (int i = 0; i < g_file_count && n < max; i++) {
        if (!(g_sel_mask & (1ull << i))) continue;
        snprintf(paths[n], 256, "SYS:\\%s", g_files[i].name);
        n++;
    }
    if (n == 0 && g_selected_file >= 0 && g_selected_file < g_file_count) {
        snprintf(paths[0], 256, "SYS:\\%s", g_files[g_selected_file].name);
        n = 1;
    }
    return n;
}

static void calc_desk_cols(void) {
    g_desk_cols = (g_W - 2 * ICON_PAD_X) / ICON_CELL_W;
    if (g_desk_cols < 1) g_desk_cols = 1;
}

static int is_c_source(const char *name) {
    if (!name) return 0;
    size_t len = strlen(name);
    return len > 2 && name[len - 2] == '.' &&
           (name[len - 1] == 'c' || name[len - 1] == 'C');
}

static void draw_file_icon(int idx) {
    struct fs_entry_info *f = &g_files[idx];
    int selected = desk_is_selected(idx);
    
    int col = idx % g_desk_cols;
    int row = idx / g_desk_cols;
    int px = ICON_PAD_X + col * ICON_CELL_W;
    int py = ICON_PAD_Y + row * ICON_CELL_H;
    
    int ix = px + (ICON_CELL_W - ICON_SIZE) / 2;
    int iy = py;

    for (int j = 0; j < ICON_SIZE; j++) {
        unsigned int c = gfx_lerp(WALL_TOP, WALL_BOTTOM, iy + j, g_H);
        for (int i = 0; i < ICON_SIZE; i++) {
            g_wall_pixels[(iy + j) * g_W + ix + i] = c;
        }
    }

    if (f->is_dir) {
        icon_draw_folder_large(g_wall_pixels, g_W, ix, iy, ICON_SIZE);
    } else if (f->is_exec) {
        icon_draw_exec_large(g_wall_pixels, g_W, ix, iy, ICON_SIZE);
    } else {
        gfx_fill_rect(g_wall_pixels, g_W, g_H, ix, iy, ICON_SIZE, ICON_SIZE, 0xFFFFFF);
        for (int j = 8; j < ICON_SIZE - 4; j += 8) {
            gfx_fill_rect(g_wall_pixels, g_W, g_H, ix + 8, iy + j, ICON_SIZE - 16, 2, 0x9E9E9E);
        }
    }

    if (selected) {
        unsigned int edge = 0xFFFFFF;
        gfx_fill_rect(g_wall_pixels, g_W, g_H, ix, iy, ICON_SIZE, 1, edge); 
        gfx_fill_rect(g_wall_pixels, g_W, g_H, ix, iy + ICON_SIZE - 1, ICON_SIZE, 1, edge); 
        gfx_fill_rect(g_wall_pixels, g_W, g_H, ix, iy, 1, ICON_SIZE, edge); 
        gfx_fill_rect(g_wall_pixels, g_W, g_H, ix + ICON_SIZE - 1, iy, 1, ICON_SIZE, edge); 
    }

    char label[64];
    int n = 0, tw = 0;
    const unsigned char *np = (const unsigned char*)f->name;
    while (*np && n < (int)sizeof(label) - 4) {
        uint32_t cp;
        int clen = ufont_utf8_decode(np, &cp);
        int cw = wm_font_width((int)cp);
        if (!cw) cw = 8;
        if (tw + cw > ICON_CELL_W - 8) break;
        memcpy(label + n, np, clen);
        n += clen;
        tw += cw;
        np += clen;
    }
    label[n] = '\0';

    int ly = iy + ICON_SIZE + 4;
    int lx = px + 2;
    int lw = ICON_CELL_W - 4;
    unsigned int lbg = selected ? 0x3A6EA5 : 0x1A1A2E;

    gfx_fill_rect(g_wall_pixels, g_W, g_H, lx, ly, lw, 16, lbg);

    int tx = px + (ICON_CELL_W - tw) / 2;
    if (tx < px + 2) tx = px + 2;
    gfx_draw_text(g_wall_pixels, g_W, tx, ly, label, 0xFFFFFF, lbg);
}

static void draw_all_files(void) {
    for (int i = 0; i < g_file_count; i++) draw_file_icon(i);
}

/* 多选变化：重绘所有选中状态发生变化的图标，只发布包围盒 */
static void redraw_selection_changed(uint64_t old_mask) {
    uint64_t changed = (old_mask ^ g_sel_mask) &
                       ((g_file_count >= 64) ? ~0ull : ((1ull << g_file_count) - 1));
    if (!changed) return;

    int x0 = g_W, y0 = g_H, x1 = 0, y1 = 0;
    int any = 0;

    for (int idx = 0; idx < g_file_count; idx++) {
        if (!((changed >> idx) & 1)) continue;

        draw_file_icon(idx);

        int cx0 = ICON_PAD_X + (idx % g_desk_cols) * ICON_CELL_W;
        int cy0 = ICON_PAD_Y + (idx / g_desk_cols) * ICON_CELL_H;
        int cx1 = cx0 + ICON_CELL_W;
        int cy1 = cy0 + ICON_SIZE + 24;
        if (!any || cx0 < x0) x0 = cx0;
        if (!any || cy0 < y0) y0 = cy0;
        if (!any || cx1 > x1) x1 = cx1;
        if (!any || cy1 > y1) y1 = cy1;
        any = 1;
    }
    if (!any) return;

    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > g_W) x1 = g_W;
    if (y1 > g_H) y1 = g_H;

    shm_mark_rect(g_wall_hdr, x0, y0, x1, y1);
    shm_publish(g_wall_hdr);
}

/* ============================================================
 *                   文件夹管理窗口 (FM)
 * ============================================================ */

#define MAX_FM_WINDOWS 4
#define FM_TOP_BAR_H 24
#define FM_ROW_H     24

/* 窗口内右键菜单 */
#define FM_MENU_W       152
#define FM_MENU_ITEM_H  24
#define FM_MENU_PAD     4
#define FM_MENU_MAX_ITEMS 5

/* 顶栏输入模式 */
#define FM_INPUT_NONE    0
#define FM_INPUT_PATH    1   /* 手打目录名跳转 */
#define FM_INPUT_NEWDIR  3
#define FM_INPUT_NEWFILE 4

#define FM_SCROLL_W      8

struct fm_win {
    int used;
    char path[128];
    unsigned int *pixels;
    struct shm_hdr *hdr;
    int ev;
    char shm_name[16];
    int cw, ch;
    int map_size;                /* 当前 hdr 实际 mmap 的字节数 */

    /* methylui 绘制组件 */
    struct mui_surface surf;
    struct mui_list  list;       /* 文件列表：一行一项 + 滚动 + 多选 */
    struct mui_scroll scroll;    /* 独立竖直滚动条（mui_scroll） */
    struct mui_textbox input_box;
    struct mui_dialog dialog;    /* Rename 对话框（不弹新窗口） */
    int scroll_dirty;

    int last_click_idx;
    uint32_t last_click_tick;

    /* 顶栏输入 / 提示 */
    int  input_mode;        /* FM_INPUT_* */
    char input_prompt[24];
    char status[64];

    /* 窗口内右键菜单 */
    int menu_open;
    int menu_target;        /* >=0: 选中项菜单；-1: 空白处菜单 */
    int menu_hover;
    int menu_x, menu_y, menu_w, menu_h;

    /* ESC 序列解析（方向键 / Delete / PageUp / PageDown） */
    int  esc_state;
    int  esc_len;
    char esc_buf[8];
    int  esc_was_ui;

    /* 拖拽 */
    int drag_candidate;
    int drag_moved;
    int drag_start_x, drag_start_y;
};

static struct fm_win g_fm_wins[MAX_FM_WINDOWS];
static int g_fm_shm_gen = 0;     /* 每个新 shm 名字唯一，避免关闭/重开时撞名 */

static void fm_paint_icon(struct mui_surface *s, int x, int y, int icon) {
    if (icon == MUI_LIST_ICON_DIR)
        icon_draw_folder_small(s->pixels, s->w, x, y);
    else if (icon == MUI_LIST_ICON_EXEC)
        icon_draw_exec_small(s->pixels, s->w, x, y);
    else
        icon_draw_file_small(s->pixels, s->w, x, y);
}

/* 文件窗口：脏区绘制 / 局部刷新 */

static void draw_fm_menu(struct fm_win *w);

static void fm_setup_surface(struct fm_win *w) {
    w->surf.pixels = w->pixels;
    w->surf.w = w->cw;
    w->surf.h = w->ch;
    w->surf.hdr = w->hdr;
    w->surf.clip_on = 0;
}

/* 合并一个脏矩形（不覆盖尚未发布的区域） */
static void shm_mark_rect(struct shm_hdr *h, int x0, int y0, int x1, int y1) {
    if (!h || x1 <= x0 || y1 <= y0) return;
    if (h->cur_x0 == SHM_DIRTY_EMPTY || h->cur_x1 <= h->cur_x0) {
        h->cur_x0 = (uint32_t)x0;
        h->cur_y0 = (uint32_t)y0;
        h->cur_x1 = (uint32_t)x1;
        h->cur_y1 = (uint32_t)y1;
    } else {
        if ((uint32_t)x0 < h->cur_x0) h->cur_x0 = (uint32_t)x0;
        if ((uint32_t)y0 < h->cur_y0) h->cur_y0 = (uint32_t)y0;
        if ((uint32_t)x1 > h->cur_x1) h->cur_x1 = (uint32_t)x1;
        if ((uint32_t)y1 > h->cur_y1) h->cur_y1 = (uint32_t)y1;
    }
}

/* 把 cur 脏区发布给 WM。pub 尚未被消费时让 cur 继续累积。 */
static void shm_publish(struct shm_hdr *h) {
    if (!h) return;
    if (h->cur_x0 == SHM_DIRTY_EMPTY || h->cur_x1 <= h->cur_x0 ||
        h->cur_y1 <= h->cur_y0)
        return;

    if (h->pub_x0 == SHM_DIRTY_EMPTY || h->pub_x1 <= h->pub_x0) {
        h->pub_x0 = h->cur_x0;
        h->pub_y0 = h->cur_y0;
        h->pub_x1 = h->cur_x1;
        h->pub_y1 = h->cur_y1;
        h->counter++;
        h->cur_x0 = SHM_DIRTY_EMPTY;
        h->cur_y0 = 0;
        h->cur_x1 = 0;
        h->cur_y1 = 0;
    }
}

static void fm_publish(struct fm_win *w) {
    if (!w || !w->used || !w->hdr) return;
    shm_publish(w->hdr);
}

static void fm_paint_topbar(struct fm_win *w) {
    struct mui_surface *s = &w->surf;

    mui_clip_set(s, 0, 0, w->cw, FM_TOP_BAR_H);
    mui_fill(s, 0, 0, w->cw, FM_TOP_BAR_H, 0x2A2A3E);

    /* 顶栏：输入框 > 状态消息 > 当前路径 */
    if (w->status[0]) {
        mui_text(s, 4, (FM_TOP_BAR_H - MUI_FONT_H) / 2,
                 w->status, 0xFFC040, 0x2A2A3E);
    } else if (w->input_mode != FM_INPUT_NONE) {
        mui_text(s, 4, (FM_TOP_BAR_H - MUI_FONT_H) / 2,
                 w->input_prompt, 0xFFFFFF, 0x2A2A3E);
        int px = 4 + mui_text_width(w->input_prompt);
        int tw = w->cw - px - 4;
        if (tw >= MUI_FONT_W) {
            w->input_box.base.w = tw;
            w->input_box.base.h = FM_TOP_BAR_H;
            w->input_box.base.focused = 1;
            w->input_box.base.dirty = 1;
            mui_widget_paint(&w->input_box.base, s, px, 0);
        }
    } else {
        char bar[160];
        snprintf(bar, sizeof(bar), "%s", w->path);
        int max_chars = (w->cw - 8) / MUI_FONT_W;
        const char *show = bar;
        if (max_chars > 0) {
            int blen = strlen(bar);
            if (blen > max_chars) show = bar + (blen - max_chars);
        }
        mui_text(s, 4, (FM_TOP_BAR_H - MUI_FONT_H) / 2,
                 show, 0xCCCCCC, 0x2A2A3E);
    }
    mui_clip_reset(s);
}

static void fm_sync_scroll_value(struct fm_win *w) {
    int max = mui_scroll_max_value(&w->scroll);
    if (w->list.top < 0) w->list.top = 0;
    if (w->list.top > max) w->list.top = max;
    w->scroll.value = w->list.top;
    w->scroll.base.dirty = 1;
}

/* 根据窗口尺寸 / 列表项数同步滚动条范围 */
static void fm_sync_scroll(struct fm_win *w) {
    w->list.base.w = w->cw - FM_SCROLL_W;
    w->list.base.h = w->ch - FM_TOP_BAR_H;
    w->scroll.base.x = w->cw - FM_SCROLL_W;
    w->scroll.base.y = FM_TOP_BAR_H;
    w->scroll.base.w = FM_SCROLL_W;
    w->scroll.base.h = w->ch - FM_TOP_BAR_H;
    if (w->scroll.base.h < 4) w->scroll.base.h = 4;

    int visible = w->scroll.base.h / FM_ROW_H;
    if (visible < 1) visible = 1;
    mui_scroll_set_range(&w->scroll, w->list.count, visible);
    fm_sync_scroll_value(w);
}

static void fm_scroll_on_change(struct mui_scroll *s) {
    struct fm_win *w = (struct fm_win*)s->base.user;
    if (!w) return;
    w->list.top = s->value;
    w->scroll_dirty = 1;
}

static void fm_paint_list(struct fm_win *w) {
    w->list.base.w = w->cw - FM_SCROLL_W;
    w->list.base.h = w->ch - FM_TOP_BAR_H;

    mui_clip_set(&w->surf, 0, FM_TOP_BAR_H,
                 w->cw - FM_SCROLL_W, w->ch - FM_TOP_BAR_H);
    mui_list_paint(&w->list, &w->surf, 0, FM_TOP_BAR_H);
    mui_clip_reset(&w->surf);
}

static void fm_paint_row(struct fm_win *w, int idx) {
    w->list.base.w = w->cw - FM_SCROLL_W;
    w->list.base.h = w->ch - FM_TOP_BAR_H;

    mui_clip_set(&w->surf, 0, FM_TOP_BAR_H,
                 w->cw - FM_SCROLL_W, w->ch - FM_TOP_BAR_H);
    mui_list_paint_row(&w->list, &w->surf, 0, FM_TOP_BAR_H, idx);
    mui_clip_reset(&w->surf);
}

static void fm_paint_scroll(struct fm_win *w) {
    w->scroll.base.dirty = 1;
    mui_clip_set(&w->surf, w->scroll.base.x, w->scroll.base.y,
                 w->scroll.base.w, w->scroll.base.h);
    mui_widget_paint(&w->scroll.base, &w->surf,
                     w->scroll.base.x, w->scroll.base.y);
    mui_clip_reset(&w->surf);
}

static void fm_paint_dialog(struct fm_win *w) {
    if (w->dialog.active)
        mui_dialog_paint(&w->dialog, &w->surf, w->cw, w->ch);
}

/* 整窗重绘（导航、文件变化、尺寸变化、菜单开关时用） */
static void fm_redraw(struct fm_win *w) {
    fm_setup_surface(w);
    fm_paint_topbar(w);
    fm_paint_list(w);
    fm_paint_scroll(w);
    if (w->menu_open) draw_fm_menu(w);
    fm_paint_dialog(w);
    fm_publish(w);
}

/* 只重绘顶栏（输入、状态提示） */
static void fm_redraw_topbar(struct fm_win *w) {
    fm_setup_surface(w);
    fm_paint_topbar(w);
    fm_publish(w);
}

/* 只重绘旧/新 hover 行 */
static void fm_redraw_hover(struct fm_win *w, int old_idx, int new_idx) {
    fm_setup_surface(w);
    if (old_idx >= 0) fm_paint_row(w, old_idx);
    if (new_idx >= 0) fm_paint_row(w, new_idx);
    fm_publish(w);
}

/* 只重绘菜单区域（菜单 hover） */
static void fm_redraw_menu(struct fm_win *w) {
    fm_setup_surface(w);
    draw_fm_menu(w);
    fm_publish(w);
}

static void fm_redraw_scroll(struct fm_win *w) {
    fm_setup_surface(w);
    fm_paint_list(w);
    fm_paint_scroll(w);
    fm_publish(w);
}

static void exec_elf(const char *path) {
    if (!path || !path[0]) return;

    if (strstr(path, "shell") != NULL) {
        spawn_term(600, 400, 200, 100);
        return;
    }
    
    char *argv[] = { (char*)path, NULL };
    struct spawn_params sp = {
        .size = sizeof(sp),
        .in_fd = SPAWN_FD_INHERIT,
        .out_fd = SPAWN_FD_INHERIT,
        .err_fd = SPAWN_FD_INHERIT,
        .in_pipe = -1, .out_pipe = -1, .err_pipe = -1,
        .in_pty = -1, .out_pty = -1, .err_pty = -1,
        .flags = 0, .envp = 0, .out_pid = 0,
        .child_class = CLASS_USER,
        .child_caps = 0,
    };
    spawn(path, argv, &sp);
}


/* ============================================================
 *        文件窗口：路径辅助 / 文件操作 / 右键菜单 / 键盘
 * ============================================================ */

/* 拼接目录与名字："SYS:\DIR" + "A.C" -> "SYS:\DIR\A.C" */
static void fm_path_join(const char *dir, const char *name, char *out, int out_size) {
    int len = strlen(dir);
    if (len > 0 && dir[len - 1] == '\\')
        snprintf(out, out_size, "%s%s", dir, name);
    else
        snprintf(out, out_size, "%s\\%s", dir, name);
}

static const char *fm_path_base(const char *path) {
    const char *base = path;
    for (const char *p = path; *p; p++) {
        if (*p == '\\') base = p + 1;
    }
    return base;
}

/* "SYS:\DIR\SUB" -> "SYS:\DIR"；根目录保持不变 */
static void fm_path_parent(const char *path, char *out, int out_size) {
    int i = (int)strlen(path) - 1;
    while (i > 0 && path[i] == '\\') i--;
    while (i >= 0 && path[i] != '\\') i--;
    if (i < 0) {
        snprintf(out, out_size, "SYS:\\");
        return;
    }
    int n = i + 1;
    if (n >= out_size) n = out_size - 1;
    memcpy(out, path, n);
    out[n] = '\0';
}

static void fm_item_path(struct fm_win *w, int idx, char *out, int out_size) {
    fm_path_join(w->path, w->list.items[idx].text, out, out_size);
}

/* path 是否严格位于 dir 里面（用于阻止把目录粘贴到它自己内部） */
static int fm_path_is_inside(const char *path, const char *dir) {
    int n = strlen(dir);
    if (n == 0) return 0;
    if (strncmp(path, dir, n) != 0) return 0;
    return path[n] == '\\';
}

static int fm_path_exists(const char *path) {
    int fd = fs_open(path, FS_O_RDONLY);
    if (fd >= 0) { fs_close(fd); return 1; }
    int d = fs_opendir(path);
    if (d >= 0) { fs_closedir(d); return 1; }
    return 0;
}

/* 在 dir 下为 name 找不冲突的名字（重名时追加 _1、_2 …） */
static void fm_unique_path(const char *dir, const char *name, char *out, int out_size) {
    fm_path_join(dir, name, out, out_size);
    if (!fm_path_exists(out)) return;

    const char *dot = NULL;
    for (const char *p = name; *p; p++) {
        if (*p == '.') dot = p;
    }
    int stem = dot ? (int)(dot - name) : (int)strlen(name);
    if (stem > 48) stem = 48;
    char base[64];
    memcpy(base, name, stem);
    base[stem] = '\0';

    for (int n = 1; n < 1000; n++) {
        char cand[96];
        if (dot)
            snprintf(cand, sizeof(cand), "%s_%d%s", base, n, dot);
        else
            snprintf(cand, sizeof(cand), "%s_%d", base, n);
        fm_path_join(dir, cand, out, out_size);
        if (!fm_path_exists(out)) return;
    }
}

/* 递归删除文件/目录；返回 0 或错误码 */
static int fm_remove_recursive(const char *path) {
    int d = fs_opendir(path);
    if (d < 0) return fs_unlink(path);          /* 普通文件 */
    fs_closedir(d);

    /* 每轮只删一个子项，删完重新枚举，避免删除时目录项移动导致漏项 */
    for (int guard = 0; guard < 4096; guard++) {
        d = fs_opendir(path);
        if (d < 0) break;

        char child[256];
        int found = 0;
        struct dirent ent;
        for (;;) {
            memset(&ent, 0, sizeof(ent));
            int rc = fs_readdir(d, &ent);
            if (rc < 0 || ent.name[0] == '\0') break;
            if (strcmp(ent.name, ".") == 0 || strcmp(ent.name, "..") == 0) continue;
            fm_path_join(path, ent.name, child, sizeof(child));
            found = 1;
            break;
        }
        fs_closedir(d);
        if (!found) break;
        if (fm_remove_recursive(child) < 0) return -1;
    }
    return fs_rmdir(path);
}

static int fm_copy_file(const char *src, const char *dst) {
    int in = fs_open(src, FS_O_RDONLY);
    if (in < 0) return in;
    int out = fs_open(dst, FS_O_WRONLY | FS_O_CREAT | FS_O_TRUNC);
    if (out < 0) { fs_close(in); return out; }

    char buf[256];
    int ret = 0;
    for (;;) {
        int n = fs_read(in, buf, sizeof(buf));
        if (n < 0) { ret = n; break; }
        if (n == 0) break;
        int wn = fs_write(out, buf, n);
        if (wn != n) { ret = (wn < 0) ? wn : -1; break; }
    }
    fs_close(in);
    fs_close(out);
    return ret;
}

static int fm_copy_recursive(const char *src, const char *dst) {
    int d = fs_opendir(src);
    if (d < 0) return fm_copy_file(src, dst);   /* 普通文件 */

    int r = fs_mkdir(dst);
    if (r < 0) { fs_closedir(d); return r; }

    struct dirent ent;
    for (;;) {
        memset(&ent, 0, sizeof(ent));
        int rc = fs_readdir(d, &ent);
        if (rc < 0 || ent.name[0] == '\0') break;
        if (strcmp(ent.name, ".") == 0 || strcmp(ent.name, "..") == 0) continue;
        char s[256], t[256];
        fm_path_join(src, ent.name, s, sizeof(s));
        fm_path_join(dst, ent.name, t, sizeof(t));
        fm_copy_recursive(s, t);
    }
    fs_closedir(d);
    return 0;
}

/* ---------- 剪贴板（支持多选批量） ---------- */
#define CLIP_MAX 32
static char g_clip_paths[CLIP_MAX][256];
static int  g_clip_count = 0;
static int  g_clip_cut = 0;

/* 批量操作时收集选中项路径；没有选中则用光标项 */
static int fm_collect_selection(struct fm_win *w, char paths[][256], int max) {
    int n = 0;
    for (int i = 0; i < w->list.count && n < max; i++) {
        if (w->list.selected[i]) {
            fm_path_join(w->path, w->list.items[i].text, paths[n], 256);
            n++;
        }
    }
    if (n == 0 && w->list.cursor >= 0 && w->list.cursor < w->list.count) {
        fm_path_join(w->path, w->list.items[w->list.cursor].text, paths[0], 256);
        n = 1;
    }
    return n;
}

/* ---------- 窗口内右键菜单 ---------- */

static int fm_menu_count(struct fm_win *w) {
    if (w->menu_target < 0) return 4;
    if (mui_list_selected_count(&w->list) > 1) return 3;
    int idx = w->menu_target;
    if (idx >= 0 && idx < w->list.count &&
        w->list.items[idx].icon == MUI_LIST_ICON_FILE &&
        is_c_source(w->list.items[idx].text))
        return 5;
    return 4;
}

static const char *fm_menu_label(struct fm_win *w, int i) {
    static const char *bg_labels[FM_MENU_MAX_ITEMS]   = { "\xE5\x88\xB7\xE6\x96\xB0", "\xE7\xB2\x98\xE8\xB4\xB4", "\xE6\x96\xB0\xE5\xBB\xBA\xE6\x96\x87\xE4\xBB\xB6\xE5\xA4\xB9", "\xE6\x96\xB0\xE5\xBB\xBA\x20\x43\x20\xE6\xBA\x90\xE6\x96\x87\xE4\xBB\xB6", NULL };
    static const char *item_labels[FM_MENU_MAX_ITEMS] = { "\xE5\xA4\x8D\xE5\x88\xB6", "\xE5\x89\xAA\xE5\x88\x87", "\xE5\x88\xA0\xE9\x99\xA4", "\xE9\x87\x8D\xE5\x91\xBD\xE5\x90\x8D", "\xE7\xBC\x96\xE8\xAF\x91" };
    return (w->menu_target >= 0) ? item_labels[i] : bg_labels[i];
}

static void draw_fm_menu(struct fm_win *w) {
    struct mui_surface *s = &w->surf;
    mui_clip_set(s, w->menu_x, w->menu_y, w->menu_w, w->menu_h);

    mui_fill(s, w->menu_x, w->menu_y, w->menu_w, w->menu_h, 0x2A2A3E);
    mui_border(s, w->menu_x, w->menu_y, w->menu_w, w->menu_h, 0x5A5A7E);

    int count = fm_menu_count(w);
    for (int i = 0; i < count; i++) {
        int y = w->menu_y + FM_MENU_PAD + i * FM_MENU_ITEM_H;
        unsigned int bg = (i == w->menu_hover) ? 0x3A6EA5 : 0x2A2A3E;
        if (i == w->menu_hover)
            mui_fill(s, w->menu_x + 1, y, w->menu_w - 2, FM_MENU_ITEM_H, bg);
        mui_text(s, w->menu_x + 8, y + (FM_MENU_ITEM_H - MUI_FONT_H) / 2,
                 fm_menu_label(w, i), 0xFFFFFF, bg);
    }

    mui_clip_reset(s);
}

static void fm_open_menu(struct fm_win *w, int target, int mx, int my) {
    w->menu_open   = 1;
    w->menu_target = target;
    w->menu_hover  = -1;
    w->menu_w      = FM_MENU_W;
    w->menu_h      = fm_menu_count(w) * FM_MENU_ITEM_H + 2 * FM_MENU_PAD;

    if (mx + w->menu_w > w->cw) mx = w->cw - w->menu_w;
    if (my + w->menu_h > w->ch) my = w->ch - w->menu_h;
    if (mx < 0) mx = 0;
    if (my < 0) my = 0;
    w->menu_x = mx;
    w->menu_y = my;
    fm_redraw(w);
}

static int fm_menu_hit(struct fm_win *w, int mx, int my) {
    if (!w->menu_open) return -1;
    if (mx < w->menu_x || mx >= w->menu_x + w->menu_w) return -1;
    if (my < w->menu_y || my >= w->menu_y + w->menu_h) return -1;
    int rel = my - w->menu_y - FM_MENU_PAD;
    if (rel < 0) return -1;
    int i = rel / FM_MENU_ITEM_H;
    if (i < 0 || i >= fm_menu_count(w)) return -1;
    return i;
}

/* ---------- 刷新 / 导航 ---------- */

static void fm_input_reset(struct fm_win *w) {
    w->input_mode   = FM_INPUT_NONE;
    w->input_prompt[0] = '\0';
    w->input_box.base.focused = 0;
}

static struct fs_entry_info g_fm_tmp[128];

static void fm_load_items(struct fm_win *w) {
    int n = fs_read_dir(w->path, g_fm_tmp, 128);
    mui_list_clear(&w->list);
    for (int i = 0; i < n; i++) {
        int icon = g_fm_tmp[i].is_dir ? MUI_LIST_ICON_DIR :
                   (g_fm_tmp[i].is_exec ? MUI_LIST_ICON_EXEC : MUI_LIST_ICON_FILE);
        mui_list_set_item(&w->list, i, g_fm_tmp[i].name, icon);
    }
    w->list.count = n;
    fm_sync_scroll(w);
}

static void fm_refresh(struct fm_win *w) {
    int top = w->list.top;
    fm_load_items(w);
    mui_list_clear_selection(&w->list);
    w->list.cursor = -1;
    w->list.hover = -1;
    w->list.top = top;
    mui_list_scroll(&w->list, 0);       /* 夹紧 top */
    fm_sync_scroll(w);
}

static void fm_select_name(struct fm_win *w, const char *name) {
    w->list.cursor = -1;
    mui_list_clear_selection(&w->list);
    for (int i = 0; i < w->list.count; i++) {
        if (strcmp(w->list.items[i].text, name) == 0) {
            mui_list_select_only(&w->list, i);
            fm_sync_scroll_value(w);
            return;
        }
    }
}

static void fm_navigate(struct fm_win *w, const char *path) {
    strncpy(w->path, path, sizeof(w->path) - 1);
    w->path[sizeof(w->path) - 1] = '\0';
    w->last_click_idx = -1;
    w->status[0] = '\0';
    w->menu_open = 0;
    w->esc_state = 0;
    w->drag_candidate = 0;
    fm_input_reset(w);
    fm_load_items(w);
    mui_list_clear_selection(&w->list);
    w->list.cursor = -1;
    w->list.top = 0;
    w->list.hover = -1;
    fm_sync_scroll(w);
    fm_redraw(w);
}

static void fm_go_parent(struct fm_win *w) {
    char parent[128];
    fm_path_parent(w->path, parent, sizeof(parent));
    if (strcmp(parent, w->path) == 0) return;
    fm_navigate(w, parent);
}

/* ---------- 键盘输入 ---------- */

static void fm_input_begin(struct fm_win *w, int mode, const char *prompt) {
    w->input_mode   = mode;
    strncpy(w->input_prompt, prompt, sizeof(w->input_prompt) - 1);
    w->input_prompt[sizeof(w->input_prompt) - 1] = '\0';
    mui_textbox_set_text(&w->input_box, "");
    w->input_box.base.focused = 1;
    w->input_box.base.dirty = 1;
    w->menu_open = 0;
    w->status[0] = '\0';
}

static void fm_set_status(struct fm_win *w, const char *msg) {
    strncpy(w->status, msg, sizeof(w->status) - 1);
    w->status[sizeof(w->status) - 1] = '\0';
}

static void fm_input_commit(struct fm_win *w) {
    char text[MUI_TEXTBOX_MAX_LEN + 1];
    int mode = w->input_mode;

    strncpy(text, mui_textbox_get_text(&w->input_box), sizeof(text) - 1);
    text[sizeof(text) - 1] = '\0';

    /* 去掉首尾空格 */
    int start = 0;
    while (text[start] == ' ') start++;
    int end = (int)strlen(text);
    while (end > start && text[end - 1] == ' ') end--;
    char name[128];
    int n = end - start;
    if (n < 0) n = 0;
    if (n > (int)sizeof(name) - 1) n = sizeof(name) - 1;
    memcpy(name, text + start, n);
    name[n] = '\0';

    fm_input_reset(w);

    if (mode == FM_INPUT_NONE || name[0] == '\0') {
        fm_redraw_topbar(w);
        return;
    }

    if (mode == FM_INPUT_PATH) {
        if (strcmp(name, "..") == 0) { fm_go_parent(w); return; }
        if (strcmp(name, ".") == 0)  { fm_redraw(w); return; }

        for (char *p = name; *p; p++) {
            if (*p == '/') *p = '\\';
        }
        char path[256];
        if (strchr(name, ':'))
            snprintf(path, sizeof(path), "%s", name);
        else if (name[0] == '\\')
            snprintf(path, sizeof(path), "SYS:%s", name);
        else
            fm_path_join(w->path, name, path, sizeof(path));

        int d = fs_opendir(path);
        if (d >= 0) {
            fs_closedir(d);
            fm_navigate(w, path);
        } else {
            fm_set_status(w, "No such directory");
            fm_redraw_topbar(w);
        }
    } else if (mode == FM_INPUT_NEWDIR) {
        char p[256];
        fm_path_join(w->path, name, p, sizeof(p));
        if (fm_path_exists(p)) {
            fm_set_status(w, "Already exists");
        } else if (fs_mkdir(p) < 0) {
            fm_set_status(w, "mkdir failed");
        } else {
            fm_refresh(w);
            fm_select_name(w, name);
            desktop_refresh_icons();
            fm_set_status(w, "Created");
        }
        fm_redraw(w);
    } else if (mode == FM_INPUT_NEWFILE) {
        char fname[96];
        strncpy(fname, name, sizeof(fname) - 1);
        fname[sizeof(fname) - 1] = '\0';
        if (!is_c_source(fname)) {
            int len = strlen(fname);
            if (len < (int)sizeof(fname) - 3) {
                fname[len]     = '.';
                fname[len + 1] = 'c';
                fname[len + 2] = '\0';
            }
        }
        char p[256];
        fm_path_join(w->path, fname, p, sizeof(p));
        if (fm_path_exists(p)) {
            fm_set_status(w, "Already exists");
        } else {
            int fd = fs_open(p, FS_O_WRONLY | FS_O_CREAT | FS_O_TRUNC);
            if (fd < 0) {
                fm_set_status(w, "Create failed");
            } else {
                fs_close(fd);
                fm_refresh(w);
                fm_select_name(w, fname);
                desktop_refresh_icons();
                fm_set_status(w, "Created");
            }
        }
        fm_redraw(w);
    }
}

/* ---------- 打开 / 删除 / 粘贴 ---------- */

static void fm_open_selected(struct fm_win *w, int new_window) {
    int idx = w->list.cursor;
    if (idx < 0 || idx >= w->list.count) {
        if (mui_list_selected_count(&w->list) == 1) {
            for (int i = 0; i < w->list.count; i++)
                if (w->list.selected[i]) { idx = i; break; }
        }
    }
    if (idx < 0 || idx >= w->list.count) return;

    char path[256];
    fm_item_path(w, idx, path, sizeof(path));
    int icon = w->list.items[idx].icon;

    if (icon == MUI_LIST_ICON_DIR) {
        if (new_window) open_folder_window(path);   /* Ctrl+Enter/Ctrl+双击：新窗口 */
        else            fm_navigate(w, path);
    } else if (is_c_source(w->list.items[idx].text)) {
        open_source_file(path);
    } else if (icon == MUI_LIST_ICON_EXEC) {
        exec_elf(path);
    } else {
        fm_set_status(w, "Can't open this file");
        fm_redraw(w);
    }
}

static void fm_delete_selected(struct fm_win *w) {
    char paths[CLIP_MAX][256];
    int n = fm_collect_selection(w, paths, CLIP_MAX);
    if (n == 0) return;

    int ok = 0;
    for (int i = 0; i < n; i++) {
        if (fm_remove_recursive(paths[i]) >= 0) ok++;
    }

    fm_refresh(w);
    desktop_refresh_icons();
    fm_set_status(w, ok > 0 ? "Deleted" : "Delete failed");
    fm_redraw(w);
}

static void fm_paste(struct fm_win *w) {
    if (g_clip_count <= 0) {
        fm_set_status(w, "Clipboard empty");
        fm_redraw(w);
        return;
    }

    int done = 0;
    int cut = g_clip_cut;

    for (int i = 0; i < g_clip_count; i++) {
        const char *src = g_clip_paths[i];
        const char *base = fm_path_base(src);
        if (!base[0]) continue;
        if (fm_path_is_inside(w->path, src)) continue;

        char src_dir[256];
        fm_path_parent(src, src_dir, sizeof(src_dir));
        if (cut && strcmp(src_dir, w->path) == 0) continue;

        char dst[256];
        fm_unique_path(w->path, base, dst, sizeof(dst));

        int r = cut ? fs_rename(src, dst) : fm_copy_recursive(src, dst);
        if (r >= 0) done++;
    }

    if (cut && done > 0) g_clip_count = 0;
    fm_refresh(w);
    desktop_refresh_icons();
    fm_set_status(w, done > 0 ? (cut ? "Moved" : "Pasted") : "Paste failed");
    fm_redraw(w);
}

/* ---------- 拖拽移动 ---------- */

#define DRAG_MAX 32
static char g_drag_paths[DRAG_MAX][256];
static int  g_drag_count = 0;
static int  g_drag_active = 0;
static int  g_drag_armed = 0;        /* 指针真正移动过，才允许落下 */
static int  g_drag_src_fm = -1;      /* -1 = 桌面 */

static void drag_clear(void) {
    g_drag_active = 0;
    g_drag_armed = 0;
    g_drag_count = 0;
    g_drag_src_fm = -1;
}

static void fm_drag_begin(struct fm_win *w) {
    int n = fm_collect_selection(w, g_drag_paths, DRAG_MAX);
    if (n <= 0) return;
    g_drag_count = n;
    g_drag_active = 1;
    g_drag_armed = 0;
    g_drag_src_fm = (int)(w - g_fm_wins);
}

static int fm_move_into(const char *src, const char *dest_dir) {
    if (strcmp(src, dest_dir) == 0) return -1;
    if (fm_path_is_inside(dest_dir, src)) return -1;   /* 不能移进自己的子目录 */

    char src_dir[256];
    fm_path_parent(src, src_dir, sizeof(src_dir));
    if (strcmp(src_dir, dest_dir) == 0) return -1;

    const char *base = fm_path_base(src);
    char dst[256];
    fm_unique_path(dest_dir, base, dst, sizeof(dst));
    return fs_rename(src, dst);
}

/* 把当前拖拽项移动到 dest_dir；w 为 NULL 表示目标是桌面根目录 */
static void fm_drop_to_dir(struct fm_win *w, const char *dest_dir) {
    if (!g_drag_active) return;
    if (!g_drag_armed) {           /* 只是点击，没有真正拖动 */
        drag_clear();
        return;
    }

    int moved = 0;
    int tried = 0;
    for (int i = 0; i < g_drag_count; i++) {
        char src_dir[256];
        fm_path_parent(g_drag_paths[i], src_dir, sizeof(src_dir));
        if (strcmp(src_dir, dest_dir) == 0) continue;   /* 已经在这个目录 */
        tried++;
        if (fm_move_into(g_drag_paths[i], dest_dir) >= 0) moved++;
    }

    int src_idx = g_drag_src_fm;
    drag_clear();

    if (src_idx >= 0 && src_idx < MAX_FM_WINDOWS && g_fm_wins[src_idx].used) {
        fm_refresh(&g_fm_wins[src_idx]);
        if (&g_fm_wins[src_idx] != w)
            fm_redraw(&g_fm_wins[src_idx]);
    }

    if (w) {
        fm_refresh(w);
        if (moved > 0)      fm_set_status(w, "Moved");
        else if (tried > 0) fm_set_status(w, "Move failed");
        fm_redraw(w);
    }

    desktop_refresh_icons();
}

/* 拖拽在同一个文件窗口内释放：落在子目录行上则移入该目录 */
static void fm_finish_drag(struct fm_win *w, int mx, int my) {
    if (!g_drag_active || g_drag_src_fm != (int)(w - g_fm_wins)) return;
    if (mx < 0 || my < 0 || mx >= w->cw || my >= w->ch) return;   /* 交给 WM 的 DROP */

    int row = mui_list_hit(&w->list, mx, my - FM_TOP_BAR_H);
    char dest[256];
    if (row >= 0 && w->list.items[row].icon == MUI_LIST_ICON_DIR)
        fm_path_join(w->path, w->list.items[row].text, dest, sizeof(dest));
    else
        snprintf(dest, sizeof(dest), "%s", w->path);

    if (strcmp(dest, w->path) == 0) {
        drag_clear();
        fm_redraw(w);
        return;
    }
    fm_drop_to_dir(w, dest);
}

/* 执行右键菜单项 */
static int compile_source_path(const char *src) {
    if (!src || !is_c_source(src)) return 0;
    char parent[256], stem[128], out[256];
    strncpy(stem, fm_path_base(src), sizeof(stem) - 1);
    stem[sizeof(stem) - 1] = '\0';
    int len = (int)strlen(stem);
    if (len > 2 && stem[len - 2] == '.' &&
        (stem[len - 1] == 'c' || stem[len - 1] == 'C'))
        stem[len - 2] = '\0';
    fm_path_parent(src, parent, sizeof(parent));
    fm_path_join(parent, stem, out, sizeof(out));

    char *argv[] = { "TCC", "-xc", "-o", out, (char*)src, NULL };
    struct spawn_params sp = {
        .size = sizeof(sp),
        .in_fd = SPAWN_FD_NULL, .out_fd = SPAWN_FD_NULL, .err_fd = SPAWN_FD_NULL,
        .in_pipe = -1, .out_pipe = -1, .err_pipe = -1,
        .in_pty = -1, .out_pty = -1, .err_pty = -1,
        .flags = 0, .envp = 0, .out_pid = 0,
        .child_class = CLASS_USER, .child_caps = 0,
    };
    int h = spawn("TCC", argv, &sp);
    if (h < 0) return 0;
    int st = 0, wr = wait(h, &st, 0);
    process_close(h);
    return wr == 0 && st == 0;
}

static void fm_compile_selected(struct fm_win *w, int idx) {
    if (!w || idx < 0 || idx >= w->list.count) return;
    char src[256];
    fm_item_path(w, idx, src, sizeof(src));
    int ok = compile_source_path(src);
    fm_refresh(w);
    desktop_refresh_icons();
    fm_set_status(w, ok ? "Compiled" : "Compile failed");
    fm_redraw(w);
}

static void desk_compile_file(int idx) {
    if (idx < 0 || idx >= g_file_count || g_files[idx].is_dir) return;
    char src[256];
    snprintf(src, sizeof(src), "SYS:\\%s", g_files[idx].name);
    if (compile_source_path(src)) desktop_refresh_icons();
}

static void fm_menu_action(struct fm_win *w, int item) {
    int target = w->menu_target;

    if (target >= 0 && target < w->list.count) {
        if (item == 0 || item == 1) {              /* Copy / Cut（支持多选） */
            int n = fm_collect_selection(w, g_clip_paths, CLIP_MAX);
            if (n > 0) {
                g_clip_count = n;
                g_clip_cut = (item == 1);
                fm_set_status(w, (item == 1) ? "Cut" : "Copied");
            }
            fm_redraw(w);
        } else if (item == 2) {                    /* Delete（支持多选） */
            fm_delete_selected(w);
        } else if (item == 3) {                    /* Rename（仅单选，MUI 对话框） */
            if (mui_list_selected_count(&w->list) <= 1) {
                int idx = w->list.cursor;
                if (idx < 0 || idx >= w->list.count || !w->list.selected[idx])
                    idx = target;
                if (idx >= 0 && idx < w->list.count) {
                    mui_dialog_open_rename(&w->dialog, w->path,
                                           w->list.items[idx].text);
                    fm_redraw(w);
                }
            }
        } else if (item == 4) {
            fm_compile_selected(w, target);
        }
        return;
    }

    if (item == 0) {                               /* Refresh */
        fm_refresh(w);
        fm_redraw(w);
    } else if (item == 1) {                        /* Paste */
        fm_paste(w);
    } else if (item == 2) {                        /* New Dir */
        fm_input_begin(w, FM_INPUT_NEWDIR, "New Dir: ");
        fm_redraw(w);
    } else if (item == 3) {                        /* New C Source */
        fm_input_begin(w, FM_INPUT_NEWFILE, "New C: ");
        fm_redraw(w);
    }
}

/* ---------- 键盘事件 ---------- */

static void fm_move_cursor(struct fm_win *w, int delta, int extend) {
    if (w->list.count <= 0) return;
    int cur = w->list.cursor;
    if (cur < 0) cur = (delta > 0) ? -1 : w->list.count;
    cur += delta;
    if (cur < 0) cur = 0;
    if (cur >= w->list.count) cur = w->list.count - 1;

    if (extend) {
        w->list.cursor = cur;
        w->list.selected[cur] = 1;
        mui_list_ensure_visible(&w->list, cur);
    } else {
        mui_list_select_only(&w->list, cur);
    }
    fm_sync_scroll_value(w);
    fm_redraw(w);
}

static void fm_handle_esc_seq(struct fm_win *w, const char *seq, unsigned int mods) {
    if (strcmp(seq, "[A") == 0) {
        fm_move_cursor(w, -1, (mods & WM_MOD_SHIFT) != 0);
    } else if (strcmp(seq, "[B") == 0) {
        fm_move_cursor(w, 1, (mods & WM_MOD_SHIFT) != 0);
    } else if (strcmp(seq, "[5~") == 0) {
        mui_scroll_page(&w->scroll, -1);
        w->scroll_dirty = 0;
        fm_sync_scroll_value(w);
        fm_redraw(w);
    } else if (strcmp(seq, "[6~") == 0) {
        mui_scroll_page(&w->scroll, 1);
        w->scroll_dirty = 0;
        fm_sync_scroll_value(w);
        fm_redraw(w);
    } else if (strcmp(seq, "[3~") == 0) {
        if (!w->esc_was_ui) fm_delete_selected(w);
    } else if (strcmp(seq, "[C") == 0) {
        if (w->input_mode != FM_INPUT_NONE) {
            mui_textbox_move_cursor(&w->input_box, 1);
            fm_redraw_topbar(w);
        }
    } else if (strcmp(seq, "[D") == 0) {
        if (w->input_mode != FM_INPUT_NONE) {
            mui_textbox_move_cursor(&w->input_box, -1);
            fm_redraw_topbar(w);
        }
    } else if (strcmp(seq, "[H") == 0) {
        if (w->input_mode != FM_INPUT_NONE) {
            mui_textbox_cursor_home(&w->input_box);
            fm_redraw_topbar(w);
        } else {
            if (w->list.count > 0) mui_list_select_only(&w->list, 0);
            fm_sync_scroll_value(w);
            fm_redraw(w);
        }
    } else if (strcmp(seq, "[F") == 0) {
        if (w->input_mode != FM_INPUT_NONE) {
            mui_textbox_cursor_end(&w->input_box);
            fm_redraw_topbar(w);
        } else {
            if (w->list.count > 0) mui_list_select_only(&w->list, w->list.count - 1);
            fm_sync_scroll_value(w);
            fm_redraw(w);
        }
    }
}


/* ---------- Desktop-side pinyin IME for file-name fields ---------- */
static int  g_ime_on, g_ime_len, g_ime_show;
static char g_ime_buf[32];

static struct mui_textbox *fm_ime_target(struct fm_win *w) {
    if (!w) return NULL;
    if (w->dialog.active) return &w->dialog.input;
    if (w->input_mode != FM_INPUT_NONE) return &w->input_box;
    return NULL;
}

static void ime_hint_build(char *out, int size) {
    if (!out || size <= 0) return;
    out[0] = '\0';
    if (!g_ime_on || !g_ime_show) return;
    if (!g_ime_len) {
        snprintf(out, size, "IME: pinyin");
        return;
    }
    int n = snprintf(out, size, "IME: %s", g_ime_buf);
    const struct ime_word *word = ime_lookup_pinyin(g_ime_buf);
    if (word) for (int i = 0; i < 3 && word->cand[i]; i++) {
        if (n >= size - 1) break;
        n += snprintf(out + n, size - n, " %d:%s", i + 1, word->cand[i]);
    }
}

static int ime_handle_key(int key, unsigned int mods,
                          struct mui_textbox *target) {
    if (key == ' ' && (mods & WM_MOD_CTRL)) {
        if (!target) return 0;
        g_ime_on = !g_ime_on;
        g_ime_len = 0;
        g_ime_buf[0] = '\0';
        g_ime_show = g_ime_on;
        return 1;
    }
    if (!g_ime_on) return 0;
    if (key >= 'A' && key <= 'Z') key = key - 'A' + 'a';
    if (key >= 'a' && key <= 'z') {
        if (g_ime_len < (int)sizeof(g_ime_buf) - 1) {
            g_ime_buf[g_ime_len++] = (char)key;
            g_ime_buf[g_ime_len] = '\0';
        }
        g_ime_show = 1;
        return 1;
    }
    if (key == '\b' || key == 127) {
        if (!g_ime_len) return 0;
        g_ime_buf[--g_ime_len] = '\0';
        g_ime_show = 1;
        return 1;
    }
    if (key == ' ' || key == '\r' || key == '\n') {
        if (!g_ime_len) return 0;
        const struct ime_word *word = ime_lookup_pinyin(g_ime_buf);
        if (word && word->cand[0]) {
            mui_textbox_insert_utf8(target, word->cand[0]);
            g_ime_len = 0;
            g_ime_buf[0] = '\0';
            g_ime_show = 0;
        }
        return 1;
    }
    if (key >= '1' && key <= '9') {
        if (!g_ime_len) return 0;
        const struct ime_word *word = ime_lookup_pinyin(g_ime_buf);
        int pick = key - '1';
        if (word && pick < 5 && word->cand[pick]) {
            mui_textbox_insert_utf8(target, word->cand[pick]);
            g_ime_len = 0;
            g_ime_buf[0] = '\0';
            g_ime_show = 0;
        }
        return 1;
    }
    if (key == 27) {
        if (!g_ime_len && !g_ime_show) return 0;
        g_ime_len = 0;
        g_ime_buf[0] = '\0';
        g_ime_show = 0;
        return 1;
    }
    return 0;
}

static int desk_ime_key(struct fm_win *w, int key, unsigned int mods) {
    struct mui_textbox *target = fm_ime_target(w);
    if (!target) return 0;
    if (!ime_handle_key(key, mods, target)) return 0;

    char hint[96];
    ime_hint_build(hint, sizeof(hint));
    if (w->dialog.active) {
        strncpy(w->dialog.error, hint, sizeof(w->dialog.error) - 1);
        w->dialog.error[sizeof(w->dialog.error) - 1] = '\0';
        fm_redraw(w);
    } else {
        strncpy(w->status, hint, sizeof(w->status) - 1);
        w->status[sizeof(w->status) - 1] = '\0';
        fm_redraw_topbar(w);
    }
    return 1;
}

static void fm_handle_key(struct fm_win *w, int key, unsigned int mods) {
    if (desk_ime_key(w, key, mods)) return;
    /* Rename 对话框（mui_dialog）：键盘模态 */
    if (w->dialog.active) {
        mui_dialog_key(&w->dialog, key, g_tick);
        if (!w->dialog.active) {
            if (w->dialog.result > 0) {
                fm_refresh(w);
                fm_set_status(w, "Renamed");
                desktop_refresh_icons();
            }
            fm_redraw(w);
        } else {
            fm_redraw(w);
        }
        return;
    }

    /* Ctrl+H：Safeguard 显示/隐藏编译产物 */
    if (key == 8 && (mods & WM_MOD_CTRL)) {
        toggle_safeguard();
        return;
    }

    /* 解析 ESC 序列（方向键 / Delete / PageUp / PageDown） */
    if (w->esc_state) {
        if (w->esc_state == 1) {
            if (key == '[') {
                w->esc_state = 2;
                w->esc_len = 1;
                w->esc_buf[0] = '[';
                w->esc_buf[1] = '\0';
                return;
            }
            w->esc_state = 0;              /* 不是序列：继续按普通键处理 */
        } else {
            if (w->esc_len < (int)sizeof(w->esc_buf) - 1) {
                w->esc_buf[w->esc_len++] = (char)key;
                w->esc_buf[w->esc_len] = '\0';
            }
            if (key == '~' || (key >= 'A' && key <= 'Z')) {
                w->esc_state = 0;
                fm_handle_esc_seq(w, w->esc_buf, mods);
                return;
            }
            return;
        }
    }

    if (key == 27) {                       /* Esc */
        w->esc_state = 1;
        w->esc_len = 0;
        w->esc_buf[0] = '\0';
        w->esc_was_ui = (w->input_mode != FM_INPUT_NONE || w->menu_open);
        if (w->input_mode != FM_INPUT_NONE) fm_input_reset(w);
        else if (w->menu_open) { w->menu_open = 0; }
        fm_redraw(w);
        return;
    }

    w->status[0] = '\0';

    if (key == '\r' || key == '\n') {      /* Enter */
        if (w->menu_open) { w->menu_open = 0; fm_redraw(w); return; }
        if (w->input_mode != FM_INPUT_NONE) fm_input_commit(w);
        else fm_open_selected(w, (mods & WM_MOD_CTRL) != 0);
        return;
    }

    if (key == '\b' || key == 127) {       /* Backspace */
        if (w->input_mode != FM_INPUT_NONE &&
            mui_textbox_get_text(&w->input_box)[0] != '\0') {
            if (w->input_box.base.on_key)
                w->input_box.base.on_key(&w->input_box.base, key);
            fm_redraw_topbar(w);
            return;
        }
        fm_input_reset(w);
        fm_go_parent(w);                   /* 回到上一级目录 */
        return;
    }

    if (key >= 32 && key < 127) {          /* 可打印字符 */
        int was_input = (w->input_mode != FM_INPUT_NONE);
        if (!was_input) {
            w->menu_open = 0;
            mui_list_clear_selection(&w->list);
            w->list.cursor = -1;
            fm_input_begin(w, FM_INPUT_PATH, "Go to: ");
        }
        if (w->input_box.base.on_key)
            w->input_box.base.on_key(&w->input_box.base, key);

        if (was_input) fm_redraw_topbar(w);
        else           fm_redraw(w);       /* 首次进入输入：同时清掉选择和菜单 */
    }
}

static struct fm_win *open_folder_window(const char *path) {
    struct fm_win *w = NULL;

    /* 优先使用空闲槽位：允许多个 Files 窗口同时打开 */
    for (int i = 0; i < MAX_FM_WINDOWS; i++) {
        if (!g_fm_wins[i].used) { w = &g_fm_wins[i]; break; }
    }
    if (!w) {
        /* 槽位满：复用第一个窗口 */
        w = &g_fm_wins[0];
    }

    if (!w->used) {
        memset(w, 0, sizeof(*w));
        w->cw = 400; w->ch = 300;
        w->last_click_idx = -1;
        w->menu_target = -1;
        snprintf(w->shm_name, sizeof(w->shm_name), "fm-%d-%d",
                 (int)(w - g_fm_wins), ++g_fm_shm_gen);
        char ev_name[24];
        snprintf(ev_name, sizeof(ev_name), "fm-ev-%d", (int)(w - g_fm_wins));

        char title[32];
        const char *base = fm_path_base(path);
        if (base && base[0]) snprintf(title, sizeof(title), "Files - %s", base);
        else                 snprintf(title, sizeof(title), "Files");

        if (wm_create_window(title, WM_KIND_NORMAL, 0, 0, w->cw, w->ch,
                             &w->pixels, &w->hdr, w->shm_name, ev_name, &w->ev) < 0) {
            return NULL;
        }
        w->used = 1;
        w->map_size = shm_total_size(w->cw, w->ch);

        /* methylui 组件：列表 + 滚动条 + 输入框 + 重命名对话框 */
        mui_list_init(&w->list, 0, FM_TOP_BAR_H,
                      w->cw - FM_SCROLL_W, w->ch - FM_TOP_BAR_H, FM_ROW_H);
        w->list.scrollbar_w = 0;      /* 滚动条由 mui_scroll 独立绘制 */
        w->list.paint_icon = fm_paint_icon;

        mui_scroll_init(&w->scroll, w->cw - FM_SCROLL_W, FM_TOP_BAR_H,
                        FM_SCROLL_W, w->ch - FM_TOP_BAR_H);
        w->scroll.base.user = w;
        w->scroll.on_change = fm_scroll_on_change;

        mui_textbox_init(&w->input_box, 0, 0, w->cw - 8);
        w->input_box.base.focused = 0;

        mui_dialog_init(&w->dialog);
    }

    fm_navigate(w, path);
    return w;
}

static void close_fm_window(struct fm_win *w) {
    if (!w->used) return;
    struct wm_destroy_request dr;
    memset(&dr, 0, sizeof(dr));
    dr.magic = WM_MAGIC; dr.type = WM_REQ_DESTROY;
    dr.owner_pid = getpid();
    strncpy(dr.shm_name, w->shm_name, sizeof(dr.shm_name) - 1);
    pipe_write(g_inbox_w, &dr, sizeof(dr));

    munmap((unsigned int)w->hdr,
           w->map_size ? w->map_size : shm_total_size(w->cw, w->ch));
    if (w->ev >= 0) ipc_close(w->ev);
    if (g_drag_src_fm == (int)(w - g_fm_wins)) drag_clear();
    w->ev = -1;
    w->hdr = NULL;
    w->pixels = NULL;
    w->used = 0;
}

static void fm_resize_window(struct fm_win *w, int new_cw, int new_ch) {
    if (new_cw <= 0 || new_ch <= 0) return;
    if (new_cw > 2048 || new_ch > 1536) return;
    if (new_cw == w->cw && new_ch == w->ch) return;

    char new_shm[16];
    snprintf(new_shm, sizeof(new_shm), "fm-%d-%d",
             (int)(w - g_fm_wins), ++g_fm_shm_gen);

    int shm_size = shm_total_size(new_cw, new_ch);
    int shm_id = shm_open(new_shm, shm_size, 0);
    if (shm_id < 0) return;

    unsigned int addr = mmap(0, shm_size, PROT_READ | PROT_WRITE, MAP_SHARED, shm_id, 0);
    if ((int)addr <= 0) return;

    struct shm_hdr *new_hdr = (struct shm_hdr*)addr;
    unsigned int *new_pixels = (unsigned int*)(addr + SHM_HDR_SIZE);
    for (int i = 0; i < new_cw * new_ch; i++) new_pixels[i] = 0x1A1A1E;
    new_hdr->counter = 0;
    new_hdr->cur_x0 = SHM_DIRTY_EMPTY; new_hdr->cur_y0 = 0;
    new_hdr->cur_x1 = 0; new_hdr->cur_y1 = 0;
    new_hdr->pub_x0 = SHM_DIRTY_EMPTY; new_hdr->pub_y0 = 0;
    new_hdr->pub_x1 = 0; new_hdr->pub_y1 = 0;

    struct wm_resize_request req;
    memset(&req, 0, sizeof(req));
    req.magic = WM_MAGIC;
    req.type = WM_REQ_RESIZE;
    req.owner_pid = getpid();
    strncpy(req.old_shm, w->shm_name, sizeof(req.old_shm) - 1);
    strncpy(req.new_shm, new_shm, sizeof(req.new_shm) - 1);
    req.new_w = new_cw;
    req.new_h = new_ch;
    pipe_write(g_inbox_w, &req, sizeof(req));

    munmap((unsigned int)w->hdr,
           w->map_size ? w->map_size : shm_total_size(w->cw, w->ch));

    w->cw = new_cw;
    w->ch = new_ch;
    w->map_size = shm_size;
    w->pixels = new_pixels;
    w->hdr = new_hdr;
    strncpy(w->shm_name, new_shm, sizeof(w->shm_name) - 1);

    w->list.base.w = new_cw - FM_SCROLL_W;
    w->list.base.h = new_ch - FM_TOP_BAR_H;
    mui_list_scroll(&w->list, 0);   /* 夹紧滚动位置 */
    fm_sync_scroll(w);
    w->menu_open = 0;       /* 旧坐标失效 */
    fm_redraw(w);
    /* 确保整个新画布都发布给 WM，避免右侧/底部残留黑边。 */
    shm_mark_rect(w->hdr, 0, 0, w->cw, w->ch);
    shm_publish(w->hdr);
}

static void handle_fm_event(struct fm_win *w, struct wm_event *ev) {
    if (ev->type == WM_EV_CLOSE_REQUEST) {
        close_fm_window(w);
        return;
    }

    if (ev->type == WM_EV_RESIZE) {
        fm_resize_window(w, (int)ev->code, (int)ev->extra);
        return;
    }

    if (ev->type == WM_EV_FOCUS_OUT) {
        if (w->dialog.active) {
            mui_dialog_close(&w->dialog);
            fm_redraw(w);
        }
        if (w->menu_open) {
            w->menu_open = 0;
            fm_redraw(w);
        }
        return;
    }

    if (ev->type == WM_EV_KEY) {
        fm_handle_key(w, (int)ev->code, ev->extra);
        return;
    }

    /* 对话框打开时：鼠标事件不改变后面的列表/选择 */
    if (w->dialog.active &&
        (ev->type == WM_EV_MOUSE_DOWN || ev->type == WM_EV_MOUSE_UP ||
         ev->type == WM_EV_MOUSE_MOVE || ev->type == WM_EV_MOUSE_LEAVE ||
         ev->type == WM_EV_WHEEL || ev->type == WM_EV_DROP)) {
        return;
    }

    /* 鼠标滚轮：mui_scroll 滚动列表 */
    if (ev->type == WM_EV_WHEEL) {
        if (!w->menu_open) {
            mui_scroll_by(&w->scroll, (int)ev->code);
            w->scroll_dirty = 0;
            fm_sync_scroll_value(w);
            w->list.hover = mui_list_hit(&w->list, ev->x, ev->y - FM_TOP_BAR_H);
            fm_redraw_scroll(w);
        }
        return;
    }

    /* 其它窗口拖过来的文件：移动到本窗口目录 */
    if (ev->type == WM_EV_DROP) {
        fm_drop_to_dir(w, w->path);
        return;
    }

    if (ev->type == WM_EV_MOUSE_MOVE) {
        if (w->menu_open) {
            int h = fm_menu_hit(w, ev->x, ev->y);
            if (h != w->menu_hover) {
                w->menu_hover = h;
                fm_redraw_menu(w);
            }
            return;
        }

        int over_scroll = (ev->x >= w->cw - FM_SCROLL_W &&
                           ev->y >= FM_TOP_BAR_H);
        if (over_scroll || w->scroll.dragging) {
            if (over_scroll && !w->scroll.base.hover) {
                w->scroll.base.hover = 1;
                fm_setup_surface(w);
                fm_paint_scroll(w);
                fm_publish(w);
            }

            if (w->scroll.base.on_mouse_move)
                w->scroll.base.on_mouse_move(&w->scroll.base,
                                             ev->x - w->scroll.base.x,
                                             ev->y - w->scroll.base.y);

            if (w->scroll_dirty) {
                w->scroll_dirty = 0;
                fm_sync_scroll_value(w);
                fm_redraw_scroll(w);
            }

            /* 指针在滚动条上：清除列表 hover */
            if (w->list.hover >= 0) {
                int old = w->list.hover;
                w->list.hover = -1;
                fm_setup_surface(w);
                fm_paint_row(w, old);
                fm_publish(w);
            }
            return;
        }

        if (w->scroll.base.hover) {
            w->scroll.base.hover = 0;
            fm_setup_surface(w);
            fm_paint_scroll(w);
            fm_publish(w);
        }

        if (w->drag_candidate && (ev->code & WM_BTN_LEFT) && !w->drag_moved) {
            int dx = ev->x - w->drag_start_x;
            int dy = ev->y - w->drag_start_y;
            if (dx * dx + dy * dy >= 36) {
                w->drag_moved = 1;
                g_drag_armed = 1;
            }
        }

        int hit = mui_list_hit(&w->list, ev->x, ev->y - FM_TOP_BAR_H);
        if (hit != w->list.hover) {
            int old = w->list.hover;
            w->list.hover = hit;
            fm_redraw_hover(w, old, hit);
        }
        return;
    }

    if (ev->type == WM_EV_MOUSE_LEAVE) {
        fm_setup_surface(w);
        int old = w->list.hover;
        w->list.hover = -1;
        if (old >= 0) fm_paint_row(w, old);

        if (w->scroll.base.hover && !w->scroll.dragging) {
            w->scroll.base.hover = 0;
            fm_paint_scroll(w);
        }
        if (w->menu_open) {
            w->menu_hover = -1;
            draw_fm_menu(w);
        }
        fm_publish(w);
        return;
    }

    if (ev->type == WM_EV_MOUSE_UP && (ev->code & WM_BTN_LEFT)) {
        if (w->scroll.dragging) {
            if (w->scroll.base.on_mouse_up)
                w->scroll.base.on_mouse_up(&w->scroll.base,
                                           ev->x - w->scroll.base.x,
                                           ev->y - w->scroll.base.y, 1);
            w->scroll_dirty = 0;
            fm_setup_surface(w);
            fm_paint_scroll(w);
            fm_publish(w);
            w->drag_candidate = 0;
            return;
        }

        w->drag_candidate = 0;
        if (g_drag_active && g_drag_src_fm == (int)(w - g_fm_wins)) {
            if (ev->extra & WM_MOD_DROP_OUTSIDE) {
                /* 释放到别的窗口：等 WM 把 WM_EV_DROP 发给目标窗口 */
            } else if (!w->drag_moved) {
                drag_clear();          /* 只是点击 */
            } else if (ev->x >= 0 && ev->y >= 0 &&
                       ev->x < w->cw && ev->y < w->ch) {
                fm_finish_drag(w, ev->x, ev->y);
            } else {
                drag_clear();
            }
        }
        return;
    }

    if (ev->type != WM_EV_MOUSE_DOWN) return;

    /* 新的点击：放弃之前没有落点的拖拽 */
    if (g_drag_active) drag_clear();
    w->status[0] = '\0';

    unsigned int mods = ev->extra;
    int right = (ev->code & WM_BTN_RIGHT) != 0;
    int left  = (ev->code & WM_BTN_LEFT) != 0;

    /* 菜单打开时先处理菜单 */
    if (w->menu_open) {
        int item = fm_menu_hit(w, ev->x, ev->y);
        w->menu_open = 0;
        if (left && item >= 0) fm_menu_action(w, item);
        else                   fm_redraw(w);
        return;
    }

    if (w->input_mode != FM_INPUT_NONE) {
        fm_input_reset(w);
        w->status[0] = '\0';
        fm_redraw(w);
    }

    if (ev->y < FM_TOP_BAR_H) return;

    /* 竖直滚动条区域：交给 mui_scroll 处理，不改变列表选择 */
    if (ev->x >= w->cw - FM_SCROLL_W) {
        if (left) {
            w->scroll.base.hover = 1;
            if (w->scroll.base.on_mouse_down)
                w->scroll.base.on_mouse_down(&w->scroll.base,
                                             ev->x - w->scroll.base.x,
                                             ev->y - w->scroll.base.y, 1);
            if (w->scroll_dirty) {
                w->scroll_dirty = 0;
                fm_sync_scroll_value(w);
                fm_redraw_scroll(w);
            } else {
                fm_setup_surface(w);
                fm_paint_scroll(w);
                fm_publish(w);
            }
            w->drag_candidate = 0;
        }
        return;
    }

    int idx = mui_list_hit(&w->list, ev->x, ev->y - FM_TOP_BAR_H);

    if (right) {
        if (idx >= 0) {
            if (!mui_list_is_selected(&w->list, idx))
                mui_list_select_only(&w->list, idx);
            w->list.cursor = idx;
            fm_sync_scroll_value(w);
            fm_open_menu(w, idx, ev->x, ev->y);
        } else {
            mui_list_clear_selection(&w->list);
            w->list.cursor = -1;
            fm_sync_scroll_value(w);
            fm_open_menu(w, -1, ev->x, ev->y);
        }
        w->last_click_idx = -1;
        w->drag_candidate = 0;
        return;
    }

    if (!left) return;

    if (idx >= 0) {
        int shift = (mods & WM_MOD_SHIFT) != 0;
        if (shift) {
            mui_list_toggle(&w->list, idx);          /* Shift+点击：多选 */
        } else if (mui_list_is_selected(&w->list, idx) &&
                   mui_list_selected_count(&w->list) > 1) {
            w->list.cursor = idx;                    /* 多选内点击：准备拖拽 */
        } else {
            mui_list_select_only(&w->list, idx);
        }
        fm_sync_scroll_value(w);

        if (!shift && idx == w->last_click_idx &&
            g_tick - w->last_click_tick < 400) {
            w->last_click_idx = -1;
            fm_open_selected(w, (mods & WM_MOD_CTRL) != 0);
            return;
        }

        w->last_click_idx = idx;
        w->last_click_tick = g_tick;
        w->drag_candidate = 1;
        w->drag_moved = 0;
        w->drag_start_x = ev->x;
        w->drag_start_y = ev->y;
        fm_drag_begin(w);       /* 记录拖拽源；真正移动需要鼠标移动超过阈值 */
        fm_redraw(w);
    } else {
        mui_list_clear_selection(&w->list);
        w->list.cursor = -1;
        w->last_click_idx = -1;
        w->drag_candidate = 0;
        fm_sync_scroll_value(w);
        fm_redraw(w);
    }
}

/* ============================================================
 *                   系统组件
 * ============================================================ */

static void draw_wallpaper(void) {
    for (int y = 0; y < g_H; y++) {
        unsigned int c = gfx_lerp(WALL_TOP, WALL_BOTTOM, y, g_H);
        for (int x = 0; x < g_W; x++) g_wall_pixels[y * g_W + x] = c;
    }
    draw_all_files();

    if (g_desk_dialog.active) {
        struct mui_surface ws;
        memset(&ws, 0, sizeof(ws));
        ws.pixels = g_wall_pixels;
        ws.w = g_W;
        ws.h = g_H;
        ws.hdr = g_wall_hdr;
        mui_dialog_paint(&g_desk_dialog, &ws, g_W, g_H);
    }

    shm_mark_rect(g_wall_hdr, 0, 0, g_W, g_H);
    shm_publish(g_wall_hdr);
}

/* 对话框按键导致内容变化时，只重绘对话框本身（它是不透明覆盖层） */
static void desk_dialog_redraw(void) {
    if (!g_desk_dialog.active) {
        draw_wallpaper();
        return;
    }

    struct mui_surface ws;
    memset(&ws, 0, sizeof(ws));
    ws.pixels = g_wall_pixels;
    ws.w = g_W;
    ws.h = g_H;
    ws.hdr = g_wall_hdr;
    mui_dialog_paint(&g_desk_dialog, &ws, g_W, g_H);
    shm_publish(g_wall_hdr);
}

/* ============================================================
 *             Safeguard：桌面上隐藏编译产物（Ctrl+H 切换）
 * ============================================================ */

static int g_show_artifacts = 0;

static int name_ci_equal(const char *a, const char *b) {
    while (*a && *b) {
        char ca = *a, cb = *b;
        if (ca >= 'a' && ca <= 'z') ca -= 32;
        if (cb >= 'a' && cb <= 'z') cb -= 32;
        if (ca != cb) return 0;
        a++; b++;
    }
    return *a == '\0' && *b == '\0';
}

static int name_has_ext_ci(const char *name, const char *ext) {
    int n = (int)strlen(name);
    int m = (int)strlen(ext);
    if (n < m) return 0;
    return name_ci_equal(name + n - m, ext);
}

/* 编译产物：可执行文件、库/字体/图片等构建资源、boot 目录 */
static int is_build_artifact(const struct fs_entry_info *f) {
    if (f->is_exec) return 1;
    if (f->is_dir) {
        return name_ci_equal(f->name, "BOOT") ||
               name_ci_equal(f->name, "BUILD");
    }
    static const char *exts[] = { ".SO", ".HEX", ".BMP", ".O", ".ELF", ".ISO", ".IMG", ".MAP" };
    for (int i = 0; i < (int)(sizeof(exts) / sizeof(exts[0])); i++) {
        if (name_has_ext_ci(f->name, exts[i])) return 1;
    }
    return 0;
}

static void desk_load_files(void) {
    static struct fs_entry_info all[MAX_DESK_FILES];
    int n = fs_read_dir(ROOT_PATH, all, MAX_DESK_FILES);

    g_file_count = 0;
    for (int i = 0; i < n; i++) {
        if (!g_show_artifacts && is_build_artifact(&all[i])) continue;
        g_files[g_file_count++] = all[i];
    }
    calc_desk_cols();
    desk_sel_clear();
    g_last_clicked_file = -1;
}

static void toggle_safeguard(void) {
    g_show_artifacts = !g_show_artifacts;
    desk_sel_clear();
    g_last_clicked_file = -1;
    desktop_refresh_icons();
}

/* 文件系统内容变化后同步桌面图标 */
static void desktop_refresh_icons(void) {
    desk_load_files();
    desk_sel_clear();
    g_last_clicked_file = -1;
    draw_wallpaper();
}

/* 桌面图标：回车打开 / Delete 删除（与文件窗口语义一致） */
static void wall_open_selected(void) {
    int idx = g_selected_file;
    if (idx < 0 || idx >= g_file_count) idx = desk_sel_first();
    if (idx < 0 || idx >= g_file_count) return;

    char path[256];
    snprintf(path, sizeof(path), "SYS:\\%s", g_files[idx].name);

    if (g_files[idx].is_dir) {
        open_folder_window(path);
    } else if (is_c_source(g_files[idx].name)) {
        open_source_file(path);
    } else if (g_files[idx].is_exec) {
        exec_elf(path);
    }
}

static void wall_delete_selected(void) {
    if (desk_sel_count() == 0) return;

    for (int i = 0; i < g_file_count; i++) {
        if (!(g_sel_mask & (1ull << i))) continue;
        char path[256];
        snprintf(path, sizeof(path), "SYS:\\%s", g_files[i].name);
        fm_remove_recursive(path);
    }

    desk_sel_clear();
    g_last_clicked_file = -1;
    desktop_refresh_icons();
}

static int g_wall_esc_state = 0;
static int g_wall_esc_digit = 0;

/* 桌面拖拽起点 */
static int g_wall_drag_candidate = 0;
static int g_wall_drag_moved = 0;
static int g_wall_drag_x = 0, g_wall_drag_y = 0;

/* 桌面坐标 -> 图标下标，空白处返回 -1 */
static int wall_hit_icon(int mx, int my) {
    if (mx < ICON_PAD_X || my < ICON_PAD_Y) return -1;

    int col = (mx - ICON_PAD_X) / ICON_CELL_W;
    int row = (my - ICON_PAD_Y) / ICON_CELL_H;
    if (col >= g_desk_cols) return -1;

    int idx = row * g_desk_cols + col;
    if (idx < 0 || idx >= g_file_count) return -1;

    int px = ICON_PAD_X + col * ICON_CELL_W;
    int py = ICON_PAD_Y + row * ICON_CELL_H;
    int ix = px + (ICON_CELL_W - ICON_SIZE) / 2;
    int iy = py;

    if ((mx >= ix && mx < ix + ICON_SIZE && my >= iy && my < iy + ICON_SIZE) ||
        (mx >= px && mx < px + ICON_CELL_W &&
         my >= iy + ICON_SIZE + 4 && my < iy + ICON_SIZE + 20)) {
        return idx;
    }
    return -1;
}

static void wall_drag_begin(void) {
    int n = desk_collect_paths(g_drag_paths, DRAG_MAX);
    if (n <= 0) return;
    g_drag_count = n;
    g_drag_active = 1;
    g_drag_armed = 0;
    g_drag_src_fm = -1;
}

static void handle_wall_event(struct wm_event *ev) {
    /* 键盘：Ctrl+H 切换编译产物显示；Enter 打开；Delete 删除 */
    if (ev->type == WM_EV_KEY) {
        int key = (int)ev->code;

        /* Rename 对话框：键盘模态，直接交给 MUI */
        if (g_desk_dialog.active) {
            if (ime_handle_key(key, ev->extra, &g_desk_dialog.input)) {
                char hint[96];
                ime_hint_build(hint, sizeof(hint));
                strncpy(g_desk_dialog.error, hint, sizeof(g_desk_dialog.error) - 1);
                g_desk_dialog.error[sizeof(g_desk_dialog.error) - 1] = '\0';
                desk_dialog_redraw();
                return;
            }
            mui_dialog_key(&g_desk_dialog, key, g_tick);
            if (!g_desk_dialog.active) {
                if (g_desk_dialog.result > 0) desktop_refresh_icons();
                else                         draw_wallpaper();
            } else {
                desk_dialog_redraw();
            }
            return;
        }

        if (key == 8 && (ev->extra & WM_MOD_CTRL)) {
            toggle_safeguard();
            return;
        }

        if (g_wall_esc_state) {
            if (g_wall_esc_state == 1) {
                if (key == '[') { g_wall_esc_state = 2; return; }
                g_wall_esc_state = 0;
            } else if (g_wall_esc_state == 2) {
                if (key >= '0' && key <= '9') {
                    g_wall_esc_digit = key;
                    g_wall_esc_state = 3;
                } else {
                    g_wall_esc_state = 0;
                }
                return;
            } else {
                g_wall_esc_state = 0;
                if (key == '~' && g_wall_esc_digit == '3') wall_delete_selected();
                return;
            }
        }
        if (key == 27) { g_wall_esc_state = 1; return; }
        if (key == '\r' || key == '\n') wall_open_selected();
        return;
    }

    /* 对话框是键盘模态：忽略鼠标/拖放，直到 Esc/Enter */
    if (g_desk_dialog.active &&
        (ev->type == WM_EV_MOUSE_DOWN || ev->type == WM_EV_MOUSE_MOVE ||
         ev->type == WM_EV_MOUSE_UP || ev->type == WM_EV_MOUSE_LEAVE ||
         ev->type == WM_EV_WHEEL || ev->type == WM_EV_DROP)) {
        return;
    }

    /* 拖放释放到桌面：移动到根目录 */
    if (ev->type == WM_EV_DROP) {
        if (g_drag_active) fm_drop_to_dir(NULL, ROOT_PATH);
        return;
    }

    if (ev->type == WM_EV_MOUSE_MOVE) {
        if (g_wall_drag_candidate && (ev->code & WM_BTN_LEFT) && !g_wall_drag_moved) {
            int dx = ev->x - g_wall_drag_x;
            int dy = ev->y - g_wall_drag_y;
            if (dx * dx + dy * dy >= 36) {
                g_wall_drag_moved = 1;
                g_drag_armed = 1;
            }
        }
        return;
    }

    if (ev->type == WM_EV_MOUSE_UP && (ev->code & WM_BTN_LEFT)) {
        g_wall_drag_candidate = 0;
        if (g_drag_active && g_drag_src_fm < 0) {
            if (!(ev->extra & WM_MOD_DROP_OUTSIDE)) {
                drag_clear();   /* 落回桌面：源就在根目录，无需移动 */
            }
            /* 释放到其它窗口：等 WM 把 WM_EV_DROP 发给目标窗口 */
        }
        g_wall_drag_moved = 0;
        return;
    }

    if (ev->type == WM_EV_MOUSE_DOWN && (ev->code & WM_BTN_RIGHT)) {
        if (g_drag_active) drag_clear();
        if (g_menu_visible) return;

        int idx = wall_hit_icon(ev->x, ev->y);
        uint64_t old = g_sel_mask;

        if (idx >= 0) {
            if (!desk_is_selected(idx)) desk_sel_only(idx);
            else                        g_selected_file = idx;
        } else {
            desk_sel_clear();
        }
        redraw_selection_changed(old);
        g_last_clicked_file = -1;

        if (idx >= 0) open_context_item_menu(idx, ev->x, ev->y);
        else          open_context_menu(ev->x, ev->y);
        return;
    }

    
    if (ev->type != WM_EV_MOUSE_DOWN || ev->code != 1) return;
    if (g_drag_active) drag_clear();

    int hit = wall_hit_icon(ev->x, ev->y);
    int shift = (ev->extra & WM_MOD_SHIFT) != 0;
    int ctrl  = (ev->extra & WM_MOD_CTRL) != 0;
    int toggle = shift || ctrl;           /* Shift 或 Ctrl 点击都可多选 */
    uint64_t old = g_sel_mask;

    if (hit >= 0) {
        if (toggle) {
            desk_sel_toggle(hit);
            g_last_clicked_file = -1;       /* 多选点击不参与双击判定 */
        } else if (!desk_is_selected(hit)) {
            desk_sel_only(hit);
        } else if (desk_sel_count() <= 1) {
            desk_sel_only(hit);
        } else {
            g_selected_file = hit;          /* 多选内部点击：保留选择，准备拖拽 */
        }
    } else {
        desk_sel_clear();
    }
    redraw_selection_changed(old);

    if (hit >= 0 && desk_is_selected(hit)) {
        g_wall_drag_candidate = 1;
        g_wall_drag_moved = 0;
        g_wall_drag_x = ev->x;
        g_wall_drag_y = ev->y;
        wall_drag_begin();       /* 记录全部选中项，拖到文件窗口即可批量移动 */

        if (!toggle && hit == g_last_clicked_file &&
            g_tick - g_click_tick < 400) {
            char path[256];
            snprintf(path, sizeof(path), "SYS:\\%s", g_files[hit].name);

            if (g_files[hit].is_dir) {
                open_folder_window(path);
            } else if (is_c_source(g_files[hit].name)) {
                open_source_file(path);
            } else if (g_files[hit].is_exec) {
                exec_elf(path);
            }

            uint64_t cur = g_sel_mask;
            desk_sel_clear();
            redraw_selection_changed(cur);
            g_last_clicked_file = -1;
            g_wall_drag_candidate = 0;
        } else {
            g_last_clicked_file = toggle ? -1 : hit;
            g_click_tick = g_tick;
        }
    } else {
        g_wall_drag_candidate = 0;
        g_last_clicked_file = -1;
    }
}

static void draw_window_items(void) {
    int x = WIN_ITEM_X0;
    for (int i = 0; i < g_win_count; i++) {
        if (x + WIN_ITEM_W > g_W - 80) break;
        unsigned int bg = g_win_list[i].minimized ? 0x1A1A2E : 0x3A4A5E;
        gfx_fill_rect(g_panel_pixels, g_W, PANEL_H, x, 0, WIN_ITEM_W - 4, PANEL_H, bg);

        char label[64];
        int k = 0, used = 0;
        const unsigned char *tp = (const unsigned char*)g_win_list[i].title;
        while (*tp && k < (int)sizeof(label) - 4) {
            uint32_t cp;
            int clen = ufont_utf8_decode(tp, &cp);
            int cw = wm_font_width((int)cp);
            if (!cw) cw = 8;
            if (used + cw > WIN_ITEM_W - 12) break;
            memcpy(label + k, tp, clen);
            k += clen; used += cw; tp += clen;
        }
        label[k] = '\0';
        gfx_draw_text(g_panel_pixels, g_W, x + 4, (PANEL_H - 16) / 2, label, COL_PANEL_FG, bg);
        x += WIN_ITEM_W;
    }
}

static void draw_clock_region(void) {
    int x0 = g_W - 80; if (x0 < 0) x0 = 0;
    gfx_fill_rect(g_panel_pixels, g_W, PANEL_H, x0, 0, g_W - x0, PANEL_H, COL_PANEL);

    struct rtc_time t;
    if (rtc_get_time(&t) == 0) {
        char buf[16];
        snprintf(buf, sizeof(buf), "%02d:%02d:%02d", t.hour, t.minute, t.second);
        gfx_draw_text(g_panel_pixels, g_W, x0 + 4, (PANEL_H - 16) / 2, buf, COL_PANEL_FG, COL_PANEL);
    }
}

static void draw_panel(void) {
    gfx_fill_rect(g_panel_pixels, g_W, PANEL_H, 0, 0, g_W, PANEL_H, COL_PANEL);
    gfx_draw_text(g_panel_pixels, g_W, 8, (PANEL_H - 16) / 2, "Start", COL_PANEL_FG, COL_PANEL);
    draw_window_items();
    draw_clock_region();
    shm_mark_rect(g_panel_hdr, 0, 0, g_W, PANEL_H);
    shm_publish(g_panel_hdr);
}

static void update_clock(void) {
    draw_clock_region();
    int x0 = g_W - 80; if (x0 < 0) x0 = 0;
    shm_mark_rect(g_panel_hdr, x0, 0, g_W, PANEL_H);
    shm_publish(g_panel_hdr);
}

#define MENU_STATE_MAIN    0
#define MENU_STATE_POWER   1
#define MENU_STATE_CONTEXT 2
#define MENU_STATE_CONTEXT_ITEM 3
#define MENU_STATE_CONTEXT_MULTI 4
static int g_menu_state = MENU_STATE_MAIN;

#define MAIN_MENU_ITEMS 3
static const char *g_main_labels[MAIN_MENU_ITEMS] = { "Power Options", "Terminal", "About" };
#define POWER_MENU_ITEMS 2
static const char *g_power_labels[POWER_MENU_ITEMS] = { "  Reset", "  Power Off" };
#define CONTEXT_MENU_ITEMS 4
#define CONTEXT_ITEM_MAX_ITEMS 5
static const char *g_context_labels[CONTEXT_MENU_ITEMS] =
    { "\xE5\x88\xB7\xE6\x96\xB0", "\xE7\xB2\x98\xE8\xB4\xB4", "\xE6\x96\xB0\xE5\xBB\xBA\xE6\x96\x87\xE4\xBB\xB6\xE5\xA4\xB9", "\xE6\x96\xB0\xE5\xBB\xBA\x20\x43\x20\xE6\xBA\x90\xE6\x96\x87\xE4\xBB\xB6" };
static const char *g_context_item_labels[CONTEXT_ITEM_MAX_ITEMS] =
    { "\xE5\xA4\x8D\xE5\x88\xB6", "\xE5\x89\xAA\xE5\x88\x87", "\xE5\x88\xA0\xE9\x99\xA4", "\xE9\x87\x8D\xE5\x91\xBD\xE5\x90\x8D", "\xE7\xBC\x96\xE8\xAF\x91" };

static int g_context_file = -1;

static int context_item_count(void) {
    int idx = g_context_file;
    if (idx >= 0 && idx < g_file_count && !g_files[idx].is_dir &&
        is_c_source(g_files[idx].name))
        return CONTEXT_ITEM_MAX_ITEMS;
    return CONTEXT_MENU_ITEMS;
}
#define CONTEXT_MULTI_ITEMS 3
static const char *g_context_multi_labels[CONTEXT_MULTI_ITEMS] =
    { "\xE5\xA4\x8D\xE5\x88\xB6", "\xE5\x89\xAA\xE5\x88\x87", "\xE5\x88\xA0\xE9\x99\xA4" };

static void draw_menu(int hover) {
    gfx_fill_rect(g_menu_pixels, g_menu_w, g_menu_h, 0, 0, g_menu_w, g_menu_h, COL_MENU_BG);

    int count;
    const char **labels;
    if (g_menu_state == MENU_STATE_CONTEXT) {
        count = CONTEXT_MENU_ITEMS;
        labels = g_context_labels;
    } else if (g_menu_state == MENU_STATE_CONTEXT_ITEM) {
        count = context_item_count();
        labels = g_context_item_labels;
    } else if (g_menu_state == MENU_STATE_CONTEXT_MULTI) {
        count = CONTEXT_MULTI_ITEMS;
        labels = g_context_multi_labels;
    } else if (g_menu_state == MENU_STATE_MAIN) {
        count = MAIN_MENU_ITEMS;
        labels = g_main_labels;
    } else {
        count = POWER_MENU_ITEMS;
        labels = g_power_labels;
    }

    for (int i = 0; i < count; i++) {
        int y = MENU_PAD + i * MENU_ITEM_H;
        unsigned int bg = (i == hover) ? COL_MENU_HL : COL_MENU_BG;
        if (i == hover) gfx_fill_rect(g_menu_pixels, g_menu_w, g_menu_h, 0, y, g_menu_w, MENU_ITEM_H, bg);
        gfx_draw_text(g_menu_pixels, g_menu_w, 8, y + (MENU_ITEM_H - 16) / 2, labels[i], COL_MENU_FG, bg);
    }
    shm_mark_rect(g_menu_hdr, 0, 0, g_menu_w, g_menu_h);
    shm_publish(g_menu_hdr);
}

/* 开一个终端窗口。
 * extra 非 NULL 时把它接到 TERM 参数后面，让终端启动后直接运行该命令
 * （argv[0] 为程序路径，如 {"KILO", "SYS:\\kmain.c", NULL}）；
 * extra 为 NULL 时终端里跑交互 shell。 */
static void spawn_term_argv(int cw, int ch, int x, int y,
                            char *const extra[], const char *title) {
    static int term_slot = 0;
    char shm_name[16], ev_name[24];
    snprintf(shm_name, sizeof(shm_name), "term-%d", term_slot);
    snprintf(ev_name, sizeof(ev_name), "wm-term-%d", term_slot);

    int shm_size = shm_total_size(cw, ch);
    int shm_id = shm_open(shm_name, shm_size, 0);
    if (shm_id < 0) return;

    pipe_unlink(ev_name);
    int fds[2];
    if (pipe_named(ev_name, fds) < 0) return;
    ipc_close(fds[0]); ipc_close(fds[1]);

    struct wm_request req;
    memset(&req, 0, sizeof(req));
    req.magic = WM_MAGIC; req.type = WM_REQ_CREATE; req.kind = WM_KIND_NORMAL;
    req.x = x; req.y = y; req.w = cw; req.h = ch; req.owner_pid = getpid();
    strncpy(req.shm_name, shm_name, sizeof(req.shm_name) - 1);
    strncpy(req.ev_name, ev_name, sizeof(req.ev_name) - 1);
    strncpy(req.title, title ? title : "Terminal", sizeof(req.title) - 1);
    if (pipe_write(g_inbox_w, &req, sizeof(req)) != sizeof(req)) return;

    char arg_w[8], arg_h[8];
    snprintf(arg_w, sizeof(arg_w), "%d", cw);
    snprintf(arg_h, sizeof(arg_h), "%d", ch);

    char *argv[8];
    int n = 0;
    argv[n++] = "TERM";
    argv[n++] = shm_name;
    argv[n++] = arg_w;
    argv[n++] = arg_h;
    argv[n++] = ev_name;
    if (extra) {
        for (int i = 0; extra[i] && n < (int)(sizeof(argv) / sizeof(argv[0])) - 1; i++)
            argv[n++] = extra[i];
    }
    argv[n] = NULL;

    struct spawn_params sp = {
        .size = sizeof(sp), .in_fd = SPAWN_FD_INHERIT, .out_fd = SPAWN_FD_INHERIT, .err_fd = SPAWN_FD_INHERIT,
        .in_pipe = -1, .out_pipe = -1, .err_pipe = -1, .in_pty = -1, .out_pty = -1, .err_pty = -1,
        .flags = 0, .envp = 0, .out_pid = 0, .child_class = CLASS_USER, .child_caps = 0,
    };
    spawn("TERM", argv, &sp);
    term_slot++;
}

static void spawn_term(int cw, int ch, int x, int y) {
    spawn_term_argv(cw, ch, x, y, NULL, "Terminal");
}

/* 用 kilo 打开 C 源文件：开一个终端窗口，里面直接跑 KILO <path> */
static void open_source_file(const char *path) {
    if (!path || !path[0]) return;

    const char *base = path;
    for (const char *p = path; *p; p++) {
        if (*p == '\\') base = p + 1;
    }
    char title[32];
    snprintf(title, sizeof(title), "Kilo - %s", base);

    char *extra[] = { "KILO", (char*)path, NULL };
    spawn_term_argv(640, 480, 160, 80, extra, title);
}

/* 桌面多选：全部选中项写入剪贴板 */
static void desk_copy_selection(int cut) {
    int n = desk_collect_paths(g_clip_paths, CLIP_MAX);
    if (n <= 0) return;
    g_clip_count = n;
    g_clip_cut = cut;
}

/* 桌面右键：把剪贴板内容粘贴到 SYS:\ */
static void desktop_paste(void) {
    if (g_clip_count <= 0) return;

    int cut = g_clip_cut;
    int done = 0;

    for (int i = 0; i < g_clip_count; i++) {
        const char *src = g_clip_paths[i];
        const char *base = fm_path_base(src);
        if (!base[0]) continue;
        if (fm_path_is_inside(ROOT_PATH, src)) continue;

        char src_dir[256];
        fm_path_parent(src, src_dir, sizeof(src_dir));
        if (cut && strcmp(src_dir, ROOT_PATH) == 0) continue;

        char dst[256];
        fm_unique_path(ROOT_PATH, base, dst, sizeof(dst));

        int r = cut ? fs_rename(src, dst) : fm_copy_recursive(src, dst);
        if (r >= 0) done++;
    }

    if (cut && done > 0) g_clip_count = 0;
    desktop_refresh_icons();
}

/* 桌面右键新建：打开根目录 Files 窗口并进入输入状态（Rename 已改走 MUI 对话框） */
static void desktop_open_fm_input(int mode) {
    struct fm_win *w = open_folder_window(ROOT_PATH);
    if (!w) return;

    fm_input_begin(w, mode, (mode == FM_INPUT_NEWDIR) ? "New Dir: " : "New C: ");
    fm_redraw(w);
}

static void open_start_menu(void) {
    if (g_menu_visible) return;
    g_menu_state = MENU_STATE_MAIN;
    g_menu_w = MENU_W; g_menu_h = MAIN_MENU_ITEMS * MENU_ITEM_H + MENU_PAD * 2;
    strcpy(g_menu_shm, "menu-shm");
    char ev_name[24]; strcpy(ev_name, "menu-ev");

    if (wm_create_window("", WM_KIND_POPUP, 4, g_H - PANEL_H - g_menu_h, g_menu_w, g_menu_h,
                         &g_menu_pixels, &g_menu_hdr, g_menu_shm, ev_name, &g_menu_ev) < 0) return;
    draw_menu(-1);
    g_menu_visible = 1;
}

static void open_context_menu(int x, int y) {
    if (g_menu_visible) return;
    g_menu_state = MENU_STATE_CONTEXT;
    g_context_file = -1;
    g_menu_w = MENU_W;
    g_menu_h = CONTEXT_MENU_ITEMS * MENU_ITEM_H + MENU_PAD * 2;
    strcpy(g_menu_shm, "menu-shm");
    char ev_name[24];
    strcpy(ev_name, "menu-ev");

    /* 夹紧到屏幕内，避免菜单超出边界 */
    if (x + g_menu_w > g_W) x = g_W - g_menu_w;
    if (y + g_menu_h > g_H) y = g_H - g_menu_h;
    if (x < 0) x = 0;
    if (y < 0) y = 0;

    if (wm_create_window("", WM_KIND_POPUP, x, y, g_menu_w, g_menu_h,
                         &g_menu_pixels, &g_menu_hdr, g_menu_shm, ev_name, &g_menu_ev) < 0)
        return;
    draw_menu(-1);
    g_menu_visible = 1;
}

/* 桌面图标右键菜单：Copy / Cut / Delete / Rename */
static void open_context_item_menu(int file_idx, int x, int y) {
    if (g_menu_visible) return;

    int multi = desk_sel_count() > 1;
    g_menu_state = multi ? MENU_STATE_CONTEXT_MULTI : MENU_STATE_CONTEXT_ITEM;
    g_context_file = file_idx;
    g_menu_w = MENU_W;
    int items = multi ? CONTEXT_MULTI_ITEMS :
                ((file_idx >= 0 && file_idx < g_file_count &&
                  !g_files[file_idx].is_dir && is_c_source(g_files[file_idx].name))
                 ? CONTEXT_ITEM_MAX_ITEMS : CONTEXT_MENU_ITEMS);
    g_menu_h = items * MENU_ITEM_H + MENU_PAD * 2;
    strcpy(g_menu_shm, "menu-shm");
    char ev_name[24];
    strcpy(ev_name, "menu-ev");

    if (x + g_menu_w > g_W) x = g_W - g_menu_w;
    if (y + g_menu_h > g_H) y = g_H - g_menu_h;
    if (x < 0) x = 0;
    if (y < 0) y = 0;

    if (wm_create_window("", WM_KIND_POPUP, x, y, g_menu_w, g_menu_h,
                         &g_menu_pixels, &g_menu_hdr, g_menu_shm, ev_name, &g_menu_ev) < 0)
        return;
    draw_menu(-1);
    g_menu_visible = 1;
}

static void close_menu_request(void) {
    if (!g_menu_visible) return;
    struct wm_destroy_request dr;
    dr.magic = WM_MAGIC; dr.type = WM_REQ_DESTROY; dr.owner_pid = getpid();
    strcpy(dr.shm_name, g_menu_shm);
    pipe_write(g_inbox_w, &dr, sizeof(dr));
}

static int menu_item_count(void) {
    if (g_menu_state == MENU_STATE_CONTEXT) return CONTEXT_MENU_ITEMS;
    if (g_menu_state == MENU_STATE_CONTEXT_ITEM) return context_item_count();
    if (g_menu_state == MENU_STATE_CONTEXT_MULTI) return CONTEXT_MULTI_ITEMS;
    if (g_menu_state == MENU_STATE_MAIN)    return MAIN_MENU_ITEMS;
    return POWER_MENU_ITEMS;
}

static void handle_menu_event(struct wm_event *ev) {
    if (ev->type == WM_EV_CLOSE) {
        if (g_menu_hdr) { munmap((unsigned int)g_menu_hdr, shm_total_size(g_menu_w, g_menu_h)); g_menu_hdr = NULL; g_menu_pixels = NULL; }
        shm_unlink(g_menu_shm); pipe_unlink("menu-ev");
        if (g_menu_ev >= 0) { ipc_close(g_menu_ev); g_menu_ev = -1; }
        g_menu_visible = 0;
        return;
    }
    if (ev->type == WM_EV_MOUSE_MOVE) {
        int rel = ev->y - MENU_PAD;
        int item = (rel >= 0) ? rel / MENU_ITEM_H : -1;
        int count = menu_item_count();
        draw_menu((item >= 0 && item < count) ? item : -1);
        return;
    }
    if (ev->type == WM_EV_MOUSE_DOWN && (ev->code & WM_BTN_LEFT)) {
        int rel = ev->y - MENU_PAD;
        int item = (rel >= 0) ? rel / MENU_ITEM_H : -1;
        int count = menu_item_count();

        if (item >= 0 && item < count) {
            if (g_menu_state == MENU_STATE_CONTEXT) {
                if (item == 0) {
                    /* Refresh：重新读取根目录、重绘壁纸与图标 */
                    desk_sel_clear();
                    g_last_clicked_file = -1;
                    desktop_refresh_icons();
                } else if (item == 1) {
                    desktop_paste();
                } else if (item == 2) {
                    desktop_open_fm_input(FM_INPUT_NEWDIR);
                } else if (item == 3) {
                    desktop_open_fm_input(FM_INPUT_NEWFILE);
                }
            } else if (g_menu_state == MENU_STATE_CONTEXT_MULTI) {
                if (item == 0)      desk_copy_selection(0);
                else if (item == 1) desk_copy_selection(1);
                else if (item == 2) wall_delete_selected();
            } else if (g_menu_state == MENU_STATE_CONTEXT_ITEM) {
                int idx = g_context_file;
                if (item == 0 || item == 1) {          /* Copy / Cut */
                    desk_copy_selection(item == 1);
                } else if (item == 2) {                /* Delete */
                    wall_delete_selected();
                } else if (item == 3 && idx >= 0 && idx < g_file_count) {
                    /* Rename：MUI 对话框，不打开 Files 窗口 */
                    mui_dialog_open_rename(&g_desk_dialog, ROOT_PATH,
                                           g_files[idx].name);
                    draw_wallpaper();
                } else if (item == 4) {
                    desk_compile_file(idx);
                }
            } else if (g_menu_state == MENU_STATE_MAIN) {
                if (item == 0) { g_menu_state = MENU_STATE_POWER; draw_menu(-1); return; }
                else if (item == 1) spawn_term(600, 400, 200, 100);
            } else {
                if (item == 0) sys_reboot();
                else if (item == 1) sys_halt();
            }
        }
        close_menu_request();
    }
}

static void handle_panel_event(struct wm_event *ev) {
    if (ev->type == WM_EV_WINDOW_LIST) {
        int n = (int)ev->code; if (n > 8) n = 8;
        g_win_count = n;
        for (int i = 0; i < n; i++) {
            struct wm_window_entry e;
            if (pipe_read(g_panel_ev, &e, sizeof(e)) != sizeof(e)) { g_win_count = i; break; }
            g_win_list[i].id = e.id; g_win_list[i].minimized = e.minimized;
            memcpy(g_win_list[i].title, e.title, 32);
            g_win_list[i].title[31] = '\0';
        }
        draw_panel();
        return;
    }
    if (ev->type == WM_EV_MOUSE_DOWN && ev->code == 1) {
        if (ev->x < WIN_ITEM_X0) { open_start_menu(); return; }
        if (ev->x >= WIN_ITEM_X0 && ev->x < WIN_ITEM_X0 + WIN_ITEM_W * g_win_count) {
            int idx = (ev->x - WIN_ITEM_X0) / WIN_ITEM_W;
            if (idx >= 0 && idx < g_win_count) {
                struct wm_action_request req;
                memset(&req, 0, sizeof(req));
                req.magic = WM_MAGIC; req.type = WM_REQ_WIN_ACTION; req.owner_pid = getpid();
                req.win_id = g_win_list[idx].id;
                req.action = g_win_list[idx].minimized ? WM_ACTION_RESTORE : WM_ACTION_MINIMIZE;
                pipe_write(g_inbox_w, &req, sizeof(req));
            }
        }
    }
}

/* ============================================================
 *                          main
 * ============================================================ */

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    const char *sw = getenv("SCREEN_W");
    const char *sh = getenv("SCREEN_H");
    g_W = sw ? atoi(sw) : 1024; g_H = sh ? atoi(sh) : 768;

    for (int i = 0; i < 1000; i++) {
        g_inbox_w = pipe_open(WM_INBOX_NAME, 1);
        if (g_inbox_w >= 0) break;
        sleep_ms(10);
    }
    if (g_inbox_w < 0) { printf("desktop: wm-inbox open failed\n"); return 1; }

    wm_font_load();
    mui_font_load();            /* methylui 字体（文件窗口用） */
    mui_dialog_init(&g_desk_dialog);
    calc_desk_cols();

    icons_load();

    strcpy(g_wall_shm, "wall-shm");
    char ev_wall[24]; strcpy(ev_wall, "wall-ev");
    if (wm_create_window("", WM_KIND_WALLPAPER, 0, 0, g_W, g_H,
                         &g_wall_pixels, &g_wall_hdr, g_wall_shm, ev_wall, &g_wall_ev) < 0) {
        printf("desktop: wallpaper create failed\n"); return 1;
    }

    desk_load_files();
    draw_wallpaper();

    strcpy(g_panel_shm, "panel-shm");
    char ev_panel[24]; strcpy(ev_panel, "panel-ev");
    if (wm_create_window("", WM_KIND_DOCK, 0, g_H - PANEL_H, g_W, PANEL_H,
                         &g_panel_pixels, &g_panel_hdr, g_panel_shm, ev_panel, &g_panel_ev) < 0) {
        printf("desktop: panel create failed\n"); return 1;
    }
    draw_panel(); update_clock();

    int clock_tick = 0;
    for (;;) {
        struct wm_event ev;
        while (g_panel_ev >= 0 && pipe_avail(g_panel_ev) >= (int)sizeof(ev)) {
            if (pipe_read(g_panel_ev, &ev, sizeof(ev)) != sizeof(ev)) break;
            handle_panel_event(&ev);
        }
        while (g_wall_ev >= 0 && pipe_avail(g_wall_ev) >= (int)sizeof(ev)) {
            if (pipe_read(g_wall_ev, &ev, sizeof(ev)) != sizeof(ev)) break;
            handle_wall_event(&ev);
        }
        if (g_desk_dialog.active &&
            mui_dialog_tick(&g_desk_dialog, g_tick)) {
            draw_wallpaper();
        }
        while (g_menu_ev >= 0 && pipe_avail(g_menu_ev) >= (int)sizeof(ev)) {
            if (pipe_read(g_menu_ev, &ev, sizeof(ev)) != sizeof(ev)) break;
            handle_menu_event(&ev);
        }
        for (int i = 0; i < MAX_FM_WINDOWS; i++) {
            struct fm_win *w = &g_fm_wins[i];
            if (!w->used) continue;
            while (w->ev >= 0 && pipe_avail(w->ev) >= (int)sizeof(ev)) {
                if (pipe_read(w->ev, &ev, sizeof(ev)) != sizeof(ev)) break;
                handle_fm_event(w, &ev);
            }
            if (w->used && w->dialog.active &&
                mui_dialog_tick(&w->dialog, g_tick)) {
                fm_redraw(w);
            }
            if (w->used) fm_publish(w);      /* 有延迟脏区时补发 */
        }

        /* 壁纸 / 任务栏 / 菜单的延迟脏区补发 */
        shm_publish(g_wall_hdr);
        shm_publish(g_panel_hdr);
        shm_publish(g_menu_hdr);

        if (++clock_tick >= 500) { clock_tick = 0; update_clock(); }
        g_tick++;
        sleep_ms(1);
    }
    return 0;
}