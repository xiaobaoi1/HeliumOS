#ifndef USER_CURL_CURL_H
#define USER_CURL_CURL_H

/* 最小 curl stub：NetSurf 的 content/fetchers/curl.h 无条件包含此头，
 * 但 NETSURF_USE_CURL=NO 时不会编 curl fetcher 的 .c。
 * 这里只需要声明 CURLM 类型，避免编译失败。 */
typedef struct CURLM CURLM;

#endif