// shared_memory.c

#include "shared_memory.h"
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>

int init_shared_memory(const char *shm_name, car_shared_mem **car_mem) {
    int shm_fd;
    pthread_mutexattr_t mutex_attr;
    pthread_condattr_t cond_attr;

    shm_fd = shm_open(shm_name, O_CREAT | O_EXCL | O_RDWR, 0666);
    if (shm_fd == -1) {
        perror("shm_open");
        return -1;
    }

    if (ftruncate(shm_fd, sizeof(car_shared_mem)) == -1) {
        perror("ftruncate");
        close(shm_fd);
        shm_unlink(shm_name);
        return -1;
    }

    *car_mem = mmap(NULL, sizeof(car_shared_mem), PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
    if (*car_mem == MAP_FAILED) {
        perror("mmap");
        close(shm_fd);
        shm_unlink(shm_name);
        return -1;
    }

    close(shm_fd);

    // initialise the entire shared memory region to zero
    memset(*car_mem, 0, sizeof(car_shared_mem));

    // mutex attributes
    if (pthread_mutexattr_init(&mutex_attr) != 0) {
        perror("pthread_mutexattr_init");
        munmap(*car_mem, sizeof(car_shared_mem));
        shm_unlink(shm_name);
        return -1;
    }
    if (pthread_mutexattr_setpshared(&mutex_attr, PTHREAD_PROCESS_SHARED) != 0) {
        perror("pthread_mutexattr_setpshared");
        pthread_mutexattr_destroy(&mutex_attr);
        munmap(*car_mem, sizeof(car_shared_mem));
        shm_unlink(shm_name);
        return -1;
    }

    if (pthread_mutex_init(&(*car_mem)->mutex, &mutex_attr) != 0) {
        perror("pthread_mutex_init");
        pthread_mutexattr_destroy(&mutex_attr);
        munmap(*car_mem, sizeof(car_shared_mem));
        shm_unlink(shm_name);
        return -1;
    }
    pthread_mutexattr_destroy(&mutex_attr);

    // condition variable attributes
    if (pthread_condattr_init(&cond_attr) != 0) {
        perror("pthread_condattr_init");
        pthread_mutex_destroy(&(*car_mem)->mutex);
        munmap(*car_mem, sizeof(car_shared_mem));
        shm_unlink(shm_name);
        return -1;
    }
    if (pthread_condattr_setpshared(&cond_attr, PTHREAD_PROCESS_SHARED) != 0) {
        perror("pthread_condattr_setpshared");
        pthread_condattr_destroy(&cond_attr);
        pthread_mutex_destroy(&(*car_mem)->mutex);
        munmap(*car_mem, sizeof(car_shared_mem));
        shm_unlink(shm_name);
        return -1;
    }

    // condition variable
    if (pthread_cond_init(&(*car_mem)->cond, &cond_attr) != 0) {
        perror("pthread_cond_init");
        pthread_condattr_destroy(&cond_attr);
        pthread_mutex_destroy(&(*car_mem)->mutex);
        munmap(*car_mem, sizeof(car_shared_mem));
        shm_unlink(shm_name);
        return -1;
    }
    pthread_condattr_destroy(&cond_attr);

    return 0;
}

int open_shared_memory(const char *shm_name, car_shared_mem **car_mem) {
    int shm_fd;

    shm_fd = shm_open(shm_name, O_RDWR, 0);
    if (shm_fd == -1) {
        perror("shm_open");
        return -1;
    }

    *car_mem = mmap(NULL, sizeof(car_shared_mem), PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
    if (*car_mem == MAP_FAILED) {
        perror("mmap");
        close(shm_fd);
        return -1;
    }

    close(shm_fd);
    return 0;
}

void close_shared_memory(car_shared_mem *car_mem) {
    munmap(car_mem, sizeof(car_shared_mem));
}

void unlink_shared_memory(const char *shm_name) {
    shm_unlink(shm_name);
}
