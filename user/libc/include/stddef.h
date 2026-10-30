#ifndef USER_STDDEF_H
#define USER_STDDEF_H

#ifndef _SIZE_T_DEFINED
#define _SIZE_T_DEFINED
typedef unsigned int size_t;
#endif

#ifndef _PTRDIFF_T_DEFINED
#define _PTRDIFF_T_DEFINED
typedef int ptrdiff_t;
#endif

#ifndef _WCHAR_T_DEFINED
#define _WCHAR_T_DEFINED
typedef int wchar_t;
#endif

#ifndef NULL
#define NULL ((void*)0)
#endif

#ifdef __TINYC__
#define offsetof(type, member) ((size_t)&((type*)0)->member)
#else
#define offsetof(type, member) __builtin_offsetof(type, member)
#endif

#endif