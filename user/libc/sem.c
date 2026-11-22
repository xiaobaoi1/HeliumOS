typedef int sem_t;

int sem_init(sem_t *sem, int pshared, unsigned int value) {
    (void)pshared; (void)value;
    if (sem) *sem = 1;
    return 0;
}

int sem_wait(sem_t *sem) {
    (void)sem;
    return 0;
}

int sem_post(sem_t *sem) {
    (void)sem;
    return 0;
}