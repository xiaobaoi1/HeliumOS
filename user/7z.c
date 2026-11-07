#include "syscall.h"
#include "stdio.h"
#include "stdlib.h"
#include "string.h"
#include "7zip/C/7z.h"
#include "7zip/C/7zAlloc.h"
#include "7zip/C/7zBuf.h"
#include "7zip/C/7zCrc.h"
#include "7zip/C/7zFile.h"
#include "7zip/C/7zTypes.h"

#define INPUT_BUF_SIZE (1u << 18)
static const ISzAlloc g_alloc = { SzAlloc, SzFree };

static int u16_name(const UInt16 *s, char *out, int cap) {
    int n = 0;
    while (*s && n < cap - 1) {
        UInt32 c = *s++;
        if (c < 0x80) out[n++] = (char)c;
        else if (c < 0x800) {
            out[n++] = (char)(0xC0 | (c >> 6));
            if (n < cap - 1) out[n++] = (char)(0x80 | (c & 0x3F));
        } else {
            out[n++] = (char)(0xE0 | (c >> 12));
            if (n < cap - 1) out[n++] = (char)(0x80 | ((c >> 6) & 0x3F));
            if (n < cap - 1) out[n++] = (char)(0x80 | (c & 0x3F));
        }
    }
    out[n] = '\0';
    return n;
}

static void make_dirs(char *path) {
    for (char *p = path + 1; *p; p++) {
        if (*p == '/' || *p == '\\') {
            char save = *p;
            *p = '\0';
            if (path[0]) fs_mkdir(path);
            *p = save;
        }
    }
    if (path[0]) fs_mkdir(path);
}

int main(int argc, char **argv) {
    if (argc < 3) {
        printf("7z l|x|t <archive.7z>\n");
        return 1;
    }
    const char *cmd = argv[1];
    const char *archive = argv[2];

    CFileInStream archive_stream;
    if (InFile_Open(&archive_stream.file, archive) != 0) {
        printf("7z: cannot open %s\n", archive);
        return 1;
    }
    FileInStream_CreateVTable(&archive_stream);
    archive_stream.wres = 0;

    CLookToRead2 look;
    LookToRead2_CreateVTable(&look, False);
    look.buf = (Byte*)malloc(INPUT_BUF_SIZE);
    if (!look.buf) { File_Close(&archive_stream.file); return 1; }
    look.bufSize = INPUT_BUF_SIZE;
    look.realStream = &archive_stream.vt;
    LookToRead2_INIT(&look);

    CrcGenerateTable();
    CSzArEx db;
    SzArEx_Init(&db);
    SRes res = SzArEx_Open(&db, &look.vt, &g_alloc, &g_alloc);
    if (res != SZ_OK) {
        printf("7z: open error %d\n", res);
        free(look.buf);
        return 1;
    }

    int list = strcmp(cmd, "l") == 0;
    int test = strcmp(cmd, "t") == 0;
    int extract = strcmp(cmd, "x") == 0 || strcmp(cmd, "e") == 0;
    if (!list && !test && !extract) {
        printf("7z: command must be l, x, e or t\n");
        SzArEx_Free(&db, &g_alloc);
        free(look.buf);
        return 1;
    }

    UInt32 block_index = 0xFFFFFFFF;
    Byte *out_buf = NULL;
    size_t out_size = 0;
    size_t total = 0;
    for (UInt32 i = 0; i < db.NumFiles; i++) {
        int is_dir = SzArEx_IsDir(&db, i);
        size_t len = SzArEx_GetFileNameUtf16(&db, i, NULL);
        if (len == 0 || len > 4096) continue;
        UInt16 *name16 = (UInt16*)malloc(len * sizeof(UInt16));
        if (!name16) continue;
        SzArEx_GetFileNameUtf16(&db, i, name16);
        char name[4096];
        u16_name(name16, name, sizeof(name));
        free(name16);
        for (char *p = name; *p; p++) if (*p == '\\') *p = '/';

        if (list) {
            printf("%s %9u  %s\n", is_dir ? "D" : " ", (unsigned)SzArEx_GetFileSize(&db, i), name);
            total++;
        } else if (test) {
            if (!is_dir) {
                size_t off = 0, out_len = 0;
                res = SzArEx_Extract(&db, &look.vt, i, &block_index,
                    &out_buf, &out_size, &off, &out_len, &g_alloc, &g_alloc);
                if (res != SZ_OK) break;
            }
            printf("T %s\n", name);
            total++;
        } else {
            if (is_dir) {
                make_dirs(name);
            } else {
                size_t off = 0, out_len = 0;
                res = SzArEx_Extract(&db, &look.vt, i, &block_index,
                    &out_buf, &out_size, &off, &out_len, &g_alloc, &g_alloc);
                if (res != SZ_OK) break;
                char tmp[4096];
                strncpy(tmp, name, sizeof(tmp) - 1);
                tmp[sizeof(tmp) - 1] = '\0';
                make_dirs(tmp);
                FILE *f = fopen(name, "wb");
                if (!f) { res = SZ_ERROR_FAIL; break; }
                if (fwrite(out_buf + off, out_len, 1, f) != 1 && out_len != 0) { fclose(f); res = SZ_ERROR_FAIL; break; }
                fclose(f);
            }
            printf("x %s\n", name);
            total++;
        }
    }
    if (out_buf) free(out_buf);
    SzArEx_Free(&db, &g_alloc);
    free(look.buf);
    File_Close(&archive_stream.file);
    if (res != SZ_OK) {
        printf("7z: error %d\n", res);
        return 1;
    }
    printf("7z: %u files\n", (unsigned)total);
    return 0;
}
