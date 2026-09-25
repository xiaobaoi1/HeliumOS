#ifndef PROC_H
#define PROC_H

#include <stdint.h>

typedef int proc_handle_t;

#define PROC_MAX_HANDLES   8
#define PROC_INVALID      (-1)

/* 访问权限 */
#define PROC_QUERY        0x01  // 预留
#define PROC_TERMINATE    0x02
#define PROC_WAIT         0x04
#define PROC_ALL          0x07

/* 前向声明 */
struct task;

struct proc_handle {
    uint8_t      used;
    uint8_t      access;        /* PROC_* 掩码 */
    uint16_t     reserved;
    struct task *task;
};

/* ---------- 生命周期 ---------- */
void proc_init(void);
void proc_register(struct task *t);
void proc_unregister(struct task *t);
void proc_free_pcb(struct task *t);         /* 真正释放 PCB */

/* ---------- 引用计数 ---------- */
void proc_ref(struct task *t);
void proc_unref(struct task *t);            /* 归零且 zombie 时释放 */

/* ---------- 全局查询 ---------- */
struct task *proc_find_by_pid(uint32_t pid);
int          proc_list(uint32_t *pids, int max);

/* ---------- 句柄 ---------- */
proc_handle_t proc_handle_alloc(struct task *target, uint8_t access);
struct task  *proc_handle_deref(proc_handle_t h, uint8_t need_access);
void          proc_release_all_handles(struct task *owner);

/* ---------- graveyard（task_exit 专用） ---------- */
void proc_reap_graveyard(void);

#endif