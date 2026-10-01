#include <stddef.h>
#include <string.h>
#include <stdlib.h>

char **environ = NULL;

static char **_heap_env = NULL;
static int    _heap_cap = 0;

/* crt0 调用：把 envp 拷到堆上 */
void __libc_init_environ(char **envp) {
    int n = 0;
    if (envp) {
        while (envp[n]) n++;
    }
    int cap = n + 16;
    _heap_env = (char**)malloc(cap * sizeof(char*));
    if (!_heap_env) {
        environ = envp;
        return;
    }
    for (int i = 0; i < n; i++) {
        int len = strlen(envp[i]) + 1;
        char *copy = (char*)malloc(len);
        if (copy) {
            memcpy(copy, envp[i], len);
            _heap_env[i] = copy;
        } else {
            _heap_env[i] = NULL;
        }
    }
    _heap_env[n] = NULL;
    _heap_cap = cap;
    environ = _heap_env;
}

/* 内部：找 name 对应项的下标；未找到返回 -1 */
static int find_env(const char *name) {
    if (!name || !environ) return -1;
    int nlen = strlen(name);
    for (int i = 0; environ[i]; i++) {
        const char *e = environ[i];
        int j = 0;
        while (j < nlen && e[j] && e[j] == name[j]) j++;
        if (j == nlen && e[j] == '=') return i;
    }
    return -1;
}

char *getenv(const char *name) {
    int i = find_env(name);
    if (i < 0) return NULL;
    int nlen = strlen(name);
    return environ[i] + nlen + 1;
}

int setenv(const char *name, const char *value, int overwrite) {
    if (!name || !value || !*name) return -1;
    if (strchr(name, '=')) return -1;
    if (environ != _heap_env) return -1;

    int nlen = strlen(name);
    int vlen = strlen(value);
    int total = nlen + 1 + vlen + 1;

    int i = find_env(name);
    if (i >= 0) {
        if (!overwrite) return 0;
        char *new_e = (char*)malloc(total);
        if (!new_e) return -1;
        memcpy(new_e, name, nlen);
        new_e[nlen] = '=';
        memcpy(new_e + nlen + 1, value, vlen);
        new_e[total - 1] = '\0';
        free(environ[i]);
        environ[i] = new_e;
        return 0;
    }

    /* 追加 */
    int n = 0;
    while (environ[n]) n++;
    if (n + 1 >= _heap_cap) {
        int new_cap = _heap_cap * 2;
        char **new_env = (char**)malloc(new_cap * sizeof(char*));
        if (!new_env) return -1;
        for (int k = 0; k < n; k++) new_env[k] = environ[k];
        free(environ);
        _heap_env = new_env;
        environ = new_env;
        _heap_cap = new_cap;
    }

    char *new_e = (char*)malloc(total);
    if (!new_e) return -1;
    memcpy(new_e, name, nlen);
    new_e[nlen] = '=';
    memcpy(new_e + nlen + 1, value, vlen);
    new_e[total - 1] = '\0';
    environ[n] = new_e;
    environ[n + 1] = NULL;
    return 0;
}

int unsetenv(const char *name) {
    if (!name || !*name) return -1;
    if (strchr(name, '=')) return -1;
    if (environ != _heap_env) return -1;

    int i = find_env(name);
    if (i < 0) return 0;
    free(environ[i]);
    for (int k = i; environ[k]; k++) {
        environ[k] = environ[k + 1];
    }
    return 0;
}