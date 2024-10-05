// safety.c

/*
 * Safety System Component
 *
 * This code is designed to adhere to safety-critical coding standards wherever possible.
 *
 * Exceptions and Deviations:
 * - Use of printf() function: We use printf() for displaying messages to the operator.
 *   While functions with variable arguments are often discouraged in safety-critical code
 *   due to potential for misuse, in this context we have controlled usage with fixed format
 *   strings, and it is necessary for user feedback.
 *
 * - Infinite loop without explicit exit condition: The main loop is designed to run indefinitely,
 *   as the safety system is expected to operate continuously. To allow for graceful shutdown,
 *   we handle SIGINT signal to exit the loop and clean up resources.
 *
 * Justification:
 * - The use of printf() is limited and controlled, with fixed format strings, minimizing
 *   risks associated with variable argument functions.
 * - The infinite loop is a requirement of the system, and proper signal handling is implemented
 *   to allow for graceful termination when needed.
 */

#include "../headers/shared_memory.h"
#include "../headers/saftey.h"
#include "utils.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <signal.h>

static volatile sig_atomic_t keep_running = 1;

static void int_handler(int dummy) {
    (void)dummy; // Suppress unused parameter warning
    keep_running = 0;
}

void run_safety_system(const char *car_name) {
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

    // Main loop
    while (keep_running) {
        if (pthread_mutex_lock(&car_mem->mutex) != 0) {
            perror("pthread_mutex_lock");
            break;
        }

        // Wait on condition variable
        while (pthread_cond_wait(&car_mem->cond, &car_mem->mutex) != 0) {
            // Retry on failure
        }

        // Check door_obstruction and status
        if (car_mem->door_obstruction == 1 &&
            strcmp(car_mem->status, "Closing") == 0) {
            // Set status to Opening
            strncpy(car_mem->status, "Opening", sizeof(car_mem->status) - 1);
            car_mem->status[sizeof(car_mem->status) - 1] = '\0';
            printf("Door obstruction detected! Opening doors.\n");
            pthread_cond_broadcast(&car_mem->cond);
        }

        // Check emergency_stop
        if (car_mem->emergency_stop == 1 && car_mem->emergency_mode == 0) {
            printf("The emergency stop button has been pressed!\n");
            car_mem->emergency_mode = 1;
            pthread_cond_broadcast(&car_mem->cond);
        }

        // Check overload
        if (car_mem->overload == 1 && car_mem->emergency_mode == 0) {
            printf("The overload sensor has been tripped!\n");
            car_mem->emergency_mode = 1;
            pthread_cond_broadcast(&car_mem->cond);
        }

        // Data consistency checks
        if (car_mem->emergency_mode != 1) {
            int data_error = 0;

            // Check current_floor and destination_floor
            if (!is_valid_floor(car_mem->current_floor) ||
                !is_valid_floor(car_mem->destination_floor)) {
                data_error = 1;
            }

            // Check status
            if (!is_valid_status(car_mem->status)) {
                data_error = 1;
            }

            // Check uint8_t fields
            if (car_mem->open_button > 1 ||
                car_mem->close_button > 1 ||
                car_mem->door_obstruction > 1 ||
                car_mem->overload > 1 ||
                car_mem->emergency_stop > 1 ||
                car_mem->individual_service_mode > 1 ||
                car_mem->emergency_mode > 1) {
                data_error = 1;
            }

            // Check door_obstruction when status is not Opening or Closing
            if (car_mem->door_obstruction == 1 &&
                strcmp(car_mem->status, "Opening") != 0 &&
                strcmp(car_mem->status, "Closing") != 0) {
                data_error = 1;
            }

            if (data_error) {
                printf("Data consistency error!\n");
                car_mem->emergency_mode = 1;
                pthread_cond_broadcast(&car_mem->cond);
            }
        }

        if (pthread_mutex_unlock(&car_mem->mutex) != 0) {
            perror("pthread_mutex_unlock");
            break;
        }
    }

    // Clean up
    close_shared_memory(car_mem);
}

int main(int argc, char *argv[]) {
    if (argc != 2) {
        fprintf(stderr, "Usage: %s {car name}\n", argv[0]);
        return EXIT_FAILURE;
    }

    setup_signal_handler(int_handler);
    run_safety_system(argv[1]);

    return EXIT_SUCCESS;
}
