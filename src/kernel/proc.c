#include <proc.h>
#include <task.h>
#include <pmm.h>
#include <vmm.h>
#include <printf.h>
#include <errno.h>
#include <stddef.h>

/* ---------- 全局进程链表 ---------- */
static struct task *proc_list_head = NULL;

/* ---------- graveyard：task_exit 里不能释放自己，放这里 ---------- */
static struct task *graveyard = NULL;

void proc_init(void) {
    proc_list_head = NULL;
    graveyard = NULL;
    kprintf("[PROC] Initialized\n");
}

/* ---------- 全局链表 ---------- */

void proc_register(struct task *t) {
    if (!t) return;
    t->proc_next = proc_list_head;
    proc_list_head = t;
}

void proc_unregister(struct task *t) {
    if (!t) return;
    if (proc_list_head == t) {
        proc_list_head = t->proc_next;
        return;
    }
    for (struct task *p = proc_list_head; p; p = p->proc_next) {
        if (p->proc_next == t) {
            p->proc_next = t->proc_next;
            return;
        }
    }
}

struct task *proc_find_by_pid(uint32_t pid) {
    for (struct task *t = proc_list_head; t; t = t->proc_next) {
        if (t->pid == pid) return t;
    }
    return NULL;
}

int proc_list(uint32_t *pids, int max) {
    int n = 0;
    for (struct task *t = proc_list_head; t && n < max; t = t->proc_next) {
        if (t->zombie) continue;
        pids[n++] = t->pid;
    }
    return n;
}

/* ---------- 引用计数 ---------- */

void proc_ref(struct task *t) {
    if (t) t->refcount++;
}

void proc_unref(struct task *t) {
    if (!t) return;
    if (t->refcount == 0) return;
    t->refcount--;

    if (t->refcount == 0 && t->zombie) {
        if (t == get_current_task()) {
            t->grave_next = graveyard;    /* ← 用 grave_next */
            graveyard = t;
        } else {
            proc_free_pcb(t);
        }
    }
}

/* 真正释放：pgd + 内核栈 + PCB 本身 */
void proc_free_pcb(struct task *t) {
    if (!t) return;
    kprintf("[TASK] Process %d is cleaned.\n", t->pid);

    /* 先从所有队列里摘掉，再释放内存 */
    remove_task_from_queue(&blocked_list_head, &blocked_list_tail, t);
    remove_task_from_queue(&ready_queue_head, &ready_queue_tail, t);

    /* 释放用户地址空间 */
    if (t->pgd) {
        vmm_free_process_address_space(t->pgd);
        t->pgd = NULL;
    }

    /* 释放内核栈 */
    if (t->kernel_stack_phys) {
        pmm_free_page(t->kernel_stack_phys);
        t->kernel_stack_phys = 0;
    }

    /* 从全局链表移除 */
    proc_unregister(t);

    /* 释放 PCB */
    pmm_free_page((uint32_t)t);
}
/* 在 irq_handler 里调用（时钟 tick 开头），此时 CPU 已在新进程的栈上 */
void proc_reap_graveyard(void) {
    while (graveyard) {
        struct task *dead = graveyard;
        graveyard = dead->grave_next;
        dead->grave_next = NULL;
        proc_free_pcb(dead);
    }
}

/* ---------- 句柄管理 ---------- */

proc_handle_t proc_handle_alloc(struct task *target, uint8_t access) {
    struct task *cur = get_current_task();
    if (!cur || !target) return PROC_INVALID;

    for (int i = 0; i < PROC_MAX_HANDLES; i++) {
        if (!cur->proc_handles[i].used) {
            cur->proc_handles[i].used   = 1;
            cur->proc_handles[i].access = access;
            cur->proc_handles[i].task   = target;
            proc_ref(target);
            return i;
        }
    }
    return PROC_INVALID;
}

struct task *proc_handle_deref(proc_handle_t h, uint8_t need_access) {
    struct task *cur = get_current_task();
    if (!cur) return NULL;
    if (h < 0 || h >= PROC_MAX_HANDLES) return NULL;
    if (!cur->proc_handles[h].used) return NULL;
    if ((cur->proc_handles[h].access & need_access) != need_access) return NULL;
    return cur->proc_handles[h].task;
}

void proc_release_all_handles(struct task *owner) {
    if (!owner) return;
    for (int i = 0; i < PROC_MAX_HANDLES; i++) {
        if (owner->proc_handles[i].used) {
            struct task *target = owner->proc_handles[i].task;
            owner->proc_handles[i].used = 0;
            owner->proc_handles[i].access = 0;
            owner->proc_handles[i].task = NULL;
            proc_unref(target);
        }
    }
}