# third_party

这里只放上游源码、预编译工具和资源，不放 HeliumOS 自己的适配代码。

| 目录 | 内容 |
|---|---|
| `netsurf/` | NetSurf 上游源码及依赖库 |
| `bearssl/` | BearSSL 上游源码 |
| `7zip/` | 7-Zip 解码源码 |
| `tcc/` | TCC 预编译产物 |
| `fonts/` | unifont 字体资源 |

规则：

- 尽量不直接修改上游文件；需要修改时放到 `tools/netsurf/patches/`。
- 构建产物不要提交到 git。
- 升级上游时记录版本或 commit 到各自的 `UPSTREAM` 文件。
