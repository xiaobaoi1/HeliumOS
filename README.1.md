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