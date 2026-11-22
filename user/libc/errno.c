int __helio_errno = 0;
int *__errno_location(void) {
    return &__helio_errno;
}