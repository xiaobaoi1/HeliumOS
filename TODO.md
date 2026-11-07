# HeliumOS TODO List（更新版）

---

## 一、已完成基线

```
✅ .bss 清零（elf_loader 用 addr += 1）
✅ PCB 大小溢出（删 fat32_file.buffer）
✅ sys_kill 释放 self 引用
✅ sys_kill 从 blocked_list 摘链
✅ proc_free_pcb 从所有队列摘链
✅ wake_up_waiters 用 grave_next（不污染 proc_next）
✅ 新架构上下文切换（switch_to 重写 + enter_user_mode）
✅ syscall_regs 机制移除
✅ 用户指针检查统一 check_user_range
✅ 清理注释痕迹（★ ← 等）
✅ fat32 LFN 检查
✅ task_create 部分失败回滚
✅ sys_brk 部分失败回滚
✅ fat32 cluster 环检测
✅ H1 用户指针页映射检查（方案 A）
✅ H2 PMM/VMM 范围对齐（PSE 4MB 大页，0..1GB）
✅ H14 fs/dev release_all 遍历调 close（方向 A）
✅ C1-C4 调试日志与残留清理
✅ F1 argv 传递（crt0 + 栈布局 + spawn 传参）
```

---

## 二、架构演进路线

### 批次 A：dev 扩权 + tty 诞生（消解 console）

**目标**：dev 成为唯一硬件入口；console 分解为 screen dev + tty 子系统。

| 项 | 说明 | 备注 |
|---|---|---|
| A.1 screen 注册为 dev | vga_ops（write 写像素 / ioctl 做 clear/color/cursor） | DEV_TYPE_VGA 已定义 |
| A.2 tty 子系统诞生 | 从 console.c 演化；内部经 `dev_get_ops` 访问 screen / keyboard | font 全局一份 |
| A.3 device.c 加 `dev_get_ops` | 内核态拿 ops 的接口 | ~10 行 |
| A.4 syscall 13~19 改调 tty_* | 段语义重定位 | 顺带处理 H3 |
| A.5 合并 H3 | tty_set_color 参数范围检查 | 随 A.4 一起 |
| A.6 合并 F7 | 串口注册 DEV_TYPE_SERIAL；kprintf 走 dev | 若并入则 A 变"三设备" |
| A.7 删除 console.c | 或改名 tty.c | — |

**完成标志**：`[TTY]` 启动日志；`dev_open(DEV_TYPE_VGA)` 可用；printf/shell 一切正常。

**遗留**：键盘字符分叉（`read(0)` 与 `dev_open(KEYBOARD)` 抢同一缓冲）→ 推到 C.2。

---

### 批次 B：标准流 slot（F11 的具体化）

**目标**：`read(0)` / `write(1)` 从魔数变成进程 slot 分派。

| 项 | 说明 |
|---|---|
| B.1 task 加 stdin/stdout/stderr slot | NULL = 默认 tty |
| B.2 io_slot 结构 | type + data + refcount |
| B.3 sys_read / sys_write 分派 | NULL/TTY → tty；FILE → fs；DEV → dev；NULL → 丢弃 |
| B.4 spawn arg3 编码重定向 | 高 16 位 stdin fd，低 16 位 stdout fd，-1 = 不重定向 |
| B.5 task_exit 释放 slot | — |
| B.6 合并 H9 | 移除 waitpid，统一到 wait(handle, ...) |

**依赖**：批次 A（tty 存在）。

**完成标志**：`cmd > file`、`cmd < file`、`echo hi > /dev/null` 可用。

**遗留**：slot 继承（spawn 时子进程默认 slot 置 NULL）→ 将来再做。

---

### 批次 C：子系统补全

三个独立子任务：

| 项 | 说明 | 复杂度 |
|---|---|---|
| C.1 前台 / 后台 + 键盘归属 | tty 有"当前前台"；键盘输入只发前台；shell 用 tty_set_foreground | 1~2 天 |
| C.2 信号 | proc 加 pending_signal；kill 扩展语义；用户 handler；Ctrl+C | 2~3 天 |
| C.3 IPC（pipe） | 独立子系统；pipe_handle 表；slot 加 SLOT_PIPE；阻塞读 | 3~5 天 |
| C.4 合并 A1 | 孤儿进程 reaper（init 进程模式）| 半天 |
| C.5 合并 H4 | normalize_path 栈使用降到安全范围 | 中 |

**顺序**：C.4（A1 合并）→ C.1 → C.2 → C.3 → C.5。

**完成标志**：Ctrl+C 能终止前台进程；`cmd1 | cmd2` 可用；父进程先退不影响子进程。

---

### 批次 D：图形子系统（远期）

- graphics 子系统（gfx_draw_line / fill_rect / blit / set_pixel）
- 独立 gfx_handles
- tty 与 graphics 共享屏幕（分区或全屏切换）
- 依赖 screen dev 的"写像素矩阵"接口（批次 A 已提供）

---

### 批次 E：网络 / 安全（远期）

- net 子系统（socket API，net_handles，网卡驱动）
- security 子系统（uid/gid/capability）

---

## 三、加固 Backlog（H 剩余）

| 编号 | 问题 | 位置 | 优先级 | 备注 |
|---|---|---|---|---|
| H1.B | Exception table（用户访问识别） | isr_handler + stub | 低 | mmap / demand paging / SMP 前 |
| H5 | vmm_map_user_page 不检查重复映射 | vmm.c | 低 | 当前调用者无重复 |
| H6 | sys_spawn 失败路径与 task_create 一致性 | syscall.c | 低 | 罕见路径 |
| H7 | 双 default_volume | fat32.c / volume.c | 低 | 无实际影响 |
| H8 | user_stack_phys 死字段 | task.h | 低 | 可随任何 task.c 改动清 |
| H10 | wake_up_waiters 里 wait_target==NULL 模糊 | task.c | 低 | 保留 + 注释 |
| H11 | elf_loader 不检查 filesz > memsz | elf_loader.c | 低 | 恶意 ELF |
| H12 | elf_loader vaddr + memsz 溢出 | elf_loader.c | 低 | 恶意 ELF |
| H13 | fs_open 打开目录语义 | fs.c / fat32.c | 低 | 明确返回 EISDIR |
| F8 | 内核堆 > 2048 | heap.c | 中 | 需要时再做 |
| F9 | FAT32 长文件名支持 | fat32.c | 低 | 当前跳过 LFN |
| F10 | FAT32 扇区缓存 | fat32.c | 低 | 性能 |
| F5 | RTC 时间 | 新模块 | 低 | CMOS |
| F6 | 关机 / 重启 | 新模块 | 低 | ACPI / QEMU 端口 |

**合并建议**：
- H8 随批次 A 的 task.c 改动顺带清
- H3 已在批次 A.5
- H9 已在批次 B.6
- H4 已在批次 C.5

---

## 四、基础设施 Backlog（A 剩余）

| 编号 | 问题 | 优先级 |
|---|---|---|
| A2 | 进程数上限 / pid 复用 | 低 |
| A3 | 内核抢占 | 低 |
| A4 | 多核 SMP | 低（大重构） |

---

## 五、推荐执行顺序

```
1. 批次 A（dev 扩权 + tty + 串口 + 清 H3/H8）   约 1~2 天
2. 批次 B（slot + 清 H9）                        约 2~3 天
3. 批次 C（前台/信号/pipe/孤儿/清 H4）           约 1 周
4. 加固 backlog 按需穿插
5. 批次 D（graphics）                            远期
6. 批次 E（net / security）                      远期
```

---

## 六、文件位置

建议放 `TODO.md`。当前对话里讨论过的"完整 OS 子系统地图"（对象型 / 服务型 / 基础设施的分层）可以另存为 `docs/architecture.md` 或 `ARCHITECTURE.md`，作为设计参考。

---

要不要现在把这份 TODO 落成实际文件？还是先看别的？










## 当前更新的 Backlog

### 待办

| 编号 | 问题 | 位置 | 优先级 | 备注 |
|---|---|---|---|---|
| H1.B | 异常表（用户访问识别） | `isr_handler` + stub | 低 | 按需分页 / mmap / SMP 前 |
| H5 | `vmm_map_user_page` 不检查重复映射 | `vmm.c` | 低 | **本次处理** |
| H6 | `sys_spawn` 失败路径与 `task_create` 一致性 | `syscall.c` | 低 | 核对过两条失败路径，均干净；需具体场景才能确认 |
| H7 | 两层卷表示（volume vs fat32_volume）命名区分 | `volume.c` / `fat32.c` | 低 | 无实际影响 |
| H10 | `wake_up_waiters` 里 `wait_target==NULL` 模糊 | `task.c` | 低 | 保留 + 补注释 |
| H14 | `acpi_poweroff` 遍历 SLP_TYPa 候选值 | `acpi.c` | 低 | 已知简化，等 AML 解析器 |
| H15 | ACPI 表校验和（RSDT/XSDT 内部表） | `acpi.c` | 低 | 表损坏时可能崩 |
| H16 | XSDT 表地址 > 4GB 跳过 | `acpi.c` | 低 | UEFI 64 位系统找不到 FADT |
| F8 | 内核堆 > 2048 | `heap.c` | 中 | 需要时再做 |
| F10 | FAT32 数据扇区缓存 | `fat32.c` | 低 | FAT 表已有单槽缓存，数据扇区没有 |

### 已修 / 已实现（从 backlog 移除）

- ~~H8~~ `user_stack_phys` 字段不存在
- ~~H11~~ `filesz > memsz` 检查已在
- ~~H12~~ `vaddr + memsz` 溢出检查已在
- ~~H13~~ `fs_open` 目录返回 `EISDIR` 已在
- ~~F5~~ RTC 已做
- ~~F6~~ 关机 / 重启已做
- ~~F9~~ FAT32 LFN 已实现

### 字段说明

- H 前缀：加固项（防御 / 正确性 / 边界）
- F 前缀：功能项（新能力）
- 优先级：低 = 不主动做，遇到再做；中 = 到某规模前必做