#ifndef USER_INFLATE_H
#define USER_INFLATE_H

#include <stdint.h>

/* 原始 DEFLATE 流解压。成功返回 0，*out_len 写入字节数。
 * 失败返回 -1。 */
int inflate_raw(const uint8_t *in, int in_len,
                uint8_t *out, int out_max, int *out_len);

/* gzip 包装（1f 8b 08 ...）。返回解压字节数，失败 -1。 */
int gzip_decompress(const uint8_t *in, int in_len,
                    uint8_t *out, int out_max);

/* zlib 包装（78 9c ...）。返回解压字节数，失败 -1。 */
int zlib_decompress(const uint8_t *in, int in_len,
                    uint8_t *out, int out_max);

#endif