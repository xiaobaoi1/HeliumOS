/* 依赖 LIBMATH.SO 的 add() */
extern int add(int, int);

int add_twice(int a, int b) {
    return add(a, b) + add(a, b);
}