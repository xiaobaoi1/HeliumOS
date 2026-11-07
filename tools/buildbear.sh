#!/bin/bash
cd /mnt/d/Local/HeliumOS-Beta
rm -rf /tmp/bearobj; mkdir /tmp/bearobj
ok=0; fail=0
while IFS= read -r f; do
  o=/tmp/bearobj/$(echo "$f" | sed 's#/#_#g').o
  if gcc -m32 -ffreestanding -fno-pic -fno-stack-protector \
    -Iuser/BearSSL-master/inc -Iuser/BearSSL-master/src \
    -Iuser/libc/include -Iuser/posix/include \
    -DBR_USE_URANDOM=0 -DBR_USE_WIN32_RAND=0 -DBR_USE_UNIX_TIME=0 -DBR_USE_WIN32_TIME=0 \
    -c "$f" -o "$o" 2>/tmp/berr; then
    ok=$((ok+1))
  else
    fail=$((fail+1)); echo "FAIL $f"; head -3 /tmp/berr
  fi
done < <(find user/BearSSL-master/src -name '*.c' | sort)
echo OK=$ok FAIL=$fail
