objs=$(find user/netsurf-master/netsurf/build/Linux-monkey -name '*.o' ! -path '*/tools/*')
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