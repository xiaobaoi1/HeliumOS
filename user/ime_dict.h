#ifndef USER_IME_DICT_H
#define USER_IME_DICT_H

struct ime_word { const char *py; const char *cand[5]; };

const struct ime_word *ime_lookup_pinyin(const char *py);

#endif
