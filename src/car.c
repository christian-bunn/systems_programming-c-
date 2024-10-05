// car.c

#include "shared_memory.h"
#include "network.h"
#include "utils.h"
#include "car.h"
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

// Thread function to handle communication with the controller
void *controller_thread(void *arg) {
    char *name = (char *)arg;
    int sockfd = -1;
    char message[256];
    car_shared_mem *car_mem;
    char shm_name[256];

    // Construct shared memory name
    snprintf(shm_name, sizeof(shm_name), "/car%s", name);

    // Open shared memory
    if (open_shared_memory(shm_name, &car_mem) != 0) {
        fprintf(stderr, "Unable to access shared memory for car %s.\n", name);
        pthread_exit(NULL);
    }

    while (keep_running) {
        // Attempt to connect to controller if not connected
        if (sockfd == -1) {
            sockfd = connect_to_controller();
            if (sockfd != -1) {
                // Send CAR message
                snprintf(message, sizeof(message), "CAR %s %s %s", name, car_mem->current_floor, car_mem->destination_floor);
                send_message(sockfd, message);
            } else {
                // Wait before retrying
                sleep_ms(1000); // Wait 1 second before retrying
                continue;
            }
        }

        // Send STATUS message
        pthread_mutex_lock(&car_mem->mutex);
        snprintf(message, sizeof(message), "STATUS %s %s %s", car_mem->status, car_mem->current_floor, car_mem->destination_floor);
        pthread_mutex_unlock(&car_mem->mutex);
        if (send_message(sockfd, message) != 0) {
            close(sockfd);
            sockfd = -1;
            continue;
        }

        // Receive messages from controller
        char *response = NULL;
        if (receive_message(sockfd, &response) != 0) {
            close(sockfd);
            sockfd = -1;
            free(response);
            continue;
        }

        if (strncmp(response, "FLOOR ", 6) == 0) {
            // Update destination floor
            pthread_mutex_lock(&car_mem->mutex);
            strncpy(car_mem->destination_floor, response + 6, sizeof(car_mem->destination_floor) - 1);
            car_mem->destination_floor[sizeof(car_mem->destination_floor) - 1] = '\0';
            pthread_cond_broadcast(&car_mem->cond);
            pthread_mutex_unlock(&car_mem->mutex);
        } else if (strcmp(response, "EMERGENCY") == 0 || strcmp(response, "INDIVIDUAL SERVICE") == 0) {
            // Handle special cases if needed
        }

        free(response);

        // Wait before next status update
        sleep_ms(1000); // Adjust the delay as needed
    }

    // Clean up
    if (sockfd != -1) {
        close(sockfd);
    }
    close_shared_memory(car_mem);
    pthread_exit(NULL);
}

void run_car(const char *name, const char *lowest_floor, const char *highest_floor, int delay) {
    char shm_name[256];
    car_shared_mem *car_mem;
    int sockfd = -1;
    char message[256];
    int delay_ms = delay;
    int connected_to_controller = 0;
    pthread_t controller_tid;

    // Construct shared memory name
    if (snprintf(shm_name, sizeof(shm_name), "/car%s", name) >= (int)sizeof(shm_name)) {
        fprintf(stderr, "Car name too long.\n");
        exit(EXIT_FAILURE);
    }

    // Initialize shared memory
    if (init_shared_memory(shm_name, &car_mem) != 0) {
        fprintf(stderr, "Failed to create shared memory for car %s.\n", name);
        exit(EXIT_FAILURE);
    }

    // Initialize shared memory values
    if (pthread_mutex_lock(&car_mem->mutex) != 0) {
        perror("pthread_mutex_lock");
        exit(EXIT_FAILURE);
    }

    strncpy(car_mem->current_floor, lowest_floor, sizeof(car_mem->current_floor) - 1);
    car_mem->current_floor[sizeof(car_mem->current_floor) - 1] = '\0';

    strncpy(car_mem->destination_floor, lowest_floor, sizeof(car_mem->destination_floor) - 1);
    car_mem->destination_floor[sizeof(car_mem->destination_floor) - 1] = '\0';

    strncpy(car_mem->status, "Closed", sizeof(car_mem->status) - 1);
    car_mem->status[sizeof(car_mem->status) - 1] = '\0';

    car_mem->open_button = 0;
    car_mem->close_button = 0;
    car_mem->door_obstruction = 0;
    car_mem->overload = 0;
    car_mem->emergency_stop = 0;
    car_mem->individual_service_mode = 0;
    car_mem->emergency_mode = 0;

    pthread_mutex_unlock(&car_mem->mutex);

    // Set up signal handler
    signal(SIGPIPE, SIG_IGN);

    // Start controller communication thread
    pthread_create(&controller_tid, NULL, controller_thread, (void *)name);

    // Main loop
    while (keep_running) {
        pthread_mutex_lock(&car_mem->mutex);

        // Check for emergency mode
        if (car_mem->emergency_mode == 1) {
            // In emergency mode, doors can be opened/closed manually
            // Elevator will not move
            if (car_mem->open_button == 1 && strcmp(car_mem->status, "Closed") == 0) {
                strncpy(car_mem->status, "Opening", sizeof(car_mem->status));
                car_mem->status[STATUS_STR_SIZE - 1] = '\0';
                pthread_cond_broadcast(&car_mem->cond);
                pthread_mutex_unlock(&car_mem->mutex);
                sleep_ms(delay_ms);
                pthread_mutex_lock(&car_mem->mutex);
                strncpy(car_mem->status, "Open", sizeof(car_mem->status) - 1);
                car_mem->status[sizeof(car_mem->status) - 1] = '\0';
                car_mem->open_button = 0;
            } else if (car_mem->close_button == 1 && strcmp(car_mem->status, "Open") == 0) {
                strncpy(car_mem->status, "Closing", sizeof(car_mem->status) - 1);
                car_mem->status[sizeof(car_mem->status) - 1] = '\0';
                pthread_cond_broadcast(&car_mem->cond);
                pthread_mutex_unlock(&car_mem->mutex);
                sleep_ms(delay_ms);
                pthread_mutex_lock(&car_mem->mutex);
                strncpy(car_mem->status, "Closed", sizeof(car_mem->status) - 1);
                car_mem->status[sizeof(car_mem->status) - 1] = '\0';
                car_mem->close_button = 0;
            } else {
                // Wait for condition variable
                pthread_cond_wait(&car_mem->cond, &car_mem->mutex);
            }
            pthread_mutex_unlock(&car_mem->mutex);
            continue;
        }

        // Check for individual service mode
        if (car_mem->individual_service_mode == 1) {
            // Handle individual service mode operations
            // Elevator can be moved manually by setting destination_floor
            // Doors do not open/close automatically
            if (car_mem->emergency_mode == 1) {
                car_mem->emergency_mode = 0;
            }

            // Check if doors need to be opened or closed
            if (car_mem->open_button == 1 && strcmp(car_mem->status, "Closed") == 0) {
                strncpy(car_mem->status, "Opening", sizeof(car_mem->status));
                car_mem->status[STATUS_STR_SIZE - 1] = '\0';
                pthread_cond_broadcast(&car_mem->cond);
                pthread_mutex_unlock(&car_mem->mutex);
                sleep_ms(delay_ms);
                pthread_mutex_lock(&car_mem->mutex);
                strncpy(car_mem->status, "Open", sizeof(car_mem->status) - 1);
                car_mem->status[sizeof(car_mem->status) - 1] = '\0';
                car_mem->open_button = 0;
            } else if (car_mem->close_button == 1 && strcmp(car_mem->status, "Open") == 0) {
                strncpy(car_mem->status, "Closing", sizeof(car_mem->status) - 1);
                car_mem->status[sizeof(car_mem->status) - 1] = '\0';
                pthread_cond_broadcast(&car_mem->cond);
                pthread_mutex_unlock(&car_mem->mutex);
                sleep_ms(delay_ms);
                pthread_mutex_lock(&car_mem->mutex);
                strncpy(car_mem->status, "Closed", sizeof(car_mem->status) - 1);
                car_mem->status[sizeof(car_mem->status) - 1] = '\0';
                car_mem->close_button = 0;
            } else if (strcmp(car_mem->status, "Closed") == 0 && compare_floors(car_mem->current_floor, car_mem->destination_floor) != 0) {
                // Move to destination floor
                strncpy(car_mem->status, "Between", sizeof(car_mem->status) - 1);
                car_mem->status[sizeof(car_mem->status) - 1] = '\0';
                pthread_cond_broadcast(&car_mem->cond);
                pthread_mutex_unlock(&car_mem->mutex);
                sleep_ms(delay_ms);
                pthread_mutex_lock(&car_mem->mutex);

                // Update current floor
                strncpy(car_mem->current_floor, car_mem->destination_floor, sizeof(car_mem->current_floor) - 1);
                car_mem->current_floor[sizeof(car_mem->current_floor) - 1] = '\0';

                strncpy(car_mem->status, "Closed", sizeof(car_mem->status) - 1);
                car_mem->status[sizeof(car_mem->status) - 1] = '\0';

                // Reset destination floor
                strncpy(car_mem->destination_floor, car_mem->current_floor, sizeof(car_mem->destination_floor) - 1);
                car_mem->destination_floor[sizeof(car_mem->destination_floor) - 1] = '\0';
            } else {
                // Wait for condition variable
                pthread_cond_wait(&car_mem->cond, &car_mem->mutex);
            }
            pthread_mutex_unlock(&car_mem->mutex);
            continue;
        }

        // Normal operation
        // Handle open and close buttons
        if (car_mem->open_button == 1) {
            if (strcmp(car_mem->status, "Open") == 0) {
                // Stay open for another delay_ms
                car_mem->open_button = 0;
            } else if (strcmp(car_mem->status, "Closing") == 0 || strcmp(car_mem->status, "Closed") == 0) {
                // Reopen doors
                strncpy(car_mem->status, "Opening", sizeof(car_mem->status));
                car_mem->status[STATUS_STR_SIZE - 1] = '\0';
                car_mem->open_button = 0;
                pthread_cond_broadcast(&car_mem->cond);
                pthread_mutex_unlock(&car_mem->mutex);
                sleep_ms(delay_ms);
                pthread_mutex_lock(&car_mem->mutex);
                strncpy(car_mem->status, "Open", sizeof(car_mem->status) - 1);
                car_mem->status[sizeof(car_mem->status) - 1] = '\0';
            }
        } else if (car_mem->close_button == 1 && strcmp(car_mem->status, "Open") == 0) {
            // Close doors immediately
            strncpy(car_mem->status, "Closing", sizeof(car_mem->status) - 1);
            car_mem->status[sizeof(car_mem->status) - 1] = '\0';
            car_mem->close_button = 0;
            pthread_cond_broadcast(&car_mem->cond);
            pthread_mutex_unlock(&car_mem->mutex);
            sleep_ms(delay_ms);
            pthread_mutex_lock(&car_mem->mutex);
            strncpy(car_mem->status, "Closed", sizeof(car_mem->status) - 1);
            car_mem->status[sizeof(car_mem->status) - 1] = '\0';
        }

        // Check if destination floor is different from current floor and doors are closed
        if (compare_floors(car_mem->current_floor, car_mem->destination_floor) != 0 && strcmp(car_mem->status, "Closed") == 0) {
            // Start moving
            strncpy(car_mem->status, "Between", sizeof(car_mem->status) - 1);
            car_mem->status[sizeof(car_mem->status) - 1] = '\0';
            pthread_cond_broadcast(&car_mem->cond);
            pthread_mutex_unlock(&car_mem->mutex);
            sleep_ms(delay_ms);
            pthread_mutex_lock(&car_mem->mutex);

            // Move one floor towards destination
            int cmp = compare_floors(car_mem->current_floor, car_mem->destination_floor);
            char next_floor[6];
            if (cmp < 0) {
                // Moving up
                // Increment floor
                get_next_floor_up(car_mem->current_floor, next_floor, highest_floor);
            } else {
                // Moving down
                // Decrement floor
                get_next_floor_down(car_mem->current_floor, next_floor, lowest_floor);
            }
            strncpy(car_mem->current_floor, next_floor, sizeof(car_mem->current_floor) - 1);
            car_mem->current_floor[sizeof(car_mem->current_floor) - 1] = '\0';

            strncpy(car_mem->status, "Closed", sizeof(car_mem->status) - 1);
            car_mem->status[sizeof(car_mem->status) - 1] = '\0';

            // Check if arrived at destination
            if (compare_floors(car_mem->current_floor, car_mem->destination_floor) == 0) {
                // Open doors
                strncpy(car_mem->status, "Opening", sizeof(car_mem->status));
                car_mem->status[STATUS_STR_SIZE - 1] = '\0';
                pthread_cond_broadcast(&car_mem->cond);
                pthread_mutex_unlock(&car_mem->mutex);
                sleep_ms(delay_ms);
                pthread_mutex_lock(&car_mem->mutex);
                strncpy(car_mem->status, "Open", sizeof(car_mem->status) - 1);
                car_mem->status[sizeof(car_mem->status) - 1] = '\0';

                // Wait delay_ms
                pthread_mutex_unlock(&car_mem->mutex);
                sleep_ms(delay_ms);
                pthread_mutex_lock(&car_mem->mutex);

                // Close doors
                strncpy(car_mem->status, "Closing", sizeof(car_mem->status) - 1);
                car_mem->status[sizeof(car_mem->status) - 1] = '\0';
                pthread_cond_broadcast(&car_mem->cond);
                pthread_mutex_unlock(&car_mem->mutex);
                sleep_ms(delay_ms);
                pthread_mutex_lock(&car_mem->mutex);
                strncpy(car_mem->status, "Closed", sizeof(car_mem->status) - 1);
                car_mem->status[sizeof(car_mem->status) - 1] = '\0';
            }
        } else {
            // Wait for condition variable
            pthread_cond_wait(&car_mem->cond, &car_mem->mutex);
        }

        pthread_mutex_unlock(&car_mem->mutex);

        // Send STATUS message to controller if connected
        if (connected_to_controller) {
            snprintf(message, sizeof(message), "STATUS %s %s %s", car_mem->status, car_mem->current_floor, car_mem->destination_floor);
            send_message(sockfd, message);
        }

        // Sleep for delay_ms
        sleep_ms(delay_ms);
    }

    // Clean up
    if (connected_to_controller) {
        close(sockfd);
    }
    pthread_join(controller_tid, NULL);
    unlink_shared_memory(shm_name);
    close_shared_memory(car_mem);
}


int main(int argc, char *argv[]) {
    if (argc != 5) {
        fprintf(stderr, "Usage: %s {name} {lowest floor} {highest floor} {delay}\n", argv[0]);
        return EXIT_FAILURE;
    }

    const char *name = argv[1];
    const char *lowest_floor = argv[2];
    const char *highest_floor = argv[3];
    int delay = atoi(argv[4]);

    if (!is_valid_floor(lowest_floor) || !is_valid_floor(highest_floor) || delay <= 0) {
        fprintf(stderr, "Invalid arguments.\n");
        return EXIT_FAILURE;
    }

    setup_signal_handler(int_handler);
    run_car(name, lowest_floor, highest_floor, delay);

    return EXIT_SUCCESS;
}