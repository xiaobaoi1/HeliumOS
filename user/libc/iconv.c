#include "iconv.h"
#include "string.h"
iconv_t iconv_open(const char *tocode, const char *fromcode) {
    if (!tocode || !fromcode) return (iconv_t)-1;
    if ((strcmp(tocode, "UTF-8") == 0 || strcmp(tocode, "utf-8") == 0) &&
        (strcmp(fromcode, "UTF-8") == 0 || strcmp(fromcode, "utf-8") == 0))
        return (iconv_t)1;
    return (iconv_t)-1;
}
size_t iconv(iconv_t cd, char **inbuf, size_t *inbytesleft,
             char **outbuf, size_t *outbytesleft) {
    if (cd == (iconv_t)-1 || !inbuf || !outbuf ||
        !inbytesleft || !outbytesleft) return (size_t)-1;
    size_t n = *inbytesleft < *outbytesleft ? *inbytesleft : *outbytesleft;
    memcpy(*outbuf, *inbuf, n);
    *inbuf += n; *outbuf += n;
    *inbytesleft -= n; *outbytesleft -= n;
    return 0;
}
int iconv_close(iconv_t cd) { (void)cd; return 0; }