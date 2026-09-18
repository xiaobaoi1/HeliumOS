#include <keyboard.h>
#include <io.h>
#include <stdint.h>
#include <stddef.h>
#include <device.h>
#include <errno.h>

#define KEYBOARD_DATA_PORT   0x60
#define KB_BUF_SIZE          128

/* ===== 内部状态，对外完全隐藏 ===== */
static char kb_buffer[KB_BUF_SIZE];
static volatile int kb_head = 0;
static volatile int kb_tail = 0;

static int shift_pressed = 0;
static int caps_lock = 0;
static int ctrl_pressed = 0;
static int alt_pressed = 0;

/* 扫描码转 ASCII（内部函数） */
static char scancode_to_ascii(uint8_t scancode) {
    // ===== 1. 修饰键：只更新状态，不产生字符 =====
    if (scancode == 0x2A || scancode == 0x36) { shift_pressed = 1; return 0; } // 左/右 Shift 按下
    if (scancode == 0xAA || scancode == 0xB6) { shift_pressed = 0; return 0; } // 左/右 Shift 释放
    if (scancode == 0x3A) { caps_lock = !caps_lock; return 0; }                // Caps Lock 切换
    if (scancode == 0x1D) { ctrl_pressed = 1; return 0; }                       // Ctrl 按下
    if (scancode == 0x9D) { ctrl_pressed = 0; return 0; }                       // Ctrl 释放
    if (scancode == 0x38) { alt_pressed = 1; return 0; }                        // Alt 按下
    if (scancode == 0xB8) { alt_pressed = 0; return 0; }                        // Alt 释放

    // for test ESC
    if (scancode == 0x01) return 27;

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
        [0x1C] = '\n',
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

    return use_upper ? shifted[scancode] : normal[scancode];
}

void keyboard_handle_irq(void) {
    uint8_t scancode = inb(KEYBOARD_DATA_PORT);
    char c = scancode_to_ascii(scancode);
    if (c) {
        int next = (kb_head + 1) % KB_BUF_SIZE;
        if (next != kb_tail) {
            kb_buffer[kb_head] = c;
            kb_head = next;
        }
    }
}

int keyboard_has_data(void) {
    return kb_head != kb_tail;
}

char keyboard_getchar(void) {
    if (kb_head == kb_tail) return 0;
    char c = kb_buffer[kb_tail];
    kb_tail = (kb_tail + 1) % KB_BUF_SIZE;
    return c;
}

/* ---------- device_ops 实现 ---------- */
static int kb_dev_open(void **state, void *arg) {
    (void)arg;
    *state = NULL;
    return OK;
}

static int kb_dev_read(void *state, void *buf, uint32_t n) {
    (void)state;
    if (!buf || n == 0) return EINVAL;
    char *p = buf;
    uint32_t got = 0;
    while (got < n && keyboard_has_data()) {
        p[got++] = keyboard_getchar();
    }
    return got;   /* 非阻塞，可能返回 0 */
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
}