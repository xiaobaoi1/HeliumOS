#include <input.h>
#include <task.h>
#include <printf.h>
#include <string.h>
#include <stddef.h>

static struct input_event g_queue[INPUT_QUEUE_SIZE];
static volatile int g_head = 0;
static volatile int g_tail = 0;
static volatile uint32_t g_owner_pid = 0;

void input_init(void) {
    g_head = g_tail = 0;
    g_owner_pid = 0;
}

void input_post(const struct input_event *ev) {
    if (!ev) return;

    int next = (g_head + 1) % INPUT_QUEUE_SIZE;
    if (next == g_tail) {
        /* 满——丢最旧的 */
        g_tail = (g_tail + 1) % INPUT_QUEUE_SIZE;
    }
    g_queue[g_head] = *ev;
    g_head = next;
}

int input_poll(struct input_event *out) {
    if (g_head == g_tail) return -1;
    *out = g_queue[g_tail];
    g_tail = (g_tail + 1) % INPUT_QUEUE_SIZE;
    return 0;
}

void input_claim_keyboard(uint32_t pid) {
    g_owner_pid = pid;
    kprintf("[INPUT] keyboard claimed by pid=%u\n", pid);
}

void input_release_keyboard(uint32_t pid) {
    if (g_owner_pid == pid) {
        g_owner_pid = 0;
        kprintf("[INPUT] keyboard released by pid=%u\n", pid);
    }
}

int input_keyboard_claimed(void) {
    return g_owner_pid != 0;
}

uint32_t input_keyboard_owner(void) { return g_owner_pid; }