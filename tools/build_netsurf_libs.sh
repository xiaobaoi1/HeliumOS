#!/bin/bash
# 批量编译 NetSurf 依赖库

set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
NS="$ROOT/user/netsurf-master"
LIBC_INC="$ROOT/user/libc/include"
LOG="$ROOT/build/netsurf_build.log"

mkdir -p "$ROOT/build"
: > "$LOG"

LIBS="libwapcaplet libutf8proc libparserutils libnsutils libnspsl \
      libnsgif libnsbmp libhubbub libcss libdom"

COMMON_ARGS=(
    COMPONENT_TYPE=lib-static
    NSSHARED="$NS/buildsystem"
    PREFIX="$ROOT/build/netsurf-sysroot"
    CC=gcc
    AR=ar
    ARFLG=cru
)

INCLUDES="-I$LIBC_INC"
for lib in $LIBS; do
    [ -d "$NS/$lib/include" ] && INCLUDES="$INCLUDES -I$NS/$lib/include"
    [ -d "$NS/$lib/inc" ]     && INCLUDES="$INCLUDES -I$NS/$lib/inc"
done
[ -d "$NS/libutf8proc/include/libutf8proc" ] && \
    INCLUDES="$INCLUDES -I$NS/libutf8proc/include/libutf8proc"

ok=0; fail=0; failed=""

for lib in $LIBS; do
    dir="$NS/$lib"
    [ -d "$dir" ] || { echo "SKIP $lib"; continue; }

    echo "=== $lib ===" | tee -a "$LOG"

    # 注意：-D_ALIGNED 的值必须带 \" 包住，
    # 否则 make 展开到 shell 时 __attribute__((aligned)) 的括号
    # 会被 /bin/sh 当成语法错误，命令无法执行。
    CFLAGS="-m32 -ffreestanding -fno-pic -fno-stack-protector \
            -include errno.h \
            -D_ALIGNED=\"__attribute__((aligned))\" \
            -I$dir/include -I$dir/src $INCLUDES"

    EXTRA=""
    if [ "$lib" = "libdom" ]; then
        EXTRA="WITH_EXPAT_BINDING=no"
    fi

    ( cd "$dir" && make "${COMMON_ARGS[@]}" $EXTRA CFLAGS="$CFLAGS" ) >>"$LOG" 2>&1
    if [ $? -eq 0 ]; then
        echo "  OK" | tee -a "$LOG"
        ok=$((ok+1))
    else
        echo "  FAIL" | tee -a "$LOG"
        fail=$((fail+1))
        failed="$failed $lib"
    fi
done

echo ""
echo "=== summary: $ok ok, $fail fail ==="
[ -n "$failed" ] && echo "failed:$failed"
echo "log: $LOG"