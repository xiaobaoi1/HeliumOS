# HeliumOS

一个从零构建的 32 位宏内核操作系统。

## 一、项目哲学

**拒绝"大一统"，拥抱"子系统内的抽象"。**

这是整个设计最核心的一条。不是拒绝抽象，而是拒绝跨子系统的统一抽象层。

| 拒绝 | 接受 |
|---|---|
| VFS（一切皆文件） | fs 自己的 `fs_open/read/write` |
| 统一 fd 表 | 每子系统独立句柄空间 |
| fork + exec | `spawn` 一步到位 |
| 挂载树命名空间 | 卷符系统（`SYS:/` `DATA:/`） |
| 所有东西都能 `read(fd, ...)` | read/write 是**标准流**，不是文件 |

**关键区分**：

- **抽象** = 子系统内统一接口，隐藏实现细节 ✅
- **统一** = 跨子系统共享抽象层 ❌
- **分层** = 上层调下层，下层不知道上层 ✅

## 二、命名空间：卷符系统

不采用 Linux 的挂载树，而采用**多起点命名**：

```
SYS:/    系统盘
DATA:/   数据盘
TMP:/    临时盘
```

- 卷之间平级，没有父子关系
- 用户看到 `SYS:/` 和 `DATA:/`，直觉知道是两块盘
- 借鉴 Windows 盘符思路，但拒绝其复杂性

但 cwd 必须有、相对路径必须有、`..` 必须有。`..` 到卷根**停止**，不跨卷。

```c
struct task {
    char cwd_volume[VOL_NAME_LEN];  // "SYS"
    char cwd_path[PATH_MAX_LEN];    // "/BOOT"
};
```

## 三、进程模型

### 3.1 拒绝 fork，采用 spawn

```
spawn(path, argv, redir) → proc_handle_t
```

- 没有 COW、没有 exec
- 语义清晰："创建一个运行新程序的新进程"
- Shell 就是 spawn + wait

### 3.2 进程对象（借鉴 Windows）

不是裸 pid，而是有引用计数的对象：

- `proc_handle_t` 是指向 PCB 的句柄
- `refcount` = self 引用 + 每个 `proc_handles[i]`
- 归零且 zombie 时释放 PCB

### 3.3 graveyard 机制

`task_exit` 不能释放自己正在用的栈。所以把不能立即释放的 PCB 挂到 graveyard，由时钟 tick 里的 `proc_reap_graveyard` 回收。

### 3.4 上下文切换

**非标准的 switch_to**：不保存"调用时的栈位置"，而是直接 iret 到目标进程的用户态。

- `kernel_esp` 语义 = "进程进入内核时的 iret 帧位置"
- 三条入口 stub（syscall / irq / isr）的栈头部布局必须**完全一致**
- 首次调度走 `switch_to(NULL, first)` → `enter_user_mode` trampoline

这带来的直接结果：**阻塞系统调用的返回值靠"阻塞前预设 regs->eax"传递**，而不是靠返回值从内核态流回。

## 四、I/O 架构

### 4.1 标准流：进程属性

`read(0)` / `write(1)` 不是 fs、不是 dev，而是**进程的两个命名端点**：

```c
struct task {
    struct io_slot stdin_slot;
    struct io_slot stdout_slot;
    struct io_slot stderr_slot;
};

struct io_slot {
    uint8_t type;   // IO_SLOT_DEFAULT / FILE / NULL
    int     fd;     // FILE 时有效
};
```

分派在 syscall 层：

```
write(1, ...) → 查 current->stdout_slot → tty / fs_write / 丢弃
```

这**不是 VFS**。它是"进程有 I/O 端口，端口可插不同设备"，和"一切皆文件"完全不同。

### 4.2 tty：文本流子系统

tty 不是"特权中间层"，是**基于 dev 的独立子系统**：

- 内部通过 `dev_get_ops(DEV_TYPE_VGA)` 拿 VGA 的 ops
- 键盘同理
- 保留字符语义（`\n` `\b` `\t`、滚屏、光标）
- 未来可换 font、可被 graphics 子系统并存

### 4.3 dev：硬件能力子系统

dev 只做硬件能直接做的事：

| 设备 | dev 提供 |
|---|---|
| VGA | 写像素、硬件光标、调色板 |
| keyboard | 读扫描码 |
| serial | 读写字节 |
| ATA | 读写扇区 |

**dev 不理解**字符、文本、图形。

### 4.4 各子系统独立

| 子系统 | 句柄 | 系统调用段 |
|---|---|---|
| fs | `fs_handles[32]` | 31~39 |
| dev | `dev_handles[8]` | 41~49 |
| proc | `proc_handles[8]` | 1~9 |
| 标准流 | 3 个 slot（非句柄） | 11~12 |
| tty 控制 | 无句柄 | 13~20 |

## 五、文件系统

### 5.1 结构

```
应用 → fs_open/read/write/... → fat32.c → ata.c → 磁盘
                                          ↑
用户程序 → dev_open(ATA) ────────────────────┘
```

**两套视图**：

- 进程上下文：走 fs 层（带句柄、cwd）
- 内核早期（kmain）：直调 `fat32_open_file` 等

这符合"内核早期不依赖 current_task"的原则。

### 5.2 FAT32 实现

- **读写**：`fat32_read_file` / `fat32_write_file`
- **创建**：`fat32_create_file`（存在则打开）
- **长文件名（LFN）**：读写都支持（UTF-16 → ASCII 简化）
- **短名生成**：`LONGNA~N.TXT` 形式，冲突递增
- **目录扩展**：目录满时自动分配新簇
- **FAT 缓存**：单槽缓存，利用局部性

**已知限制**：

- UTF-16 只处理 ASCII（> 0x7F 写不进去）
- LFN 项不跨扇区（名字 ≤ 195 字符）
- 无 unlink / mkdir / rmdir / rename

### 5.3 卷抽象

`volume.c` 提供：

- `volume_register("SYS", VOL_FS_FAT32, lba, count, fat32_vol)`
- `volume_lookup("SYS")`
- 每个 volume 有 `fs_private` 指向具体文件系统上下文（`fat32_volume`）

## 六、内存管理

### 6.1 PMM：位图

- 管理 0..1GB（可配）
- 4KB 页
- `pmm_alloc_page` 从低地址扫描

### 6.2 VMM：PSE 大页 + 用户页表

**内核恒等映射**用 PSE 4MB 大页：

- 256 个 PDE 覆盖 0..1GB
- 内核代码直接强转物理地址当指针用

**用户地址空间**用常规 4KB 页表：

- 每进程独立 pgd
- 复制内核前 256 项
- 用户 pgd 没有递归映射（1023 项）

### 6.3 内核堆：slab

- 9 个大小类（8..2048）
- 每 slab 一页（4KB）
- `kmalloc(size > 2048)` 返回 NULL

## 七、系统调用

### 7.1 分段

```
1~9     进程控制（exit, spawn, sleep, wait, kill, close）
11~12   标准流（read / write）
13~20   tty 控制（clear, color, cursor, foreground）
21~29   内存（brk）
31~39   文件系统
41~49   设备
61~69   进程环境（cwd, chdir）
```

### 7.2 返回值语义

| 类型 | 成功返回 | 失败返回 |
|---|---|---|
| I/O | 字节数 | 负 errno |
| 创建 | 句柄（非负） | 负 errno |
| 状态改变 | 0 | 负 errno |
| 查询 | 查询值 | 负 errno |

### 7.3 阻塞系统调用的返回值

**关键设计**：`sys_wait` / `sys_sleep` 阻塞期间，`syscall_handler` 的 `regs->eax = ret` 不会执行（因为 switch_to 直接 iret 到用户态）。

返回值靠：

- **阻塞前预设**：`regs->eax = EAGAIN`（wait）
- **唤醒时覆盖**：`task_terminate` 时设 `regs->eax = exit_status`

## 八、用户态

### 8.1 crt0

`_start_crt0` 是 ELF entry：

1. 从栈读 argc/argv
2. 调 `_start(argc, argv)`
3. 返回后执行 `SYS_EXIT`

### 8.2 libc

最小集：

- `string.c`：memcpy / memset / strlen / strcmp / strncpy 等
- `printf.c`：%d %u %x %X %p %c %s %l*（无 %f，无宽度修饰）
- `malloc.c`：简单 first-fit，不合并相邻 free 块

### 8.3 Shell

- `read_line`：轮询键盘 + 退格
- `tokenize`：空格拆分
- 内置命令：`ls` `cd` `pwd` `cat` `clear` `help` `exit` `touch` `sink`
- `spawn` + `wait` 执行外部程序

## 九、当前状态

### 已完成

```
✅ 引导：Multiboot2 + PSE 分页
✅ 中断：IDT / PIC / ISR / IRQ / syscall
✅ 内存：PMM 位图 + VMM 大页 + slab 堆
✅ 调度：就绪队列 + 阻塞链表 + graveyard
✅ 进程：spawn / wait / kill / 引用计数
✅ 文件系统：FAT32 读写 + LFN + 目录扩展
✅ 设备：dev 子系统 + VGA + keyboard + serial
✅ tty：基于 dev 的文本流
✅ 标准流：slot 机制（read/write 可重定向）
✅ 用户态：crt0 + 最小 libc + shell
✅ argv 传递
✅ 前台归属机制（未在 shell 接入）
```

### 已知缺口

- 用户异常：越界访问会冻结整个内核（`cli; hlt`）
- 无信号，Ctrl+C 不可用
- 无 IPC / pipe
- 无 envp
- FAT32 无 unlink / mkdir / rename
- 前台机制已就绪但未接线

## 十、未来规划

### 主线 1：稳定性 → 交互性

| 顺序 | 项 | 说明 |
|---|---|---|
| 1 | **用户异常处理** | 用户错误只杀进程，不冻结系统 |
| 2 | **前台完整接入** | shell 调 `tty_set_foreground` |
| 3 | **信号** | 完整 handler + sigreturn |
| 4 | Ctrl+C 上线 | 依赖前台 + 信号 |

### 主线 2：功能 → 可用性

| 顺序 | 项 | 说明 |
|---|---|---|
| 1 | **envp** | 环境变量 |
| 2 | **IPC / pipe** | `cmd1 \| cmd2` |
| 3 | **FAT32 写操作补全** | unlink / mkdir / rmdir / rename |
| 4 | **Shell 补全** | rm / mkdir / mv / cp |
| 5 | **libc 完善** | assert / ctype / qsort |

### 长期

- 时间（RTC）、关机 / 重启
- 图形子系统（基于 screen dev）
- 网络栈
- SMP 多核
- 按需分页 / mmap

## 十一、目录结构

```
src/
├── boot/         start.asm（Multiboot2 + 页表 + 中断入口 + switch_to）
├── include/      公共头文件
├── kernel/       kmain / idt / isr / gdt / tss / syscall / tty / proc / device
├── drivers/      screen / keyboard / serial / ata / fat32
├── fs/           volume / path / fs
├── memory/       pmm / vmm / heap
└── process/      task / scheduler

user/
├── libc/         用户态 C 库
├── crt0.asm      启动桩
├── shell.c       shell
├── test.c        综合测试
├── testkill.c    kill 路径测试
├── test_write.c  FAT32 写测试
├── argtest.c     argv 测试
├── sleeper.c     靶子
├── idle.c        空转
└── linker.ld
```

## 十二、构建与运行

```bash
make            # 生成 ISO
make run        # QEMU 跑 ISO

make run-disk   # 生成 FAT32 硬盘镜像并跑（需要 sudo）
make debug-disk # 同上，加 gdb 端口

make clean
```

**要求**：`nasm` `gcc`（32 位支持）`ld` `qemu-system-i386` `grub-mkrescue`。

## 十三、设计取向总结

HeliumOS 不是"重新发明 UNIX"，而是**从第一原理重新审视操作系统应该怎么组织**：

- 抽象 ≠ 统一
- 卷符 ≠ 挂载树
- spawn ≠ fork
- 标准流 ≠ 文件
- 子系统 ≠ 共享抽象

每一条边界都是**深思熟虑的取舍**，不是妥协。

---

**当前阶段**：稳定性与交互性并进。用户异常处理优先，随后接前台 + 信号。

**项目愿景**：一个架构清晰、边界明确、可读可控的小型宏内核。





















## BUGs
先归纳，不急着给方案。

## 现状的真实代价

`read_line` 忙等的开销不只是"不优雅"：

- shell 每 tick（1ms）被抢占一次，其余时间在 `pause` 里空转
- IDLE 也在 `pause` 空转
- **整个系统在等键盘时 100% CPU**——但这不是 shell 造成的，是 IDLE 造成的

所以 shell 忙等和 IDLE 忙等叠加，看不出差别。**真正的问题不是 shell，是 IDLE 用 `pause` 而非 `hlt`**。shell 忙等只是让这个问题更显眼。

## "真阻塞 read"在内核里意味着什么

不是把 `tty_read` 改成"没数据就睡"那么简单。它需要三条链同时存在：

| 链 | 现状 | 要做 |
|---|---|---|
| **等待状态** | `TASK_STATE_*` 里没有 WAITING_TTY | 加状态，或复用 |
| **唤醒源** | `keyboard_handle_irq` 只往 `kb_buffer` 塞字符，不知道有没有进程在等 | 键盘 IRQ 里加"唤醒 tty 等待者" |
| **信号打断** | `pipe_read` 有完整的 EINTR 循环（`block_current` + `schedule` + 醒来查 pending） | tty_read 要抄这套 |

三条链里，**第二条是真正的耦合点**：键盘 IRQ 是硬件中断上下文，在里面调 `unblock_task`（动 ready queue）时序敏感。`pipe_wakeup_all` 是进程上下文里调的，键盘 IRQ 不是。

## 三条路对比

| 方案 | 内核改动 | 键盘延迟 | shell 复杂度 | 何时合适 |
|---|---|---|---|---|
| 现状（忙等） | 无 | 极低 | 无 | 现在 |
| shell 里 `sleep_ms(1)` | 无 | ≤1ms | 一行 | 现在就能做 |
| 真阻塞 read | 中（三条链） | 极低 | 无 | 有多个读键盘的进程时 |

## 我的判断

**现在都不做。** 理由：

1. **`sleep_ms(1)` 只是把忙等挪到内核**——shell 睡了，IDLE 还在 `pause`。CPU 占用不变，只是"看起来优雅"。
2. **真阻塞 read 是一个子系统级改动**，不是顺手。它值得单独立项，前提是：
   - 有非 shell 的进程读键盘（比如全屏编辑器）
   - 或者 IDLE 改成 `hlt` 后，忙等的功耗变得真实可见
3. **当前 shell 是唯一读键盘的进程**，前台切换已经是显式的（`tty_set_foreground`），阻塞 read 换不来任何可观察的收益。

**如果一定要现在动**，我倾向 `sleep_ms(1)`——一行，不改内核，等真需求出现时再升级到阻塞 read。但这不是"更好"，只是"看起来不那么忙"。

## 顺带发现：唤醒路径不一致

看 `sys_kill` 和 `signal_foreground`：

```c
if (target->state == TASK_STATE_SLEEPING ||
    target->state == TASK_STATE_WAITING_CHILD) {
    remove_task_from_queue(...);
    target->state = TASK_STATE_READY;
    enqueue_task(...);
}
```

**只唤醒 SLEEPING 和 WAITING_CHILD，不唤醒 WAITING_PIPE**。

`WAITING_PIPE` 靠 `pipe_wakeup_all` 唤醒——但那是数据/EOF 事件触发的，不是信号触发的。所以：

- A 阻塞在 `pipe_read` 上等数据
- B `kill(A, SIGINT)`
- `sys_kill` 把 pending 设了，但**不唤醒 A**
- A 继续睡，直到 pipe 有数据或写端关闭才醒
- 醒来才看到 pending → 返回 EINTR

**信号打断 pipe 阻塞是延迟的**。`pipe_read` 里确实有 `if (cur->pending_signals & ~cur->blocked_signals) return EINTR;`——但那个检查在**阻塞之前**，醒来后要靠 `pipe_wakeup_all` 才能重新走到检查点。

这是**已经存在的 bug**，和 tty 无关。如果将来加 WAITING_TTY，这个模式会再复制一份。

要不要现在处理？我的建议：**记下，不做**。因为：
- 当前没有"阻塞在 pipe 上被信号杀"的实际场景（pipe 都是短命命令）
- 修它要动 `sys_kill` / `signal_foreground` 的唤醒逻辑，触及信号投递路径
- 等 WAITING_TTY 或前台进程组出现时，一起统一

---

**结论**：read 的问题**先不动**。等你哪天觉得 shell 响应或 IDLE 功耗真的烦了，再来立"阻塞 IO"这个项。那时候 tty 阻塞 + pipe 唤醒一致性 + IDLE `hlt` 一起做，才是一个完整的设计。

要不要把这条记进项目文档的"已知的坑"里？还是口头记住就行？


信号打断 pipe 阻塞有延迟：sys_kill / signal_foreground 只唤醒 SLEEPING 和 WAITING_CHILD，不唤醒 WAITING_PIPE。阻塞在 pipe_read 的进程收到信号后，要等 pipe 数据/EOF 事件才醒。pipe_read 里的 pending 检查只在阻塞前生效，醒来后才重新走到。和 tty 阻塞一起处理。


记下。

## BUG 记录

**现象**：修改过磁盘后 `test.elf` 出现 2 个 unpass
- `test_dev_vga` 的 `dev_open VGA`
- 另一个未见（用户已改 test.c 加汇总）

**触发路径**：
```
添加 SHELL cp + FS_O_TRUNC 截断
  → cp / ls / mkdir / cd 若干次
  → 跑 test.elf
  → 2 个 unpass
```

**可复现性**：

| 操作 | 结果 |
|---|---|
| 重启 QEMU（同一 disk.img） | 复现 |
| 重建 disk.img | 不复现 |

**结论**：disk.img 上的持久状态是触发因素，代码本身不是充分条件。

## 排除 & 怀疑

**排除**：

- 代码分支问题（重建后代码没变，行为变好）→ 只在特定磁盘状态下才走到
- `pmm` 位图错误（每次启动都重建，和 disk.img 无关）
- 单纯 ELF 损坏（test.elf 大部分 PASS，说明加载正常）

**怀疑**（按可能性排序）：

1. **FAT32 写路径在特定条件下写坏了元数据**——比如簇链被错误释放后又被重分配，覆盖了某文件的数据，`test.elf` 读到后走异常分支
2. **`free_cluster_chain` / `fat32_truncate` 里对"末簇"处理有边界问题**——`cp` 覆盖 `SHELL.ELF` 大小时会走到截断 + 扩展的完整路径，值得单独测
3. **PCB 页被用户态映射覆盖**（可能性低，因为和 disk.img 关联不上）

## 下次遇到时的抓取清单

1. **完整 FAIL 输出**（test.c 已改，应该能看到了）
2. `ls /` 和 `ls /TMP` 输出
3. 触发这个 bug 的**完整命令序列**
4. 保留 disk.img（不要重建）—— 需要时可以拿 gdb 或十六进制看 `/TMP` 目录项

## 短期 workaround

重建 disk.img。已知有效。

---

这条记在这里，不占主线。继续主线 2 剩余项：**libc 完善（`assert` / `ctype` / `qsort`）**。

先问一句：`assert` 失败时的行为——直接 `_exit(1)`，还是打印信息后 `_exit(1)`？后者需要 `fprintf` 到 stderr，但当前 `dprintf` 只支持 `fd 1`（`sys_write` 里 `if (fd != 1) return EINVAL`）。要不要顺手让 `stderr`（fd 2）也能写？这个改动很小，但属于"给不存在的将来预留"还是"顺手做对"，取决于你想不想让 `assert` / 错误信息走 stderr。




等等。上述修改我未应用。
我在之前的基础上又发现了一个bug
这是我的test（使用了一个很大的数组[32][200]）
在运行完test后，运行testkill时sleeper的输出不正确（输出的字符被中途截断）












## poweroff有序关闭 (UNDO)