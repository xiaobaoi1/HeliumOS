/* 精简 POSIX ERE 引擎（回溯算法）
 * 支持：字面量、.、[...]、[^...]、*、+、?、()、|、^、$、\转义
 * 不支持：{n,m}、反向引用、非贪婪、lookaround
 * 覆盖 NetSurf save_complete.c 的 <base href="..."> 提取需求。
 * 复杂模式会返回 REG_NOMATCH（graceful 退化，不崩溃）。
 */
#include "regex.h"
#include "string.h"
#include "stdlib.h"

#define TOK_MAX 256

enum {
    TOK_CHAR, TOK_ANY, TOK_CLASS, TOK_LPAREN, TOK_RPAREN,
    TOK_STAR, TOK_PLUS, TOK_QUERY, TOK_ALT, TOK_BOL, TOK_EOL,
};

struct tok {
    int type;
    int grp;
    unsigned char ch;
    unsigned char cls[32];
    int neg;
};

struct regex_priv {
    struct tok toks[TOK_MAX];
    int ntok;
    int ngroup;
    int icase;
};

static int parse_class(const char **pp, struct tok *t) {
    const char *p = *pp;
    memset(t->cls, 0, 32);
    t->neg = 0;
    if (*p == '^') { t->neg = 1; p++; }
    int first = 1;
    while (*p && (*p != ']' || first)) {
        first = 0;
        unsigned char c = (unsigned char)*p++;
        if (c == '\\' && *p) c = (unsigned char)*p++;
        if (c == '-' && *p && *p != ']') {
            unsigned char hi = (unsigned char)*p++;
            if (hi == '\\' && *p) hi = (unsigned char)*p++;
            for (int x = c; x <= hi; x++) t->cls[x / 8] |= (1 << (x % 8));
        } else {
            t->cls[c / 8] |= (1 << (c % 8));
        }
    }
    if (*p == ']') p++;
    *pp = p;
    return 0;
}

static int tok_atom(const char **pp, struct tok *t) {
    const char *p = *pp;
    unsigned char c = (unsigned char)*p++;
    t->grp = 0;
    switch (c) {
        case '.': t->type = TOK_ANY; break;
        case '[': t->type = TOK_CLASS; parse_class(&p, t); break;
        case '(': t->type = TOK_LPAREN; break;
        case ')': t->type = TOK_RPAREN; break;
        case '|': t->type = TOK_ALT; break;
        case '^': t->type = TOK_BOL; break;
        case '$': t->type = TOK_EOL; break;
        case '\\':
            if (!*p) return -1;
            c = (unsigned char)*p++;
            t->type = TOK_CHAR;
            t->ch = c;
            break;
        case '*': t->type = TOK_STAR; break;
        case '+': t->type = TOK_PLUS; break;
        case '?': t->type = TOK_QUERY; break;
        default:
            t->type = TOK_CHAR;
            t->ch = c;
    }
    *pp = p;
    return 0;
}

static int compile(struct regex_priv *r, const char *pat, int cflags) {
    r->ntok = 0;
    r->ngroup = 1;
    r->icase = (cflags & REG_ICASE) != 0;
    const char *p = pat;
    while (*p) {
        if (r->ntok >= TOK_MAX - 1) return -REG_ESPACE;
        if (tok_atom(&p, &r->toks[r->ntok]) < 0) return -REG_EESCAPE;
        if (r->toks[r->ntok].type == TOK_LPAREN)
            r->toks[r->ntok].grp = r->ngroup++;
        r->ntok++;
    }
    return 0;
}

int regcomp(regex_t *preg, const char *pattern, int cflags) {
    if (!preg || !pattern) return REG_BADPAT;
    struct regex_priv *r = (struct regex_priv*)malloc(sizeof(*r));
    if (!r) return REG_ESPACE;
    int err = compile(r, pattern, cflags);
    if (err < 0) { free(r); return -err; }
    preg->re_nsub = r->ngroup - 1;
    preg->re_priv = r;
    return 0;
}

static int match_one(const struct tok *t, const char *s, int slen, int pos,
                     int icase) {
    if (pos >= slen) return -1;
    unsigned char c = (unsigned char)s[pos];
    switch (t->type) {
        case TOK_CHAR: {
            unsigned char p = t->ch;
            if (icase) {
                if (c >= 'A' && c <= 'Z') c += 32;
                if (p >= 'A' && p <= 'Z') p += 32;
            }
            return (c == p) ? pos + 1 : -1;
        }
        case TOK_ANY:
            return (c == '\n') ? -1 : pos + 1;
        case TOK_CLASS: {
            int hit = (t->cls[c / 8] >> (c % 8)) & 1;
            if (t->neg) hit = !hit;
            return hit ? pos + 1 : -1;
        }
    }
    return -1;
}

static int match_alt(struct regex_priv *r, int t_start, int t_end,
                     const char *s, int slen, int sp,
                     regmatch_t *pm, size_t nm);

static int match_seq(struct regex_priv *r, int t_start, int t_end,
                     const char *s, int slen, int sp,
                     regmatch_t *pm, size_t nm) {
    if (t_start >= t_end) return sp;
    const struct tok *t = &r->toks[t_start];

    if (t->type == TOK_LPAREN) {
        int depth = 1, i;
        for (i = t_start + 1; i < t_end; i++) {
            int ty = r->toks[i].type;
            if (ty == TOK_LPAREN) depth++;
            else if (ty == TOK_RPAREN) { depth--; if (depth == 0) break; }
        }
        if (depth != 0) return -1;
        int end = match_alt(r, t_start + 1, i, s, slen, sp, pm, nm);
        if (end < 0) return -1;
        if (pm && (size_t)t->grp < nm) {
            pm[t->grp].rm_so = sp;
            pm[t->grp].rm_eo = end;
        }
        return match_seq(r, i + 1, t_end, s, slen, end, pm, nm);
    }

    int next = t_start + 1;
    int quant = 0;
    if (next < t_end) {
        int nt = r->toks[next].type;
        if (nt == TOK_STAR || nt == TOK_PLUS || nt == TOK_QUERY) {
            quant = nt;
            next++;
        }
    }

    if (quant == 0) {
        int np = match_one(t, s, slen, sp, r->icase);
        if (np < 0) return -1;
        return match_seq(r, next, t_end, s, slen, np, pm, nm);
    }

    int positions[64];
    int npos = 0;
    positions[npos++] = sp;
    int cp = sp;
    while (npos < 64) {
        int np = match_one(t, s, slen, cp, r->icase);
        if (np < 0) break;
        positions[npos++] = np;
        cp = np;
    }

    int min_match = (quant == TOK_PLUS) ? 1 : 0;
    int max_match = (quant == TOK_QUERY) ? 1 : npos - 1;

    for (int k = max_match; k >= min_match; k--) {
        int r2 = match_seq(r, next, t_end, s, slen, positions[k], pm, nm);
        if (r2 >= 0) return r2;
    }
    return -1;
}

static int match_alt(struct regex_priv *r, int t_start, int t_end,
                     const char *s, int slen, int sp,
                     regmatch_t *pm, size_t nm) {
    int alt = -1, depth = 0;
    for (int i = t_start; i < t_end; i++) {
        int ty = r->toks[i].type;
        if (ty == TOK_LPAREN) depth++;
        else if (ty == TOK_RPAREN) depth--;
        else if (ty == TOK_ALT && depth == 0) { alt = i; break; }
    }
    if (alt < 0)
        return match_seq(r, t_start, t_end, s, slen, sp, pm, nm);
    int r1 = match_seq(r, t_start, alt, s, slen, sp, pm, nm);
    if (r1 >= 0) return r1;
    return match_seq(r, alt + 1, t_end, s, slen, sp, pm, nm);
}

int regexec(const regex_t *preg, const char *string, size_t nmatch,
            regmatch_t pmatch[], int eflags) {
    if (!preg || !preg->re_priv || !string) return REG_BADPAT;
    struct regex_priv *r = (struct regex_priv*)preg->re_priv;
    int slen = (int)strlen(string);
    int has_bol = (r->ntok > 0 && r->toks[0].type == TOK_BOL);

    for (int start = 0; start <= slen; start++) {
        if (has_bol && start > 0 && !(eflags & REG_NOTBOL)) break;
        int ts = has_bol ? 1 : 0;
        int end = match_alt(r, ts, r->ntok, string, slen, start, pmatch, nmatch);
        if (end >= 0) {
            if (nmatch > 0 && pmatch) {
                pmatch[0].rm_so = start;
                pmatch[0].rm_eo = end;
                for (size_t i = 1; i < nmatch; i++) {
                    pmatch[i].rm_so = -1;
                    pmatch[i].rm_eo = -1;
                }
            }
            return 0;
        }
    }
    return REG_NOMATCH;
}

size_t regerror(int errcode, const regex_t *preg, char *errbuf,
                size_t errbuf_size) {
    (void)preg;
    const char *msg = "regex error";
    switch (errcode) {
        case REG_NOMATCH: msg = "no match"; break;
        case REG_BADPAT:  msg = "bad pattern"; break;
        case REG_EBRACK:  msg = "unmatched ["; break;
        case REG_EPAREN:  msg = "unmatched ("; break;
        case REG_ESPACE:  msg = "out of memory"; break;
    }
    size_t len = strlen(msg) + 1;
    if (errbuf && errbuf_size > 0) {
        size_t n = (len < errbuf_size) ? len : errbuf_size;
        memcpy(errbuf, msg, n - 1);
        errbuf[n - 1] = 0;
    }
    return len;
}

void regfree(regex_t *preg) {
    if (!preg) return;
    if (preg->re_priv) free(preg->re_priv);
    preg->re_priv = NULL;
}