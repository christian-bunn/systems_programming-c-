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
#include <errno.h>
#include <time.h>
#include <sys/select.h>
#include <sys/time.h>
#include <unistd.h>

static volatile sig_atomic_t keep_running = 1;

static void int_handler(int dummy) {
    (void)dummy; // Suppress unused parameter warning
    keep_running = 0;
}

typedef struct {
    char name[256];
    int delay;
    car_shared_mem *car_mem;
    const char *lowest_floor;
    const char *highest_floor;
} controller_args_t;

// Thread function to handle communication with the controller
void *controller_thread(void *arg) {
    controller_args_t *args = (controller_args_t *)arg;
    int sockfd = -1;
    char message[512];
    car_shared_mem *car_mem = args->car_mem;
    int delay = args->delay;
    char *name = args->name;

    signal(SIGPIPE, SIG_IGN); // Ignore SIGPIPE to prevent crashes

    while (keep_running) {
        pthread_mutex_lock(&car_mem->mutex);

        // Check for special modes
        int in_special_mode = car_mem->individual_service_mode || car_mem->emergency_mode;
        pthread_mutex_unlock(&car_mem->mutex);

        if (in_special_mode) {
            // Close connection if connected
            if (sockfd != -1) {
                close(sockfd);
                sockfd = -1;
            }
            sleep_ms(delay);
            continue;
        }

        // Attempt to connect to controller if not connected
        if (sockfd == -1) {
            sockfd = connect_to_controller();
            if (sockfd != -1) {
                // Send CAR message
                snprintf(message, sizeof(message), "CAR %s %s %s", name, args->lowest_floor, args->highest_floor);
                if (send_message(sockfd, message) != 0) {
                    close(sockfd);
                    sockfd = -1;
                    sleep_ms(delay);
                    continue;
                }

                // Send initial STATUS message
                pthread_mutex_lock(&car_mem->mutex);
                snprintf(message, sizeof(message), "STATUS %s %s %s", car_mem->status, car_mem->current_floor, car_mem->destination_floor);
                pthread_mutex_unlock(&car_mem->mutex);
                if (send_message(sockfd, message) != 0) {
                    close(sockfd);
                    sockfd = -1;
                    sleep_ms(delay);
                    continue;
                }
            } else {
                sleep_ms(delay);
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
            sleep_ms(delay);
            continue;
        }

        // Use select() to check for incoming messages without blocking
        fd_set read_fds;
        struct timeval timeout;
        FD_ZERO(&read_fds);
        FD_SET(sockfd, &read_fds);
        timeout.tv_sec = 0;
        timeout.tv_usec = 0;

        int activity = select(sockfd + 1, &read_fds, NULL, NULL, &timeout);

        if (activity > 0 && FD_ISSET(sockfd, &read_fds)) {
            // Receive message
            char *response = NULL;
            if (receive_message(sockfd, &response) != 0) {
                close(sockfd);
                sockfd = -1;
                free(response);
                continue;
            }

            // Handle received message
            if (strncmp(response, "FLOOR ", 6) == 0) {
                pthread_mutex_lock(&car_mem->mutex);
                snprintf(car_mem->destination_floor, FLOOR_STR_SIZE, "%.*s", FLOOR_STR_SIZE - 1, response + 6);
                pthread_cond_broadcast(&car_mem->cond);
                pthread_mutex_unlock(&car_mem->mutex);
            }

            free(response);
        }

        // Wait before next check
        sleep_ms(10);
    }

    // Clean up
    if (sockfd != -1) {
        close(sockfd);
    }
    pthread_exit(NULL);
}

void run_car(const char *name, const char *lowest_floor, const char *highest_floor, int delay) {
    char shm_name[256];
    car_shared_mem *car_mem;
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
    pthread_mutex_lock(&car_mem->mutex);

    // Set initial values
    snprintf(car_mem->current_floor, FLOOR_STR_SIZE, "%.*s", FLOOR_STR_SIZE - 1, lowest_floor);
    snprintf(car_mem->destination_floor, FLOOR_STR_SIZE, "%.*s", FLOOR_STR_SIZE - 1, lowest_floor);
    snprintf(car_mem->status, STATUS_STR_SIZE, "Closed");

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
    signal(SIGINT, int_handler); // Handle SIGINT for cleanup

    // Prepare arguments for controller_thread
    controller_args_t ctrl_args;
    strncpy(ctrl_args.name, name, sizeof(ctrl_args.name) - 1);
    ctrl_args.name[sizeof(ctrl_args.name) - 1] = '\0';
    ctrl_args.delay = delay;
    ctrl_args.car_mem = car_mem;
    ctrl_args.lowest_floor = lowest_floor;
    ctrl_args.highest_floor = highest_floor;

    // Start controller communication thread
    pthread_create(&controller_tid, NULL, controller_thread, (void *)&ctrl_args);

    // Main loop
    while (keep_running) {
        pthread_mutex_lock(&car_mem->mutex);

        // Handle emergency_stop
        if (car_mem->emergency_stop) {
            car_mem->emergency_mode = 1;
            pthread_mutex_unlock(&car_mem->mutex);
            continue;
        }

        // Check for emergency mode
        if (car_mem->emergency_mode == 1) {
            // In emergency mode, doors can be opened/closed manually
            // Elevator will not move
            if (car_mem->open_button == 1 && strcmp(car_mem->status, "Closed") == 0) {
                snprintf(car_mem->status, STATUS_STR_SIZE, "Opening");
                car_mem->open_button = 0;
                pthread_cond_broadcast(&car_mem->cond);

                pthread_mutex_unlock(&car_mem->mutex);
                sleep_ms(delay);

                pthread_mutex_lock(&car_mem->mutex);
                snprintf(car_mem->status, STATUS_STR_SIZE, "Open");
                pthread_cond_broadcast(&car_mem->cond);
            } else if (car_mem->close_button == 1 && strcmp(car_mem->status, "Open") == 0) {
                snprintf(car_mem->status, STATUS_STR_SIZE, "Closing");
                car_mem->close_button = 0;
                pthread_cond_broadcast(&car_mem->cond);

                pthread_mutex_unlock(&car_mem->mutex);
                sleep_ms(delay);

                pthread_mutex_lock(&car_mem->mutex);
                snprintf(car_mem->status, STATUS_STR_SIZE, "Closed");
                pthread_cond_broadcast(&car_mem->cond);
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

            // Check if doors need to be opened or closed
            if (car_mem->open_button == 1 && strcmp(car_mem->status, "Closed") == 0) {
                snprintf(car_mem->status, STATUS_STR_SIZE, "Opening");
                car_mem->open_button = 0;
                pthread_cond_broadcast(&car_mem->cond);

                pthread_mutex_unlock(&car_mem->mutex);
                sleep_ms(delay);

                pthread_mutex_lock(&car_mem->mutex);
                snprintf(car_mem->status, STATUS_STR_SIZE, "Open");
                pthread_cond_broadcast(&car_mem->cond);
            } else if (car_mem->close_button == 1 && strcmp(car_mem->status, "Open") == 0) {
                snprintf(car_mem->status, STATUS_STR_SIZE, "Closing");
                car_mem->close_button = 0;
                pthread_cond_broadcast(&car_mem->cond);

                pthread_mutex_unlock(&car_mem->mutex);
                sleep_ms(delay);

                pthread_mutex_lock(&car_mem->mutex);
                snprintf(car_mem->status, STATUS_STR_SIZE, "Closed");
                pthread_cond_broadcast(&car_mem->cond);
            } else if (strcmp(car_mem->status, "Closed") == 0 &&
                       compare_floors(car_mem->current_floor, car_mem->destination_floor) != 0 &&
                       is_floor_in_range(car_mem->destination_floor, lowest_floor, highest_floor)) {
                // Move to destination floor
                snprintf(car_mem->status, STATUS_STR_SIZE, "Between");
                pthread_cond_broadcast(&car_mem->cond);

                pthread_mutex_unlock(&car_mem->mutex);

                while (keep_running &&
                       compare_floors(car_mem->current_floor, car_mem->destination_floor) != 0) {
                    sleep_ms(delay);

                    pthread_mutex_lock(&car_mem->mutex);

                    // Re-check status
                    if (car_mem->emergency_stop || car_mem->emergency_mode || car_mem->individual_service_mode == 0) {
                        // Stop moving
                        snprintf(car_mem->status, STATUS_STR_SIZE, "Closed");
                        pthread_cond_broadcast(&car_mem->cond);
                        pthread_mutex_unlock(&car_mem->mutex);
                        break;
                    }

                    char next_floor[FLOOR_STR_SIZE];
                    int cmp = compare_floors(car_mem->current_floor, car_mem->destination_floor);
                    if (cmp < 0) {
                        // Moving up
                        get_next_floor_up(car_mem->current_floor, next_floor, highest_floor);
                    } else {
                        // Moving down
                        get_next_floor_down(car_mem->current_floor, next_floor, lowest_floor);
                    }

                    snprintf(car_mem->current_floor, FLOOR_STR_SIZE, "%s", next_floor);
                    pthread_cond_broadcast(&car_mem->cond);

                    pthread_mutex_unlock(&car_mem->mutex);
                }

                pthread_mutex_lock(&car_mem->mutex);
                // Arrived at destination
                snprintf(car_mem->status, STATUS_STR_SIZE, "Closed");

                // Reset destination floor
                snprintf(car_mem->destination_floor, FLOOR_STR_SIZE, "%s", car_mem->current_floor);
                pthread_cond_broadcast(&car_mem->cond);
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
                // Stay open for another delay
                car_mem->open_button = 0;
                pthread_mutex_unlock(&car_mem->mutex);
                sleep_ms(delay);
                continue;
            } else if (strcmp(car_mem->status, "Closing") == 0 || strcmp(car_mem->status, "Closed") == 0) {
                // Reopen doors
                snprintf(car_mem->status, STATUS_STR_SIZE, "Opening");
                car_mem->open_button = 0;
                pthread_cond_broadcast(&car_mem->cond);

                pthread_mutex_unlock(&car_mem->mutex);
                sleep_ms(delay);

                pthread_mutex_lock(&car_mem->mutex);
                snprintf(car_mem->status, STATUS_STR_SIZE, "Open");
                pthread_cond_broadcast(&car_mem->cond);

                pthread_mutex_unlock(&car_mem->mutex);
                sleep_ms(delay);

                pthread_mutex_lock(&car_mem->mutex);
                snprintf(car_mem->status, STATUS_STR_SIZE, "Closing");
                pthread_cond_broadcast(&car_mem->cond);

                pthread_mutex_unlock(&car_mem->mutex);
                sleep_ms(delay);

                pthread_mutex_lock(&car_mem->mutex);
                snprintf(car_mem->status, STATUS_STR_SIZE, "Closed");
                pthread_cond_broadcast(&car_mem->cond);

                pthread_mutex_unlock(&car_mem->mutex);
                continue;
            }
            // Does nothing if status is "Opening" or "Between"
        }

        if (car_mem->close_button == 1 && strcmp(car_mem->status, "Open") == 0) {
            // Close doors immediately
            snprintf(car_mem->status, STATUS_STR_SIZE, "Closing");
            car_mem->close_button = 0;
            pthread_cond_broadcast(&car_mem->cond);

            pthread_mutex_unlock(&car_mem->mutex);
            sleep_ms(delay);

            pthread_mutex_lock(&car_mem->mutex);
            snprintf(car_mem->status, STATUS_STR_SIZE, "Closed");
            pthread_cond_broadcast(&car_mem->cond);

            pthread_mutex_unlock(&car_mem->mutex);
            continue;
        }

        // Handle door obstruction
        if (car_mem->door_obstruction && strcmp(car_mem->status, "Closing") == 0) {
            // Reopen doors
            snprintf(car_mem->status, STATUS_STR_SIZE, "Opening");
            pthread_cond_broadcast(&car_mem->cond);

            pthread_mutex_unlock(&car_mem->mutex);
            continue;
        }

        // Check if destination floor is different from current floor and doors are closed
        if (compare_floors(car_mem->current_floor, car_mem->destination_floor) != 0 &&
            strcmp(car_mem->status, "Closed") == 0) {

            // Handle overload
            if (car_mem->overload) {
                // Cannot move due to overload, keep doors open
                snprintf(car_mem->status, STATUS_STR_SIZE, "Open");
                pthread_cond_broadcast(&car_mem->cond);
                pthread_mutex_unlock(&car_mem->mutex);
                continue;
            }

            // Start moving
            snprintf(car_mem->status, STATUS_STR_SIZE, "Between");
            pthread_cond_broadcast(&car_mem->cond);

            pthread_mutex_unlock(&car_mem->mutex);

            while (keep_running &&
                   compare_floors(car_mem->current_floor, car_mem->destination_floor) != 0) {
                sleep_ms(delay);

                pthread_mutex_lock(&car_mem->mutex);

                // Re-check status
                if (car_mem->emergency_stop || car_mem->emergency_mode || car_mem->individual_service_mode) {
                    // Stop moving
                    snprintf(car_mem->status, STATUS_STR_SIZE, "Closed");
                    pthread_cond_broadcast(&car_mem->cond);
                    pthread_mutex_unlock(&car_mem->mutex);
                    break;
                }

                char next_floor[FLOOR_STR_SIZE];
                int cmp = compare_floors(car_mem->current_floor, car_mem->destination_floor);
                if (cmp < 0) {
                    // Moving up
                    get_next_floor_up(car_mem->current_floor, next_floor, highest_floor);
                } else {
                    // Moving down
                    get_next_floor_down(car_mem->current_floor, next_floor, lowest_floor);
                }

                snprintf(car_mem->current_floor, FLOOR_STR_SIZE, "%s", next_floor);
                pthread_cond_broadcast(&car_mem->cond);

                pthread_mutex_unlock(&car_mem->mutex);
            }

            pthread_mutex_lock(&car_mem->mutex);
            // Arrived at destination
            snprintf(car_mem->status, STATUS_STR_SIZE, "Closed");
            pthread_cond_broadcast(&car_mem->cond);

            // Open doors
            snprintf(car_mem->status, STATUS_STR_SIZE, "Opening");
            pthread_cond_broadcast(&car_mem->cond);

            pthread_mutex_unlock(&car_mem->mutex);
            sleep_ms(delay);

            pthread_mutex_lock(&car_mem->mutex);
            snprintf(car_mem->status, STATUS_STR_SIZE, "Open");
            pthread_cond_broadcast(&car_mem->cond);

            pthread_mutex_unlock(&car_mem->mutex);
            sleep_ms(delay);

            pthread_mutex_lock(&car_mem->mutex);
            snprintf(car_mem->status, STATUS_STR_SIZE, "Closing");
            pthread_cond_broadcast(&car_mem->cond);

            pthread_mutex_unlock(&car_mem->mutex);
            sleep_ms(delay);

            pthread_mutex_lock(&car_mem->mutex);
            snprintf(car_mem->status, STATUS_STR_SIZE, "Closed");
            pthread_cond_broadcast(&car_mem->cond);
            pthread_mutex_unlock(&car_mem->mutex);

        } else if (compare_floors(car_mem->current_floor, car_mem->destination_floor) == 0 &&
                   strcmp(car_mem->status, "Closed") == 0) {
            // Open doors
            snprintf(car_mem->status, STATUS_STR_SIZE, "Opening");
            pthread_cond_broadcast(&car_mem->cond);

            pthread_mutex_unlock(&car_mem->mutex);
            sleep_ms(delay);

            pthread_mutex_lock(&car_mem->mutex);
            snprintf(car_mem->status, STATUS_STR_SIZE, "Open");
            pthread_cond_broadcast(&car_mem->cond);

            pthread_mutex_unlock(&car_mem->mutex);
            sleep_ms(delay);

            pthread_mutex_lock(&car_mem->mutex);
            snprintf(car_mem->status, STATUS_STR_SIZE, "Closing");
            pthread_cond_broadcast(&car_mem->cond);

            pthread_mutex_unlock(&car_mem->mutex);
            sleep_ms(delay);

            pthread_mutex_lock(&car_mem->mutex);
            snprintf(car_mem->status, STATUS_STR_SIZE, "Closed");
            pthread_cond_broadcast(&car_mem->cond);
            pthread_mutex_unlock(&car_mem->mutex);

        } else {
            // Wait for condition variable
            pthread_cond_wait(&car_mem->cond, &car_mem->mutex);
            pthread_mutex_unlock(&car_mem->mutex);
        }

        // Sleep for a short time to prevent tight looping
        sleep_ms(10);
    }

    // Clean up
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
        fprintf(stderr, "Invalid arguments. lowest_floor: %s, highest_floor: %s, delay: %d\n",
                lowest_floor, highest_floor, delay);
        return EXIT_FAILURE;
    }

    setup_signal_handler(int_handler);
    run_car(name, lowest_floor, highest_floor, delay);

    return EXIT_SUCCESS;
}
