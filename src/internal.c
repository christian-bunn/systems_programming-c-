// internal.c

/*
Note: My get_next_floor_up and get_next_floor_down functions used in this file have been implemented in utils.c and declared in utils.h.
      This allows for them to be shared across components.
*/

#include "../headers/shared_memory.h"
#include "../headers/utils.h"
#include "../headers/internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

void run_internal(const char *car_name, const char *operation) {
    char shm_name[256];
    car_shared_mem *car_mem;

    // Construct shared memory name
    if (snprintf(shm_name, sizeof(shm_name), "/car%s", car_name) >= (int)sizeof(shm_name)) {
        fprintf(stderr, "Car name too long.\n");
        exit(EXIT_FAILURE);
    }

    // Open shared memory
    if (open_shared_memory(shm_name, &car_mem) != 0) {
        fprintf(stderr, "Unable to access car %s.\n", car_name);
        exit(EXIT_FAILURE);
    }

    // Lock mutex
    if (pthread_mutex_lock(&car_mem->mutex) != 0) {
        perror("pthread_mutex_lock");
        close_shared_memory(car_mem);
        exit(EXIT_FAILURE);
    }

    if (strcmp(operation, "open") == 0) {
        car_mem->open_button = 1;
    } else if (strcmp(operation, "close") == 0) {
        car_mem->close_button = 1;
    } else if (strcmp(operation, "stop") == 0) {
        car_mem->emergency_stop = 1;
    } else if (strcmp(operation, "service_on") == 0) {
        car_mem->individual_service_mode = 1;
        car_mem->emergency_mode = 0;
    } else if (strcmp(operation, "service_off") == 0) {
        car_mem->individual_service_mode = 0;
    } else if (strcmp(operation, "up") == 0 || strcmp(operation, "down") == 0) {
        if (car_mem->individual_service_mode != 1) {
            printf("Operation only allowed in service mode.\n");
            pthread_mutex_unlock(&car_mem->mutex);
            close_shared_memory(car_mem);
            return;
        }
        if (strcmp(car_mem->status, "Open") == 0 || strcmp(car_mem->status, "Opening") == 0 ||
            strcmp(car_mem->status, "Closing") == 0) {
            printf("Operation not allowed while doors are open.\n");
            pthread_mutex_unlock(&car_mem->mutex);
            close_shared_memory(car_mem);
            return;
        }
        if (strcmp(car_mem->status, "Between") == 0) {
            printf("Operation not allowed while elevator is moving.\n");
            pthread_mutex_unlock(&car_mem->mutex);
            close_shared_memory(car_mem);
            return;
        }

        // Set destination floor
        char next_floor[6];
        if (strcmp(operation, "up") == 0) {
            get_next_floor_up(car_mem->current_floor, next_floor, "999"); // Assuming highest floor is 999
        } else {
            get_next_floor_down(car_mem->current_floor, next_floor, "B99"); // Assuming lowest floor is B99
        }
        strncpy(car_mem->destination_floor, next_floor, sizeof(car_mem->destination_floor) - 1);
        car_mem->destination_floor[sizeof(car_mem->destination_floor) - 1] = '\0';
    } else {
        printf("Invalid operation.\n");
        pthread_mutex_unlock(&car_mem->mutex);
        close_shared_memory(car_mem);
        return;
    }

    // Signal condition variable
    pthread_cond_broadcast(&car_mem->cond);
    pthread_mutex_unlock(&car_mem->mutex);
    close_shared_memory(car_mem);
}

int main(int argc, char *argv[]) {
    if (argc != 3) {
        fprintf(stderr, "Usage: %s {car name} {operation}\n", argv[0]);
        return EXIT_FAILURE;
    }

    run_internal(argv[1], argv[2]);
    return EXIT_SUCCESS;
}
