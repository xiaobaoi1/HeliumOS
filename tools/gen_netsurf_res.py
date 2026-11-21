#!/usr/bin/env python3
import os

RES_DIR = "third_party/netsurf/resources"
OUT_FILE = "third_party/netsurf/builtin_resources.c"

# 我们需要打包进内存的文件列表（可以根据需要扩充）
FILES = [
    "default.css",
    "quirks.css",
    "internal.css"
]

def main():
    with open(OUT_FILE, "w") as f:
        f.write('#include <stddef.h>\n#include <stdint.h>\n\n')
        f.write('struct builtin_resource {\n')
        f.write('    const char *name;\n')
        f.write('    const uint8_t *data;\n')
        f.write('    size_t len;\n')
        f.write('};\n\n')

        entries = []
        for fname in FILES:
            path = os.path.join(RES_DIR, fname)
            if not os.path.exists(path):
                print(f"Warning: {path} not found, skipping.")
                continue
            
            with open(path, "rb") as rf:
                data = rf.read()
            
            # C 语言数组名，例如 default_css_data
            var_name = fname.replace('.', '_').replace('-', '_')
            
            f.write(f'const uint8_t {var_name}_data[] = {{\n    ')
            for i, b in enumerate(data):
                f.write(f'0x{b:02x}, ')
                if (i + 1) % 16 == 0:
                    f.write('\n    ')
            f.write('\n};\n')
            f.write(f'const size_t {var_name}_len = {len(data)};\n\n')
            
            entries.append((fname, var_name))

        # 生成查找表
        f.write('const struct builtin_resource builtin_resources[] = {\n')
        for fname, var_name in entries:
            f.write(f'    {{ "{fname}", {var_name}_data, {var_name}_len }},\n')
        f.write('    { NULL, NULL, 0 }\n};\n')
        
    print(f"Generated {OUT_FILE}")

if __name__ == '__main__':
    main()