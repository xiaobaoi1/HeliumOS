#include <keyboard.h>
#include <io.h>
#include <stdint.h>
#include <stddef.h>
#include <device.h>
#include <errno.h>
#include <signal.h>
#include <input.h>
#include <irq.h>

#define KEYBOARD_DATA_PORT   0x60

static int shift_pressed = 0;
static int caps_lock = 0;
static int ctrl_pressed = 0;
static int alt_pressed = 0;

static void update_mods(void) {
    uint32_t m = 0;
    if (shift_pressed) m |= INPUT_MOD_SHIFT;
    if (ctrl_pressed)  m |= INPUT_MOD_CTRL;
    if (alt_pressed)   m |= INPUT_MOD_ALT;
    input_set_mods(m);
}

/* 扫描码转 ASCII（内部函数） */
static char scancode_to_ascii(uint8_t scancode) {
    // ===== 1. 修饰键：只更新状态，不产生字符 =====
    if (scancode == 0x2A || scancode == 0x36) { shift_pressed = 1; update_mods(); return 0; } // 左/右 Shift 按下
    if (scancode == 0xAA || scancode == 0xB6) { shift_pressed = 0; update_mods(); return 0; } // 左/右 Shift 释放
    if (scancode == 0x3A) { caps_lock = !caps_lock; return 0; }                // Caps Lock 切换
    if (scancode == 0x1D) { ctrl_pressed = 1; update_mods(); return 0; }        // Ctrl 按下
    if (scancode == 0x9D) { ctrl_pressed = 0; update_mods(); return 0; }        // Ctrl 释放
    if (scancode == 0x38) { alt_pressed = 1; update_mods(); return 0; }         // Alt 按下
    if (scancode == 0xB8) { alt_pressed = 0; update_mods(); return 0; }         // Alt 释放

    // for test ESC
    if (scancode == 0x01) return 27;

        /* Ctrl+C */
    if (ctrl_pressed && scancode == 0x2E) {
        if (input_keyboard_claimed()) {
            /* WM 模式：投递 ^C 字符，让 term 写 pty，由 pty 行规程处理 */
            /* HeliumOS 特殊性：内核 tty 与 pty 并存期间的特殊过渡逻辑 */
            struct input_event ev = {
                .type = INPUT_KEY,
                .code = 0x03,
                .x = 0, .y = 0,
                .mods = input_get_mods(),
            };
            input_post(&ev);
        } else {
            /* 内核 tty 模式：直接发信号给 tty 前台 */
            extern void signal_foreground(int sig);
            signal_foreground(SIGINT);
        }
        return 0;
    }

    // ===== 2. 忽略其他释放码 =====
    if (scancode & 0x80) return 0;

    // ===== 3. 普通键字符表（未按 Shift）=====
    static const char normal[128] = {
        [0x02] = '1', [0x03] = '2', [0x04] = '3', [0x05] = '4', [0x06] = '5',
        [0x07] = '6', [0x08] = '7', [0x09] = '8', [0x0A] = '9', [0x0B] = '0',
        [0x0C] = '-', [0x0D] = '=',
        [0x0E] = '\b', [0x0F] = '\t',
        [0x10] = 'q', [0x11] = 'w', [0x12] = 'e', [0x13] = 'r', [0x14] = 't',
        [0x15] = 'y', [0x16] = 'u', [0x17] = 'i', [0x18] = 'o', [0x19] = 'p',
        [0x1A] = '[', [0x1B] = ']',
        [0x1C] = '\r',
        [0x1E] = 'a', [0x1F] = 's', [0x20] = 'd', [0x21] = 'f', [0x22] = 'g',
        [0x23] = 'h', [0x24] = 'j', [0x25] = 'k', [0x26] = 'l', [0x27] = ';',
        [0x28] = '\'', [0x29] = '`',
        [0x2B] = '\\',
        [0x2C] = 'z', [0x2D] = 'x', [0x2E] = 'c', [0x2F] = 'v', [0x30] = 'b',
        [0x31] = 'n', [0x32] = 'm', [0x33] = ',', [0x34] = '.', [0x35] = '/',
        [0x37] = '*',
        [0x39] = ' ',
    };

    // ===== 4. Shift 按下时的字符表 =====
    static const char shifted[128] = {
        [0x02] = '!', [0x03] = '@', [0x04] = '#', [0x05] = '$', [0x06] = '%',
        [0x07] = '^', [0x08] = '&', [0x09] = '*', [0x0A] = '(', [0x0B] = ')',
        [0x0C] = '_', [0x0D] = '+',
        [0x0E] = '\b', [0x0F] = '\t',
        [0x10] = 'Q', [0x11] = 'W', [0x12] = 'E', [0x13] = 'R', [0x14] = 'T',
        [0x15] = 'Y', [0x16] = 'U', [0x17] = 'I', [0x18] = 'O', [0x19] = 'P',
        [0x1A] = '{', [0x1B] = '}',
        [0x1C] = '\n',
        [0x1E] = 'A', [0x1F] = 'S', [0x20] = 'D', [0x21] = 'F', [0x22] = 'G',
        [0x23] = 'H', [0x24] = 'J', [0x25] = 'K', [0x26] = 'L', [0x27] = ':',
        [0x28] = '"', [0x29] = '~',
        [0x2B] = '|',
        [0x2C] = 'Z', [0x2D] = 'X', [0x2E] = 'C', [0x2F] = 'V', [0x30] = 'B',
        [0x31] = 'N', [0x32] = 'M', [0x33] = '<', [0x34] = '>', [0x35] = '?',
        [0x37] = '*',
        [0x39] = ' ',
    };

    // ===== 5. 决定用哪张表 =====
    // 字母键：Shift 与 Caps Lock 异或（都开或都关 = 小写）
    // 其他键：只看 Shift
    int is_letter =
        (scancode >= 0x10 && scancode <= 0x19) ||  // q..p
        (scancode >= 0x1E && scancode <= 0x26) ||  // a..l
        (scancode >= 0x2C && scancode <= 0x32);    // z..m

    int use_upper = is_letter ? (shift_pressed ^ caps_lock) : shift_pressed;

    char base = use_upper ? shifted[scancode] : normal[scancode];

    /* Ctrl + 字母 → 控制字符（Ctrl+A=0x01 ... Ctrl+Z=0x1A）。
     * Ctrl+C 已在上面单独处理（返回 0），不会走到这里。 */
    if (ctrl_pressed && base >= 'a' && base <= 'z') {
        return (char)(base - 'a' + 1);
    }

    return base;
}

void keyboard_handle_irq(void) {
    uint8_t scancode = inb(KEYBOARD_DATA_PORT);
    char c = scancode_to_ascii(scancode);
    if (c) {
        struct input_event ev = {
            .type = INPUT_KEY,
            .code = (uint32_t)(unsigned char)c,
            .x = 0, .y = 0,
            .mods = input_get_mods(),
        };
        input_post(&ev);
        return;
    }

    /* 方向键 → VT100 ESC 序列。kilo 的 editorReadKey 依赖这个。 */
    const char *seq = 0;
    switch (scancode) {
        case 0x48: seq = "\x1b[A"; break;   /* 上 */
        case 0x50: seq = "\x1b[B"; break;   /* 下 */
        case 0x4D: seq = "\x1b[C"; break;   /* 右 */
        case 0x4B: seq = "\x1b[D"; break;   /* 左 */
        case 0x47: seq = "\x1b[H"; break;   /* Home */
        case 0x4F: seq = "\x1b[F"; break;   /* End */
        case 0x53: seq = "\x1b[3~"; break;  /* Delete */
        case 0x49: seq = "\x1b[5~"; break;  /* Page Up */
        case 0x51: seq = "\x1b[6~"; break;  /* Page Down */
    }
    if (seq) {
        for (int i = 0; seq[i]; i++) {
            struct input_event ev = {
                .type = INPUT_KEY,
                .code = (uint32_t)(unsigned char)seq[i],
                .x = 0, .y = 0,
                .mods = input_get_mods(),
            };
            input_post(&ev);
        }
    }
}
/* ---------- device_ops 实现 ---------- */
static int kb_dev_open(void **state, void *arg) {
    (void)arg;
    *state = NULL;
    return OK;
}

static int kb_dev_read(void *state, void *buf, uint32_t n) {
    (void)state; (void)buf; (void)n;
    return 0;   /* 键盘事件走 input 队列，设备层不提供读 */
}

static int kb_dev_write(void *state, const void *buf, uint32_t n) {
    (void)state; (void)buf; (void)n;
    return ENOSYS;   /* 键盘不支持写 */
}

static int kb_dev_ioctl(void *state, uint32_t cmd, void *arg) {
    (void)state; (void)cmd; (void)arg;
    return ENOSYS;   /* 暂无 ioctl */
}

static void kb_dev_close(void *state) {
    (void)state;
}

static const struct device_ops kb_ops = {
    .open  = kb_dev_open,
    .read  = kb_dev_read,
    .write = kb_dev_write,
    .ioctl = kb_dev_ioctl,
    .close = kb_dev_close,
};

/* 在 keyboard_init 里注册 */
void keyboard_init(void) {
    dev_register(DEV_TYPE_KEYBOARD, &kb_ops);
    irq_register(1, keyboard_handle_irq, "keyboard");
}