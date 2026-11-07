int main(int argc, char **argv) {
    (void)argc; (void)argv;
    while (1) {
        __asm__ volatile("pause");
    }
    return 0;
}