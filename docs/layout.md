# 仓库目录分层

```text
kernel/       内核与驱动
user/         HeliumOS 用户态：libc、应用、UI、资源
third_party/  上游源码与预编译工具
tools/        构建、镜像、调试脚本
build/        构建产物（不提交）
```

## kernel

| 路径 | 内容 |
|---|---|
| `kernel/boot/` | Multiboot2 入口 |
| `kernel/core/` | 内核核心（调度、syscall、ipc、net 等） |
| `kernel/drivers/` | 硬件驱动 |
| `kernel/fs/` | 卷与路径解析 |
| `kernel/include/` | 内核公共头文件 |
| `kernel/memory/` | PMM/VMM/heap |
| `kernel/process/` | task/scheduler |
| `kernel/linker.ld` | 内核链接脚本 |

## user

`user/libc` 与 `user/lib` 是第一方代码，不是第三方库。  
当前 POSIX 兼容层已经合并进 `user/libc`，不再单独保留 `user/posix`。

## third_party

只放上游源码和预编译产物：

```text
third_party/netsurf/
third_party/bearssl/
third_party/7zip/
third_party/tcc/
third_party/fonts/
```

构建产物（`build-*`、`*.a`、`*.o`、`nsmonkey` 等）不提交。

## 常用命令

```bash
make build/kernel.elf
make build/shell.ELF build/desktop.ELF build/wm.ELF build/term.ELF
make build/bearssl/libbearssl.a
make run-disk
```
