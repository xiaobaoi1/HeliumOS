# NetSurf 在 HeliumOS 上的移植说明

本文记录把 NetSurf 浏览器核心移植到 HeliumOS 时，对 NetSurf 源码、构建脚本和 HeliumOS libc 所做的修改。目标是让 NetSurf 的 `monkey` 前端能够在 HeliumOS 用户态运行，并通过本地 `file://` URL 渲染 HTML。

当前状态：

- 10 个 NetSurf 依赖库可以编译：`libwapcaplet`、`libutf8proc`、`libparserutils`、`libnsutils`、`libnspsl`、`libnsgif`、`libnsbmp`、`libhubbub`、`libcss`、`libdom`。
- NetSurf `monkey` 前端可以链接成 HeliumOS 原生程序 `build/nsmonkey.elf`。
- `file:///test.html` 可以正常完成 fetch、HTML 解析、CSS 选择、DOM → box、布局和绘制。
- 当前只验证了本地文件；HTTP、TLS、JavaScript 和多数图片解码尚未启用。

---

## 1. 目录与产物

```text
third_party/netsurf/                 NetSurf 上游源码及依赖库
  content/                           NetSurf 内容处理
  frontends/monkey/                  monkey 前端
  libcss/ libdom/ libhubbub/ ...     随 NetSurf 一起引入的依赖库
  resources/                         默认 CSS、消息、图标等运行时资源

build/netsurf-sysroot/               为 NetSurf 生成的 pkg-config、静态库和头文件
build/nsmonkey.elf                   HeliumOS 原生 NetSurf 可执行文件

tools/build_netsurf_libs.sh          编译 10 个依赖库
tools/build_netsurf_core.sh          配置并编译 NetSurf 核心
tools/ld_netsurf.sh                  用 HeliumOS linker script 链接最终 ELF
tools/gen_netsurf_res.py             资源转 C 数组的辅助脚本（目前未接入默认构建）
```

镜像中的布局：

```text
/NSMONKEY                            build/nsmonkey.elf
/netsurf/res/                        third_party/netsurf/resources/*
/test.html                           make image 生成的测试页
```

`build/nsmonkey.elf` 的运行时资源路径由 `tools/build_netsurf_core.sh` 生成的 `netsurf_defines.h` 指定：

```c
#define MONKEY_RESPATH "SYS:/netsurf/res/"
```

---

## 2. 修改文件总览

### 2.1 NetSurf 源码

| 文件 | 修改目的 |
|---|---|
| `third_party/netsurf/content/fetchers/file/file.c` | 用 HeliumOS 的 `fs_open`/`fs_read`/`fs_close` 替代 POSIX `open`/`mmap`/`fopen`，实现本地文件读取 |
| `third_party/netsurf/utils/file.c` | 把 `file:///...` 的 POSIX 路径转换为 HeliumOS 卷符路径 `SYS:/...` |
| `third_party/netsurf/frontends/monkey/main.c` | 进入 monkey 主循环前把 PTY 切换到 cooked 模式，退出后恢复 raw 模式 |
| `third_party/netsurf/content/handlers/html/html.c` | 无 charset 时默认 UTF-8，避免编码变更路径误报 `NoMemory` |

### 2.2 构建与镜像集成

| 文件 | 修改目的 |
|---|---|
| `makefile` | 增加 `nsmonkey` 目标；`image` 目标复制 `NSMONKEY`、NetSurf 资源和测试 HTML |
| `tools/build_netsurf_libs.sh` | 以 `-m32 -ffreestanding` 批量编译 NetSurf 依赖库 |
| `tools/build_netsurf_core.sh` | 生成 NetSurf sysroot、pkg-config、兼容桩，并编译 `nsmonkey` |
| `tools/ld_netsurf.sh` | 用 HeliumOS 的 `user/linker.ld` 和 `libc.a` 链接 NetSurf 对象 |
| `.gitignore` | 忽略生成的 `nsmonkey`，同时保留依赖库的源码 `build/` 目录 |

### 2.3 HeliumOS libc 兼容改动

| 文件 | 修改目的 |
|---|---|
| `user/libc/string.c` | 增加 `strcasestr()`，供 HTML/CSS 媒体查询等代码使用 |
| `user/libc/include/string.h` | 增加 `strcasestr()` 声明 |
| `user/libc/malloc.c` | 修正 `malloc(0)`、brk 增长记账和大块 mmap 失败后的 brk 回退 |
| `user/libc/include/errno.h` | 补充 `E2BIG`、`EILSEQ` 等 NetSurf/parserutils 需要的错误码 |
| `user/libc/include/curl/curl.h` | 提供最小 `CURLM` 类型 stub，满足 `NETSURF_USE_CURL=NO` 时的头文件包含 |
| `user/libc/include/execinfo.h` | 提供 `backtrace()` 系列声明 stub，供 assert/调试代码编译 |
| `user/libc/regex.c` | 为 NetSurf `save_complete.c` 的 `<base href>` 提取提供最小正则能力 |
| `user/libc/zlib.c` | 适配 NetSurf hashtable 的一次性内存块用法 |
| `user/libc/time.c` | 时区/时间格式化返回安全 fallback |
| `user/libc/iconv.c`、`user/libc/math.c` | 构建脚本用它们生成 `libiconv.a`、`libm.a` |

---

## 3. NetSurf 源码修改详情

### 3.1 `content/fetchers/file/file.c`：本地文件 fetcher

上游 NetSurf 的 file fetcher 依赖 POSIX 文件接口，并根据是否启用 `HAVE_MMAP` 选择：

- `open()` + `mmap()` 读取；
- 或 `fopen()` + `fread()` 读取。

HeliumOS 用户态没有完整的 POSIX 文件描述符和 `mmap` 文件映射语义，因此改为使用 HeliumOS 的 fs 系统调用封装：

```c
#include "syscall.h"

fd = fs_open(ctx->path, O_RDONLY);
...
n = fs_read(fd, buf + size, FETCH_FILE_MAX_BUF_SIZE - size);
...
fs_close(fd);
```

当前实现的行为：

1. 用 `fs_open(path, O_RDONLY)` 打开文件。
2. 一次性分配 `FETCH_FILE_MAX_BUF_SIZE`（1 MiB）缓冲区。
3. 循环调用 `fs_read()`，最多读取 1 MiB。
4. 发送 `Content-Type`、实际读取到的 `Content-Length`。
5. 发送一次 `FETCH_DATA`，最后发送 `FETCH_FINISHED`。

与上游相比的差异：

- 移除了 `mmap()` / `fopen()` / `fread()` 分支。
- 移除了 `fstat()` 的 `ETag` 和 `304 Not Modified` 逻辑；参数 `fdstat` 目前不使用。
- `Content-Length` 使用实际读取的字节数，而不是 `stat.st_size`。
- 打开失败返回 404，读取失败返回 500。
- 文件大于 1 MiB 时只会读取前 1 MiB。

该文件仍然保留 NetSurf 原有的目录列表、URL 解析和 fetch 回调框架。

### 3.2 `utils/file.c`：`file://` 到 HeliumOS 路径的转换

NetSurf 默认把 `file:///test.html` 转换成一个 POSIX 绝对路径，例如：

```text
/test.html
```

HeliumOS 的文件路径需要卷符，例如：

```text
SYS:/test.html
```

因此在 `posix_nsurl_to_path()` 的末尾增加了一段转换逻辑：

```c
if (strncmp(path, "/SYS:/", 6) == 0) {
    helium = strdup(path + 1);              /* /SYS:/x -> SYS:/x */
} else if (strncmp(path, "/SYS/", 5) == 0) {
    helium = malloc(n + 3);                 /* /SYS/x -> SYS:/x */
    ...
} else if (path[0] == '/') {
    helium = malloc(n + 5);                 /* /x -> SYS:/x */
    ...
} else {
    helium = malloc(n + 6);                 /* x -> SYS:/x */
    ...
}
```

覆盖的情况：

| URL 解析后的路径 | 转换结果 |
|---|---|
| `/test.html` | `SYS:/test.html` |
| `/SYS:/test.html` | `SYS:/test.html` |
| `/SYS/test.html` | `SYS:/test.html` |
| `test.html` | `SYS:/test.html` |

转换失败时释放旧路径并返回 `NSERROR_NOMEM`。

### 3.3 `frontends/monkey/main.c`：PTY cooked/raw 切换

NetSurf 的 monkey 前端通过标准输入逐行读取命令，例如：

```text
WINDOW NEW file:///test.html
WINDOW REDRAW 0
```

它使用 `fgets()` 读取整行，因此要求终端处于 cooked（行缓冲）模式。HeliumOS shell 在启动子进程前可能把 PTY 留在了 raw 模式，导致 monkey 收不到完整行而挂起。

修改方式：

```c
#include "syscall.h"

term_set_raw(0);        /* 进入 NetSurf 前切换为 cooked */
moutf(MOUT_GENERIC, "STARTED");
monkey_run();
term_set_raw(1);        /* 退出后恢复 shell 使用的 raw 模式 */
```

`term_set_raw(0)` 表示行缓冲/回显模式，`term_set_raw(1)` 表示 raw 模式。

### 3.4 `content/handlers/html/html.c`：HTML 编码处理

这是本次移植最后修复的关键问题。

#### 问题现象

`file:///test.html` fetch 成功、CSS 读取成功、`dom_to_box()` 返回成功，但窗口仍然显示：

```text
FetchErrorTitle
NoMemory
```

调试日志显示：

```text
HTML: process_data dom_ret=65541
```

`65541 = 65536 + 5`：

```text
DOM_HUBBUB_HUBBUB_ERR | HUBBUB_ENCODINGCHANGE
```

说明 HTML 解析器在第一次收到数据时触发了编码变更事件。随后错误页被当作新的 HTML 内容解析，所以后续日志看起来“解析成功”，但渲染的是错误页。

#### 修改一：没有 charset 时默认 UTF-8

在 `html_create_html_data()` 中，如果 HTTP 参数没有提供 charset，则直接以 UTF-8 创建解析器：

```c
if (c->encoding == NULL) {
    c->encoding = strdup("UTF-8");
    if (c->encoding == NULL) {
        ...
        return NSERROR_NOMEM;
    }
    c->encoding_source = DOM_HUBBUB_ENCODING_SOURCE_DETECTED;
}
```

这样 `parse_params.enc` 非空，hubbub 以“已确定 UTF-8”的状态启动，不会在第一个数据块上再触发编码变更。

#### 修改二：编码变更路径不再直接返回 NOMEM

在 `html_process_encoding_change()` 中：

```c
encoding = dom_hubbub_parser_get_encoding(...);
if (encoding == NULL) {
    encoding = "UTF-8";
}
```

上游这里返回 `NSERROR_NOMEM`。对于 HeliumOS 当前的文件 fetcher，这种 `NULL` 实际表示“没有检测到可用的编码名”，不是内存不足；直接返回 NOMEM 会让浏览器误显示 `NoMemory` 错误页。

#### 修改三：开始转换时同样使用 UTF-8 fallback

在 `html_begin_conversion()` 中：

```c
if (encoding == NULL) {
    encoding = "UTF-8";
}
```

原先该分支会广播 `NSERROR_NOMEM` 并返回 `false`。现在改为继续以 UTF-8 完成转换。

这些修改共同保证：

- 文件内容没有 charset 时使用 UTF-8；
- 编码检测失败不会被误判为 OOM；
- 页面不会因为编码事件被替换成 `FetchErrorTitle` 错误页。


---

## 4. 构建脚本说明

### 4.1 `tools/build_netsurf_libs.sh`

负责编译 NetSurf 的 10 个依赖库：

```text
libwapcaplet libutf8proc libparserutils libnsutils libnspsl
libnsgif libnsbmp libhubbub libcss libdom
```

关键点：

- 使用宿主 `gcc -m32`，不是交叉编译器。
- 使用 `-ffreestanding -fno-pic -fno-stack-protector`，与 HeliumOS 用户态 ABI 保持一致。
- 通过 `-Iuser/libc/include` 使用 HeliumOS 的 libc 头文件。
- 通过 `-include errno.h` 补齐部分库需要的 errno 定义。
- 用 `-D_ALIGNED="__attribute__((aligned))"` 解决构建系统中 `_ALIGNED` 宏展开到 shell 时的括号问题。
- `libdom` 关闭 expat binding：`WITH_EXPAT_BINDING=no`。
- 静态库输出到 `build/netsurf-sysroot/lib/`。

### 4.2 `tools/build_netsurf_core.sh`

负责准备 NetSurf 核心的构建环境：

1. 把 10 个依赖库复制到 `build/netsurf-sysroot/lib/`。
2. 生成 `.pc` 文件，按依赖关系声明 `Requires`。
3. 用 HeliumOS libc 的 `zlib.c`、`inflate.c` 生成 `libz.a`。
4. 用 `math.c` 和浮点 stub 生成 `libm.a`。
5. 用 `iconv.c` 生成 `libiconv.a`。
6. 生成 `libnetsurf_stubs.a`。
7. 生成 `netsurf_defines.h`，其中包含：

```c
#define MONKEY_RESPATH "SYS:/netsurf/res/"
```

8. 生成一个 `gcc32-stub-wrapper.sh`，在链接阶段自动追加 `-lnetsurf_stubs`。
9. 以 `TARGET=monkey` 调用 NetSurf 自带 Makefile。

当前配置：

```text
NETSURF_USE_CURL=NO
NETSURF_USE_OPENSSL=NO
NETSURF_USE_JPEG=NO
NETSURF_USE_JPEGXL=NO
NETSURF_USE_PNG=NO
NETSURF_USE_WEBP=NO
NETSURF_USE_DUKTAPE=NO
NETSURF_USE_NSLOG=NO
NETSURF_USE_LIBICONV_PLUG=NO
NETSURF_USE_NSSVG=NO
NETSURF_USE_ROSPRITE=NO
NETSURF_USE_BMP=YES
NETSURF_USE_GIF=YES
NETSURF_USE_NSPSL=YES
NETSURF_USE_UTF8PROC=YES
NETSURF_FS_BACKING_STORE=NO
```

因此当前版本支持 BMP/GIF，不支持 PNG/JPEG/WebP/SVG/JavaScript/curl/TLS。

`libnetsurf_stubs.a` 目前包含：

```c
void fetch_file_register(void) { }
const unsigned char filesystem_llcache_table[256] = { 0 };
```

它们是链接期的兼容桩；真正的 file fetcher 仍然由 NetSurf 的 `content/fetchers/file/file.c` 提供。

### 4.3 `tools/ld_netsurf.sh`

把 NetSurf 对象和 HeliumOS libc 链接成最终 ELF：

```bash
objs=$(find third_party/netsurf/build/Linux-monkey -name '*.o' ! -path '*/tools/*')
ld -m elf_i386 -nostdlib --allow-multiple-definition -T user/linker.ld \
   -o build/nsmonkey.elf build/crt0.o \
   --start-group $objs \
     -Lbuild/netsurf-sysroot/lib \
     -lcss -ldom -lhubbub -lwapcaplet -lparserutils \
     -lnsutils -lnsgif -lnsbmp -lnspsl -lutf8proc \
     -lnetsurf_stubs -lm -lz \
     build/libc.a \
   --end-group \
   /usr/lib/gcc/x86_64-linux-gnu/15/32/libgcc.a
```

关键点：

- 使用 HeliumOS 的 `user/linker.ld`。
- 使用 `build/crt0.o` 作为用户态入口。
- 使用 `build/libc.a`，而不是宿主 glibc。
- 允许重复定义，便于 NetSurf 兼容桩和实际实现共存。

### 4.4 `makefile`

新增目标：

```make
nsmonkey:
	bash tools/build_netsurf_libs.sh
	bash tools/build_netsurf_core.sh
	bash tools/ld_netsurf.sh
```

`image` 目标增加：

- 如果存在 `build/nsmonkey.elf`，复制为镜像根目录 `/NSMONKEY`；
- 删除旧的 `/netsurf` 目录；
- 创建 `/netsurf/res`；
- 复制 `third_party/netsurf/resources/*` 到 `/netsurf/res/`；
- 生成 `/test.html` 测试页。

### 4.5 `.gitignore`

- 忽略 `third_party/netsurf/nsmonkey` 和 `build/` 产物。
- 用 negate 规则保留 `third_party/netsurf/lib*/build/` 中的生成脚本和源数据，避免误删依赖库的一部分源码。

---

## 5. HeliumOS libc 兼容改动

NetSurf 及其依赖库大量使用 POSIX/glibc 扩展。HeliumOS 的 libc 只实现其中一部分，因此补了以下内容。其中部分兼容代码在更早的移植分支中已经加入，这里统一列出，作为 NetSurf 运行所依赖的 libc 基线。

### 5.1 `strcasestr()`

文件：

```text
user/libc/string.c
user/libc/include/string.h
```

实现大小写不敏感的子串查找，返回指向 `haystack` 中首次匹配位置的指针。NetSurf 的 HTML `<link media>` 判断等代码会使用它。

### 5.2 `malloc` 修正

文件：`user/libc/malloc.c`

修改点：

- `malloc(0)` 返回一个可释放的最小块，而不是 `NULL`。部分第三方代码把 `malloc(0) == NULL` 当作 OOM。
- 大分配优先 `mmap`，失败后回退到 `brk`。
- `brk` 增长后记录完整的 `g_grow_size`，避免只记录请求大小而导致堆块覆盖。
- 分配失败时输出 `MALLOC mmap fail` / `MALLOC brk fail` 诊断。

这些修改解决了 NetSurf 在解析和布局阶段出现的假性 OOM。

### 5.3 errno 补充

文件：`user/libc/include/errno.h`

补充：

```c
E2BIG
EILSEQ
```

`libparserutils` 和字符集处理会引用这些错误码。

### 5.4 curl / execinfo 头文件 stub

文件：

```text
user/libc/include/curl/curl.h
user/libc/include/execinfo.h
```

- `curl/curl.h` 只声明 `CURLM` 类型。`NETSURF_USE_CURL=NO` 时不会编译 curl fetcher，但 NetSurf 的 `content/fetchers/curl.h` 会无条件包含该头文件。
- `execinfo.h` 声明 `backtrace()` 系列函数。HeliumOS 不实现完整栈回溯，assert 失败时返回 0 作为最小实现。

### 5.5 其他兼容实现

| 文件 | 说明 |
|---|---|
| `user/libc/regex.c` | 为 NetSurf `save_complete.c` 提取 `<base href="...">` 提供最小正则支持 |
| `user/libc/zlib.c` | 适配 NetSurf hashtable 对完整内存块的 inflate 用法 |
| `user/libc/time.c` | 时间/时区格式化返回安全 fallback，避免 NetSurf 初始化失败 |
| `user/libc/iconv.c` | 作为 `libiconv.a` 的构建输入 |
| `user/libc/math.c` | 作为 `libm.a` 的构建输入 |


---

## 6. 运行时流程

以 `file:///test.html` 为例：

1. monkey 前端收到：

```text
WINDOW NEW file:///test.html
```

2. NetSurf 的 file fetcher 解析 URL。
3. `utils/file.c` 把路径转换为：

```text
SYS:/test.html
```

4. `content/fetchers/file/file.c` 调用：

```c
fs_open("SYS:/test.html", O_RDONLY);
fs_read(...);
fs_close(...);
```

5. NetSurf 读取默认资源：

```text
SYS:/netsurf/res/default.css
SYS:/netsurf/res/internal.css
SYS:/netsurf/res/quirks.css
```

6. DOM 解析完成后创建 CSS selection context。
7. `dom_to_box()` 构建 box tree。
8. 布局并调用 monkey 的 plot 接口输出：

```text
PLOT TEXT ... STR HeliumOS NetSurf
PLOT TEXT ... STR Hello world!
```

---

## 7. 构建与测试

### 7.1 构建

首次构建需要先生成 HeliumOS 用户态 libc 和 crt0：

```bash
make libc.a build/crt0.o
```

然后：

```bash
make nsmonkey
```

或者分步执行：

```bash
bash tools/build_netsurf_libs.sh
bash tools/build_netsurf_core.sh
bash tools/ld_netsurf.sh
```

生成：

```text
build/nsmonkey.elf
```

### 7.2 生成镜像

```bash
make image
```

该步骤需要 `sudo`。

### 7.3 运行

```bash
make run-disk
```

进入 HeliumOS 后：

```text
nsmonkey
WINDOW NEW file:///test.html
WINDOW REDRAW 0
```

预期窗口内容：

```text
HeliumOS NetSurf
Hello world!
```

当前测试页没有 `<title>`，所以窗口标题显示 URL `file:///test.html` 是正常现象。

### 7.4 构建产物检查

```bash
file build/nsmonkey.elf
```

应为：

```text
ELF 32-bit LSB executable, Intel 80386, statically linked, not stripped
```

---

## 8. 已知限制

1. **只支持本地文件**
   - `NETSURF_USE_CURL=NO`，尚未接入 HTTP/HTTPS。
   - `NETSURF_USE_OPENSSL=NO`，尚未接入 TLS。

2. **文件大小限制**
   - `FETCH_FILE_MAX_BUF_SIZE` 为 1 MiB。
   - 超过 1 MiB 的本地文件只会读取前 1 MiB。

3. **无 ETag / 304**
   - file fetcher 不再发送 `ETag`，也不会处理 `304 Not Modified`。
   - 这与 HeliumOS 当前缺少完整 `fstat` 文件元数据有关。

4. **编码策略**
   - 文件内容没有 charset 时默认 UTF-8。
   - 非 UTF-8 内容（例如 GBK、Shift-JIS）目前没有完整自动检测流程。

5. **图片和脚本**
   - 已启用 BMP/GIF。
   - 未启用 PNG/JPEG/WebP/SVG/JavaScript。

6. **资源打包脚本**
   - `tools/gen_netsurf_res.py` 目前只是辅助脚本，默认构建不会生成或链接 `builtin_resources.c`。
   - 运行时资源仍从 `/netsurf/res/` 读取。

7. **malloc 诊断**
   - `user/libc/malloc.c` 仍保留 `MALLOC ... fail` 输出，便于后续排查内存问题；可以在稳定后改为编译开关。

---

## 9. 后续可做的工作

- 把 `file://` fetcher 改成真正的流式读取，避免 1 MiB 上限和整文件缓冲。
- 恢复 `fstat`/`ETag`，支持 304 缓存。
- 增加 PNG/JPEG 解码库。
- 接入 curl/OpenSSL 或 HeliumOS 原生网络栈。
- 完善非 UTF-8 HTML 的编码检测。
- 清理 `malloc` 诊断输出或改成 `MALLOC_DEBUG` 编译选项。
- 把 `gen_netsurf_res.py` 接入构建，提供不依赖文件系统的内置资源 fallback。

---

## 10. 相关提交

```text
dff0354 chore: remove temporary NetSurf fetch and HTML diagnostics
da3eddc fix: start HTML parser in UTF-8 when no charset is supplied
fecf3c8 debug: trace HTML encoding-change parser recreation
944e620 debug: guard zero-length HTML chunk logging
3c38964 fix: fall back to UTF-8 on HTML encoding change without charset
99cdbc6 debug: log HTML parser chunk result
636c7cd debug: trace content error source and box completion
86813c1 debug: trace DOM to box conversion result
1816133 debug: report HTML CSS selection failure codes
2c387b7 fix: fallback to UTF-8 when no HTML encoding is detected
c482750 fix: stream local files and add NSFILE diagnostics
3e63371 fix: use cooked pty for monkey line input and create netsurf resource dir
508449d feat: port NetSurf core to main layout and add native file fetcher
```

其中与最终行为直接相关的提交是：

- `508449d`：file fetcher、路径转换、构建脚本和镜像集成。
- `3e63371`：monkey PTY cooked 模式。
- `da3eddc`：HTML 默认 UTF-8，彻底消除 `NoMemory` 错误页。
- `dff0354`：移除临时调试输出。

