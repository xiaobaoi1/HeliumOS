#!/bin/bash
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
NS="$ROOT/user/netsurf-master"
LIBC_INC="$ROOT/user/libc/include"
LOG="$ROOT/build/netsurf_core.log"
SYSROOT="$ROOT/build/netsurf-sysroot"
PCDIR="$SYSROOT/lib/pkgconfig"
TMP="$ROOT/build/netsurf-tmp"
HOST_DIR="build-x86_64-linux-gnu-x86_64-linux-gnu-release-lib-static"
LIB_CFLAGS="-m32 -ffreestanding -fno-pic -fno-stack-protector -O2 -I$LIBC_INC"

mkdir -p "$SYSROOT/lib" "$SYSROOT/include" "$PCDIR" "$TMP"

cat > "$SYSROOT/include/netsurf_defines.h" <<'EOF'
#ifndef NETSURF_BUILTIN_LOG_FILTER
#define NETSURF_BUILTIN_LOG_FILTER ""
#endif
#ifndef NETSURF_BUILTIN_VERBOSE_FILTER
#define NETSURF_BUILTIN_VERBOSE_FILTER ""
#endif
EOF
: > "$LOG"

# ---------- 1. 拷 10 个依赖库 ----------
for lib in libwapcaplet libparserutils libutf8proc libnsutils libnspsl \
           libnsgif libnsbmp libhubbub libcss libdom; do
    src="$NS/$lib/$HOST_DIR/$lib.a"
    [ -f "$src" ] && cp "$src" "$SYSROOT/lib/" || echo "MISSING: $src"
done

# ---------- 2. .pc 文件（带 Requires） ----------
gen_pc() {
    local name=$1 ver=$2 desc=$3 incdir=$4
    local requires="${5:-}"
    {
        echo "prefix=$SYSROOT"
        echo "libdir=\${prefix}/lib"
        echo "includedir=$incdir"
        echo ""
        echo "Name: $name"
        echo "Description: $desc"
        echo "Version: $ver"
        [ -n "$requires" ] && echo "Requires: $requires"
        echo "Libs: -L\${libdir} -l${name#lib}"
        echo "Cflags: -I\${includedir}"
    } > "$PCDIR/$name.pc"
}

gen_pc libwapcaplet   0.4.3 "libwapcaplet"    "$NS/libwapcaplet/include"
gen_pc libparserutils 0.2.5 "libparserutils"  "$NS/libparserutils/include"
gen_pc libhubbub      0.3.8 "libhubbub"       "$NS/libhubbub/include"       "libparserutils"
gen_pc libcss         0.9.2 "libcss"          "$NS/libcss/include"          "libwapcaplet libparserutils"
gen_pc libdom         0.4.2 "libdom"          "$NS/libdom/include"          "libwapcaplet libparserutils libhubbub"
gen_pc libnsbmp       0.1.7 "libnsbmp"        "$NS/libnsbmp/include"
gen_pc libnsgif       1.0.0 "libnsgif"        "$NS/libnsgif/include"
gen_pc libnsutils     0.1.1 "libnsutils"      "$NS/libnsutils/include"
gen_pc libnspsl       0.1.7 "libnspsl"        "$NS/libnspsl/include"
gen_pc libutf8proc    2.8.0 "libutf8proc"     "$NS/libutf8proc/include/libutf8proc"

# ---------- 3. libz.a ----------
ZOBJS=""
for src in user/libc/zlib.c user/libc/inflate.c; do
    obj="$TMP/$(basename "${src%.c}").o"
    gcc $LIB_CFLAGS -c "$ROOT/$src" -o "$obj" || exit 1
    ZOBJS="$ZOBJS $obj"
done
ar rcs "$SYSROOT/lib/libz.a" $ZOBJS

# ---------- 4. libm.a ----------
cat > "$TMP/math_float_stub.c" <<'EOF'
float fabsf(float x) { return x < 0 ? -x : x; }
float floorf(float x) { double r=(double)x; long i=(long)r;
    return (float)((r<0 && (double)i!=r) ? i-1 : i); }
float ceilf(float x)  { double r=(double)x; long i=(long)r;
    return (float)((r>0 && (double)i!=r) ? i+1 : i); }
float roundf(float x) { return (float)(x>=0 ? floorf(x+0.5f) : ceilf(x-0.5f)); }
float sqrtf(float x)  { if (x<=0) return 0; double r = x>1?x:1;
    for (int i=0;i<24;i++) r = 0.5*(r+x/r); return (float)r; }
EOF

gcc $LIB_CFLAGS -c "$ROOT/user/libc/math.c"  -o "$TMP/math.o"       || exit 1
gcc $LIB_CFLAGS -c "$TMP/math_float_stub.c"   -o "$TMP/math_float.o" || exit 1
gcc $LIB_CFLAGS -c "$ROOT/user/libc/iconv.c"  -o "$TMP/iconv.o"      || exit 1
ar rcs "$SYSROOT/lib/libm.a"     "$TMP/math.o" "$TMP/math_float.o"
ar rcs "$SYSROOT/lib/libiconv.a" "$TMP/iconv.o"

# ---------- 5. libnetsurf_stubs.a ----------
cat > "$TMP/netsurf_stubs.c" <<'EOF'
void fetch_file_register(void) { }
const unsigned char filesystem_llcache_table[256] = { 0 };
EOF
gcc $LIB_CFLAGS -c "$TMP/netsurf_stubs.c" -o "$TMP/netsurf_stubs.o" || exit 1
ar rcs "$SYSROOT/lib/libnetsurf_stubs.a" "$TMP/netsurf_stubs.o"

# ---------- 6. libdom bindings 头 ----------
mkdir -p "$SYSROOT/include/dom/bindings/hubbub"
cp "$NS"/libdom/bindings/hubbub/*.h \
   "$SYSROOT/include/dom/bindings/hubbub/" 2>/dev/null || true

export PKG_CONFIG_LIBDIR="$PCDIR"

# ---------- 7. include 路径 ----------
INCLUDES="-I$LIBC_INC -I$SYSROOT/include"
for lib in libwapcaplet libutf8proc libparserutils libnsutils libnspsl \
           libnsgif libnsbmp libhubbub libcss libdom; do
    [ -d "$NS/$lib/include" ] && INCLUDES="$INCLUDES -I$NS/$lib/include"
done
[ -d "$NS/libutf8proc/include/libutf8proc" ] && \
    INCLUDES="$INCLUDES -I$NS/libutf8proc/include/libutf8proc"

cd "$NS/netsurf" || exit 1
rm -rf build/Linux-monkey

# ---------- 8. wrapper ----------
cat > "$TMP/gcc32-stub-wrapper.sh" <<WRAPPER_EOF
#!/bin/bash
has_c=0
for arg in "\$@"; do
    if [ "\$arg" = "-c" ]; then has_c=1; break; fi
done
if [ \$has_c -eq 1 ]; then
    exec gcc -m32 "\$@"
else
    exec gcc -m32 -Wl,--allow-multiple-definition "\$@" \\
         -L$SYSROOT/lib -lnetsurf_stubs
fi
WRAPPER_EOF
chmod +x "$TMP/gcc32-stub-wrapper.sh"

# ---------- 9. make config ----------
make TARGET=monkey \
     NSSHARED="$NS/buildsystem" \
     PREFIX="$SYSROOT" \
     NETSURF_USE_CURL=NO \
     NETSURF_USE_OPENSSL=NO \
     NETSURF_USE_JPEG=NO \
     NETSURF_USE_JPEGXL=NO \
     NETSURF_USE_PNG=NO \
     NETSURF_USE_WEBP=NO \
     NETSURF_USE_DUKTAPE=NO \
     NETSURF_USE_NSLOG=NO \
     NETSURF_USE_LIBICONV_PLUG=NO \
     NETSURF_USE_NSSVG=NO \
     NETSURF_USE_ROSPRITE=NO \
     NETSURF_USE_BMP=YES \
     NETSURF_USE_GIF=YES \
     NETSURF_USE_NSPSL=YES \
     NETSURF_USE_UTF8PROC=YES \
     NETSURF_FS_BACKING_STORE=NO \
     config >> "$LOG" 2>&1

gcc -m32 -ffreestanding -fno-pic -I$LIBC_INC -c "$NS/netsurf/posix_shim.c" -o "$TMP/posix_shim.o"

# ---------- 10. 正式 make（不再传 -DHAVE_*） ----------
make TARGET=monkey \
     NSSHARED="$NS/buildsystem" \
     PREFIX="$SYSROOT" \
     CC="$TMP/gcc32-stub-wrapper.sh" AR=ar ARFLG=cru \
     NETSURF_USE_CURL=NO \
     NETSURF_USE_OPENSSL=NO \
     NETSURF_USE_JPEG=NO \
     NETSURF_USE_JPEGXL=NO \
     NETSURF_USE_PNG=NO \
     NETSURF_USE_WEBP=NO \
     NETSURF_USE_DUKTAPE=NO \
     NETSURF_USE_NSLOG=NO \
     NETSURF_USE_LIBICONV_PLUG=NO \
     NETSURF_USE_NSSVG=NO \
     NETSURF_USE_ROSPRITE=NO \
     NETSURF_USE_BMP=YES \
     NETSURF_USE_GIF=YES \
     NETSURF_USE_NSPSL=YES \
     NETSURF_USE_UTF8PROC=YES \
     NETSURF_FS_BACKING_STORE=NO \
     CFLAGS="-m32 -ffreestanding -fno-pic -fno-stack-protector \
             -Dnsmonkey \
             -Wno-error=missing-field-initializers \
             -include $SYSROOT/include/netsurf_defines.h \
             -include errno.h -Wno-error=redundant-decls -Wno-error=sign-compare \
             -Wno-error=int-conversion -Wno-error=incompatible-pointer-types \
             -I$NS/netsurf/include -I$NS/netsurf \
             $INCLUDES" \
     2>&1 | tee -a "$LOG"

echo "exit: $?"
echo "log: $LOG"

# ---------- 11. 检查产物 ----------
if [ -f "$NS/netsurf/nsmonkey" ]; then
    echo "=== nsmonkey 生成成功 ==="
    file "$NS/netsurf/nsmonkey"
    ls -la "$NS/netsurf/nsmonkey"
elif [ -f "$NS/netsurf/build/Linux-monkey/nsmonkey" ]; then
    echo "=== nsmonkey 生成成功 ==="
    file "$NS/netsurf/build/Linux-monkey/nsmonkey"
    ls -la "$NS/netsurf/build/Linux-monkey/nsmonkey"
else
    echo "=== nsmonkey 未生成，日志尾部 ==="
    tail -n 30 "$LOG"
fi