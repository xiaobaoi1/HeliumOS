#ifndef USER_SEMAPHORE_H
#define USER_SEMAPHORE_H

/* HeliumOS 提供进程内最小 semaphore；TCC 的 libtcc 多线程锁使用。 */
typedef int sem_t;

int sem_init(sem_t *sem, int pshared, unsigned int value);
int sem_wait(sem_t *sem);
int sem_post(sem_t *sem);

#endif
