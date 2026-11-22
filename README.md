# Dependence
```bash
sudo apt update
sudo apt install -y \
    build-essential nasm gcc-multilib \
    grub-pc-bin grub-common \
    xorriso mtools \
    qemu-system-x86 \
    gdb-multiarch
```

# Documentation

- [技术实现手册](docs/manual_26_11_11.md)：内核/用户态链路、数据结构、已知问题清单
- [目录结构说明](docs/layout.md)：`kernel/`、`user/`、`third_party/` 分层
- [TODO](TODO.md)：架构演进和 backlog
