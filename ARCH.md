# 完整 OS 的子系统地图

## 一、分类：对象型 vs 服务型

判断一个东西是不是"子系统"，标准是**它有没有自己的对象、API、句柄空间**。

两类：

**对象型**：有明确的对象类型，有 per-process 句柄空间。
**服务型**：提供能力，但没有 per-process 句柄（或只有少量）。

## 二、对象型子系统

| 子系统 | 对象 | 句柄空间 | 现在 | 未来 |
|---|---|---|---|---|
| **proc** | 进程 | `proc_handles[8]` | ✅ | 完善 |
| **fs** | 文件/目录 | `fs_handles[32]` | ✅ | 加 RAMFS、写支持 |
| **dev** | 设备 | `dev_handles[8]` | ✅ 骨架 | 接串口/ATA/网卡 |
| **net** | socket | `net_handles[N]` | ❌ | socket API |
| **ipc** | pipe/shm/mq | `ipc_handles[N]` | ❌ | pipe 优先 |

**每类都有自己的对象和句柄空间。互相不共享。**

**特点**：
- 用户可以显式 `open` 得到一个句柄
- 句柄可以在进程内使用、传递（未来）
- 每个子系统有独立的系统调用段

## 三、服务型子系统

| 子系统 | 提供 | 状态 | 现在 | 未来 |
|---|---|---|---|---|
| **tty** | 字符流 + 行编辑 + font 渲染 | 全局（一块屏） | ❌（现为 console） | 从 console 演化 |
| **graphics** | 画线/矩形/blit/纹理 | 全局（一个屏幕） | ❌ | 基于 screen dev |
| **time** | tick / RTC / 定时器 | 全局 | 部分（tick） | 加 RTC |
| **signal** | 异步通知 | per-process 属性 | ❌ | 依赖 proc |
| **security** | uid/gid/capability | per-process 属性 | ❌ | 加权限检查 |

**特点**：
- 用户不"打开"它们，直接调 API
- 状态是全局的（或进程的属性）
- 没有句柄表

## 四、基础设施（不是子系统）

这些是**所有子系统共用的地基**，用户不直接调用：

| 层 | 内容 |
|---|---|
| memory | pmm + vmm + heap |
| scheduler | task + switch_to + 时钟调度 |
| interrupt | idt + isr + syscall 入口 |
| boot | start.asm + gdt + tss + multiboot |

**基础设施不属于任何子系统**，被所有子系统依赖。

## 五、依赖关系

```
                 用户程序 / shell
                       │
                  syscall 层（胶水）
                       │
        ┌──────────────┼──────────────┬──────────────┐
        ▼              ▼              ▼              ▼
    ┌───────┐      ┌───────┐      ┌───────┐     ┌───────┐
    │ proc  │      │  fs   │      │  dev  │     │  net  │
    └───────┘      └───────┘      └───────┘     └───────┘
        │              │              │              │
        │              │              │              │
        │              │        ┌─────┴──────┐       │
        │              │        │            │       │
        │              ▼        ▼            ▼       ▼
        │        ┌──────────────────────────────┐
        │        │  dev 内核接口（dev_get_ops）  │
        │        └──────────────────────────────┘
        │                    │
        │                    ▼
        │              硬件驱动
        │
        ▼
    ┌────────────────────────────────────────┐
    │  服务型子系统                            │
    │  tty  graphics  time  signal  security  │
    └────────────────────────────────────────┘
                       │
                       ▼
    ┌────────────────────────────────────────┐
    │  基础设施                                │
    │  memory  scheduler  interrupt  boot     │
    └────────────────────────────────────────┘
```

**关键规则**：

1. **对象型之间互不依赖**：fs 不知道 proc 存在，proc 不调 fs。需要协作时由 syscall 层胶水。
2. **对象型依赖 dev 内核接口**：fs、net、tty、graphics 都通过 `dev_get_ops` 访问硬件。
3. **服务型依赖对象型**：tty 依赖 dev（内核接口），signal 依赖 proc。
4. **所有子系统依赖基础设施**。

## 六、"proc 加载 ELF 需要 fs"怎么处理

这是个反例。`spawn` 系统调用要读 ELF 才能创建进程。

**两种做法**：

**A. proc 内部调 fs**：
```c
// proc.c
proc_create(path) {
    fd = fs_open(path);       // ← proc 调 fs
    elf = fs_read(fd);
    ...
}
```
**问题**：proc 依赖 fs，破坏了"对象型之间互不依赖"。

**B. syscall 层胶水**：
```c
// syscall.c
sys_spawn(path) {
    elf = fs_load_elf(path);   // 调 fs
    proc_create_from_elf(elf); // 调 proc
    ...
}
```
**优点**：proc 和 fs 各自独立，谁都不知道对方。syscall 层协调。

**走 B**。这和"内核早期直调 fat32，不经过 fs 层"是同构的——**协作发生在 syscall 层，不在子系统内部**。

## 七、为什么这样分

对照你的哲学：

| 原则 | 落实 |
|---|---|
| 子系统独立 | 对象型各有句柄空间、API、syscall 段 |
| 子系统内抽象 | fs 内统一 `fs_*`，dev 内统一 `device_ops` |
| 拒绝跨子系统统一 | 没有 VFS；没有统一 fd 表；没有"一切皆文件" |
| 内核 vs 进程两套视图 | dev 有"用户句柄视图"和"内核 ops 视图" |
| 分层单向 | 用户 → syscall → 子系统 → dev → 驱动，不反向 |

## 八、未来加子系统的成本

每加一个对象型子系统，要做的事：

1. 定义对象类型（`struct net_socket` 等）
2. 定义句柄空间（task 里加 `net_handles[N]`）
3. 写 API（`net_open` / `net_read` / ...）
4. 加 syscall 段（比如 `81~89`）
5. 加释放路径（`task_exit` 时清）

**成本是线性的**，不影响其他子系统。

每加一个服务型子系统，成本更低——只需加 API 和 syscall 段。

## 九、现在 vs 完整 OS

| 层 | 现在 | 完整 OS |
|---|---|---|
| 对象型 | proc, fs, dev | + net, ipc |
| 服务型 | （console 待转 tty） | tty, graphics, time, signal, security |
| 基础设施 | 全部 | 扩展（FPU 保存、SMP、页错误处理） |
| 内核驱动 | ATA, keyboard, screen, serial | + 网卡、USB、声卡… |

## 十、想请你确认

1. **"对象型 / 服务型"这个分法**认可吗？
2. **对象型只有 5 个**（proc/fs/dev/net/ipc）够吗？还是你觉得还有别的？
3. **服务型有哪些**：tty/graphics/time/signal/security——够全吗？
4. **syscall 层胶水**这个方向——proc 和 fs 通过 syscall 层协作，而不是互相调用。接受吗？

定完这个骨架，就可以谈"从现状到目标"的具体路径了。