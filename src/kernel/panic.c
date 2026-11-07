#include <panic.h>
#include <printf.h>
#include <stdarg.h>
#include <stdint.h>
#include <stddef.h>

static void dump_regs(const struct registers *r) {
    if (!r) return;
    kprintf("--- registers ---\n");
    kprintf("  eax=%p ebx=%p ecx=%p edx=%p\n",
            r->eax, r->ebx, r->ecx, r->edx);
    kprintf("  esi=%p edi=%p ebp=%p esp=%p\n",
            r->esi, r->edi, r->ebp, r->esp);
    kprintf("  eip=%p cs=%x eflags=%p\n",
            r->eip, r->cs, r->eflags);
    kprintf("  int_no=%d err_code=%p\n",
            (int)r->int_no, r->err_code);
    kprintf("  user_esp=%p user_ss=%x\n",
            r->user_esp, r->user_ss);
}

static void halt_forever(void) {
    for (;;) {
        __asm__ volatile("cli; hlt");
    }
}

void panic(const char *fmt, ...) {
    kprintf("\n[PANIC] ");
    va_list ap;
    va_start(ap, fmt);
    vkprintf(fmt, ap);
    va_end(ap);
    kprintf("\n");

    g_pr.backend = 0;
    g_pr.fb = fb->virt;
    g_pr.fb_w = fb->width;
    g_pr.fb_h = fb->height;
    g_pr.fb_pitch_px = fb->pitch / 4;
    g_pr.cols = (int)(fb->width / 8);
    g_pr.rows = (int)(fb->height / 16);
    g_pr.cur_x = 0;
    g_pr.cur_y = 0;
    g_pr.cur_fg = 0xFFFFFF;
    g_pr.cur_bg = 0x000000;

    uint32_t total = g_pr.fb_h * g_pr.fb_pitch_px;
    for (uint32_t i = 0; i < total; i++) g_pr.fb[i] = 0x000000;
    return 1;
}

static int pr_try_vga(void) {
    g_pr.backend = 1;
    g_pr.vga = (volatile uint16_t*)0xB8000;
    g_pr.cols = 80;
    g_pr.rows = 25;
    g_pr.cur_x = 0;
    g_pr.cur_y = 0;
    g_pr.cur_fg = 0xFFFFFF;
    g_pr.cur_bg = 0x000000;
    for (int i = 0; i < 80 * 25; i++)
        g_pr.vga[i] = ((uint16_t)0x07 << 8) | ' ';
    return 1;
}

static void pr_fallback_serial(void) {
    g_pr.backend = 2;
    g_pr.cols = 80;
    g_pr.rows = 25;
    g_pr.cur_x = 0;
    g_pr.cur_y = 0;
    g_pr.cur_fg = 0xFFFFFF;
    g_pr.cur_bg = 0x000000;
}

/* ---------- dump 片段 ---------- */

static void pr_dump_regs(const struct registers *r) {
    if (!r) return;
    pr_puts("\n  --- registers ---\n");
    pr_printf("  eax=%08X ebx=%08X ecx=%08X edx=%08X\n",
              r->eax, r->ebx, r->ecx, r->edx);
    pr_printf("  esi=%08X edi=%08X ebp=%08X esp=%08X\n",
              r->esi, r->edi, r->ebp, r->esp);
    pr_printf("  eip=%08X cs=%04X eflags=%08X\n",
              r->eip, r->cs, r->eflags);
    pr_printf("  int=%u err=%08X\n",
              (unsigned)r->int_no, r->err_code);
    pr_printf("  user_esp=%08X user_ss=%04X\n",
              r->user_esp, r->user_ss);
}

static void pr_dump_task(struct task *t) {
    if (!t) {
        pr_puts("\n  --- current process --- (none)\n");
        return;
    }
    pr_puts("\n  --- current process ---\n");
    pr_printf("  pid=%u state=%u class=%u caps=0x%X\n",
              t->pid, t->state, (unsigned)t->class, t->caps);
    if (t->image[0])
        pr_printf("  image=%s\n", t->image);
    pr_printf("  creator_pid=%u entry=%08X\n",
              t->creator_pid, t->entry_point);
    pr_printf("  cwd=%s:%s\n",
              t->cwd_volume[0] ? t->cwd_volume : "?",
              t->cwd_path[0] ? t->cwd_path : "/");
    pr_printf("  heap=[%08X, %08X) brk=%08X\n",
              t->heap_base, t->heap_limit, t->heap_brk);

    int vma_count = 0;
    for (struct vma *v = t->vma_list; v; v = v->next) vma_count++;
    pr_printf("  vma_count=%d\n", vma_count);
}

static void pr_dump_log(void) {
    int n = log_ring_size();
    if (n <= 0) {
        pr_puts("\n  --- last log --- (empty)\n");
        return;
    }

    pr_puts("\n  --- last log ---\n");

    /* 统计总行数 */
    int lines = 0;
    for (int i = 0; i < n; i++)
        if (log_ring_at(i) == '\n') lines++;

    /* 只保留最后 30 行 */
    int start = 0;
    if (lines > 30) {
        int target = lines - 30;
        int count = 0;
        for (int i = 0; i < n; i++) {
            if (log_ring_at(i) == '\n') {
                count++;
                if (count == target) { start = i + 1; break; }
            }
        }
    }

    for (int i = start; i < n; i++) {
        char c = log_ring_at(i);
        if (c) pr_putc(c);
    }
    if (log_ring_at(n - 1) != '\n') pr_putc('\n');
}

/* ---------- 入口 ---------- */

static void pr_begin(const char *file, uint32_t line) {
    __asm__ volatile("cli");

    if (!pr_try_fb()) {
        if (!pr_try_vga()) pr_fallback_serial();
    }

    if (g_pr.backend != 2) {
        /* 标题行：白底黑字 */
        g_pr.cur_fg = 0x000000;
        g_pr.cur_bg = 0xFFFFFF;
        pr_puts("================ HELIUMOS KERNEL PANIC ================\n");
        g_pr.cur_fg = 0xFFFFFF;
        g_pr.cur_bg = 0x000000;
    } else {
        pr_puts("================ HELIUMOS KERNEL PANIC ================\n");
    }

    pr_printf("\n  Stop code: 0x%04X\n", line);
    pr_printf("  Location:  %s:%u\n\n", file, line);
}

static void pr_end(void) {
    uint32_t cr2, cr3;
    __asm__ volatile("mov %%cr2, %0" : "=r"(cr2));
    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
    kprintf("  cr2=%p cr3=%p\n", cr2, cr3);

    kprintf("[PANIC] System halted.\n");
    halt_forever();
}

void panic_regs(const struct registers *regs, const char *fmt, ...) {
    kprintf("\n[PANIC] ");
    va_list ap;
    va_start(ap, fmt);
    vkprintf(fmt, ap);
    va_end(ap);
    kprintf("\n");

    dump_regs(regs);

    uint32_t cr2, cr3;
    __asm__ volatile("mov %%cr2, %0" : "=r"(cr2));
    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
    kprintf("  cr2=%p cr3=%p\n", cr2, cr3);

    kprintf("[PANIC] System halted.\n");
    halt_forever();
}