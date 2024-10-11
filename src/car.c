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
#include <errno.h>

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

    time_t last_status_time = 0;

    while (keep_running) {
        pthread_mutex_lock(&car_mem->mutex);

        // Check for special modes
        int in_special_mode = car_mem->individual_service_mode || car_mem->emergency_mode;
        pthread_mutex_unlock(&car_mem->mutex);

        if (in_special_mode) {
            // Send appropriate message before disconnecting
            if (sockfd != -1) {
                pthread_mutex_lock(&car_mem->mutex);
                if (car_mem->individual_service_mode) {
                    send_message(sockfd, "INDIVIDUAL SERVICE");
                } else if (car_mem->emergency_mode) {
                    send_message(sockfd, "EMERGENCY");
                }
                pthread_mutex_unlock(&car_mem->mutex);
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
                pthread_mutex_lock(&car_mem->mutex);
                snprintf(message, sizeof(message), "CAR %s %s %s", name, args->lowest_floor, args->highest_floor);
                pthread_mutex_unlock(&car_mem->mutex);
                send_message(sockfd, message);

                // Send initial STATUS message
                pthread_mutex_lock(&car_mem->mutex);
                snprintf(message, sizeof(message), "STATUS %s %s %s", car_mem->status, car_mem->current_floor, car_mem->destination_floor);
                pthread_mutex_unlock(&car_mem->mutex);
                send_message(sockfd, message);

                last_status_time = time(NULL);
            } else {
                sleep_ms(delay);
                continue;
            }
        }

        // Send STATUS message if status changed or delay passed
        time_t current_time = time(NULL);

        pthread_mutex_lock(&car_mem->mutex);
        int status_changed = car_mem->status_changed;
        car_mem->status_changed = 0;
        pthread_mutex_unlock(&car_mem->mutex);

        if (status_changed || (current_time - last_status_time) * 1000 >= delay) {
            pthread_mutex_lock(&car_mem->mutex);
            snprintf(message, sizeof(message), "STATUS %s %s %s", car_mem->status, car_mem->current_floor, car_mem->destination_floor);
            pthread_mutex_unlock(&car_mem->mutex);

            if (send_message(sockfd, message) != 0) {
                close(sockfd);
                sockfd = -1;
                continue;
            }

            last_status_time = current_time;
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
                strncpy(car_mem->destination_floor, response + 6, FLOOR_STR_SIZE - 1);
                car_mem->destination_floor[FLOOR_STR_SIZE - 1] = '\0';
                car_mem->status_changed = 1;
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
    strncpy(car_mem->current_floor, lowest_floor, FLOOR_STR_SIZE - 1);
    car_mem->current_floor[FLOOR_STR_SIZE - 1] = '\0';

    strncpy(car_mem->destination_floor, lowest_floor, FLOOR_STR_SIZE - 1);
    car_mem->destination_floor[FLOOR_STR_SIZE - 1] = '\0';

    strncpy(car_mem->status, "Closed", STATUS_STR_SIZE - 1);
    car_mem->status[STATUS_STR_SIZE - 1] = '\0';

    car_mem->open_button = 0;
    car_mem->close_button = 0;
    car_mem->door_obstruction = 0;
    car_mem->overload = 0;
    car_mem->emergency_stop = 0;
    car_mem->individual_service_mode = 0;
    car_mem->emergency_mode = 0;
    car_mem->status_changed = 1; // Indicate initial status has changed

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
            car_mem->status_changed = 1;
            pthread_mutex_unlock(&car_mem->mutex);
            continue;
        }

        // Check for emergency mode
        if (car_mem->emergency_mode == 1) {
            // In emergency mode, doors can be opened/closed manually
            // Elevator will not move
            if (car_mem->open_button == 1 && strcmp(car_mem->status, "Closed") == 0) {
                strncpy(car_mem->status, "Opening", STATUS_STR_SIZE - 1);
                car_mem->status[STATUS_STR_SIZE - 1] = '\0';
                car_mem->open_button = 0;
                car_mem->status_changed = 1;
                pthread_cond_broadcast(&car_mem->cond);

                pthread_mutex_unlock(&car_mem->mutex);
                sleep_ms(delay);

                pthread_mutex_lock(&car_mem->mutex);
                strncpy(car_mem->status, "Open", STATUS_STR_SIZE - 1);
                car_mem->status[STATUS_STR_SIZE - 1] = '\0';
                car_mem->status_changed = 1;
                pthread_cond_broadcast(&car_mem->cond);
            } else if (car_mem->close_button == 1 && strcmp(car_mem->status, "Open") == 0) {
                strncpy(car_mem->status, "Closing", STATUS_STR_SIZE - 1);
                car_mem->status[STATUS_STR_SIZE - 1] = '\0';
                car_mem->close_button = 0;
                car_mem->status_changed = 1;
                pthread_cond_broadcast(&car_mem->cond);

                pthread_mutex_unlock(&car_mem->mutex);
                sleep_ms(delay);

                pthread_mutex_lock(&car_mem->mutex);
                strncpy(car_mem->status, "Closed", STATUS_STR_SIZE - 1);
                car_mem->status[STATUS_STR_SIZE - 1] = '\0';
                car_mem->status_changed = 1;
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
                strncpy(car_mem->status, "Opening", STATUS_STR_SIZE - 1);
                car_mem->status[STATUS_STR_SIZE - 1] = '\0';
                car_mem->open_button = 0;
                car_mem->status_changed = 1;
                pthread_cond_broadcast(&car_mem->cond);

                pthread_mutex_unlock(&car_mem->mutex);
                sleep_ms(delay);

                pthread_mutex_lock(&car_mem->mutex);
                strncpy(car_mem->status, "Open", STATUS_STR_SIZE - 1);
                car_mem->status[STATUS_STR_SIZE - 1] = '\0';
                car_mem->status_changed = 1;
                pthread_cond_broadcast(&car_mem->cond);
            } else if (car_mem->close_button == 1 && strcmp(car_mem->status, "Open") == 0) {
                strncpy(car_mem->status, "Closing", STATUS_STR_SIZE - 1);
                car_mem->status[STATUS_STR_SIZE - 1] = '\0';
                car_mem->close_button = 0;
                car_mem->status_changed = 1;
                pthread_cond_broadcast(&car_mem->cond);

                pthread_mutex_unlock(&car_mem->mutex);
                sleep_ms(delay);

                pthread_mutex_lock(&car_mem->mutex);
                strncpy(car_mem->status, "Closed", STATUS_STR_SIZE - 1);
                car_mem->status[STATUS_STR_SIZE - 1] = '\0';
                car_mem->status_changed = 1;
                pthread_cond_broadcast(&car_mem->cond);
            } else if (strcmp(car_mem->status, "Closed") == 0 &&
                       compare_floors(car_mem->current_floor, car_mem->destination_floor) != 0 &&
                       is_floor_in_range(car_mem->destination_floor, lowest_floor, highest_floor)) {
                // Move to destination floor
                strncpy(car_mem->status, "Between", STATUS_STR_SIZE - 1);
                car_mem->status[STATUS_STR_SIZE - 1] = '\0';
                car_mem->status_changed = 1;
                pthread_cond_broadcast(&car_mem->cond);

                pthread_mutex_unlock(&car_mem->mutex);

                while (keep_running &&
                       compare_floors(car_mem->current_floor, car_mem->destination_floor) != 0) {
                    sleep_ms(delay);

                    pthread_mutex_lock(&car_mem->mutex);

                    // Re-check status
                    if (car_mem->emergency_stop || car_mem->emergency_mode || car_mem->individual_service_mode == 0) {
                        // Stop moving
                        strncpy(car_mem->status, "Closed", STATUS_STR_SIZE - 1);
                        car_mem->status[STATUS_STR_SIZE - 1] = '\0';
                        car_mem->status_changed = 1;
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

                    strncpy(car_mem->current_floor, next_floor, sizeof(car_mem->current_floor) - 1);
                    car_mem->current_floor[sizeof(car_mem->current_floor) - 1] = '\0';
                    car_mem->status_changed = 1;
                    pthread_cond_broadcast(&car_mem->cond);

                    pthread_mutex_unlock(&car_mem->mutex);
                }

                pthread_mutex_lock(&car_mem->mutex);
                // Arrived at destination
                strncpy(car_mem->status, "Closed", STATUS_STR_SIZE - 1);
                car_mem->status[STATUS_STR_SIZE - 1] = '\0';

                // Reset destination floor
                strncpy(car_mem->destination_floor, car_mem->current_floor, sizeof(car_mem->destination_floor) - 1);
                car_mem->destination_floor[sizeof(car_mem->destination_floor) - 1] = '\0';
                car_mem->status_changed = 1;
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
                car_mem->status_changed = 1;
                pthread_mutex_unlock(&car_mem->mutex);
                sleep_ms(delay);
                continue;
            } else if (strcmp(car_mem->status, "Closing") == 0 || strcmp(car_mem->status, "Closed") == 0) {
                // Reopen doors
                strncpy(car_mem->status, "Opening", STATUS_STR_SIZE - 1);
                car_mem->status[STATUS_STR_SIZE - 1] = '\0';
                car_mem->open_button = 0;
                car_mem->status_changed = 1;
                pthread_cond_broadcast(&car_mem->cond);

                pthread_mutex_unlock(&car_mem->mutex);
                sleep_ms(delay);

                pthread_mutex_lock(&car_mem->mutex);
                strncpy(car_mem->status, "Open", STATUS_STR_SIZE - 1);
                car_mem->status[STATUS_STR_SIZE - 1] = '\0';
                car_mem->status_changed = 1;
                pthread_cond_broadcast(&car_mem->cond);

                pthread_mutex_unlock(&car_mem->mutex);
                sleep_ms(delay);

                pthread_mutex_lock(&car_mem->mutex);
                strncpy(car_mem->status, "Closing", STATUS_STR_SIZE - 1);
                car_mem->status[STATUS_STR_SIZE - 1] = '\0';
                car_mem->status_changed = 1;
                pthread_cond_broadcast(&car_mem->cond);

                pthread_mutex_unlock(&car_mem->mutex);
                sleep_ms(delay);

                pthread_mutex_lock(&car_mem->mutex);
                strncpy(car_mem->status, "Closed", STATUS_STR_SIZE - 1);
                car_mem->status[STATUS_STR_SIZE - 1] = '\0';
                car_mem->status_changed = 1;
                pthread_cond_broadcast(&car_mem->cond);

                pthread_mutex_unlock(&car_mem->mutex);
                continue;
            }
            // Does nothing if status is "Opening" or "Between"
        }

        if (car_mem->close_button == 1 && strcmp(car_mem->status, "Open") == 0) {
            // Close doors immediately
            strncpy(car_mem->status, "Closing", STATUS_STR_SIZE - 1);
            car_mem->status[STATUS_STR_SIZE - 1] = '\0';
            car_mem->close_button = 0;
            car_mem->status_changed = 1;
            pthread_cond_broadcast(&car_mem->cond);

            pthread_mutex_unlock(&car_mem->mutex);
            sleep_ms(delay);

            pthread_mutex_lock(&car_mem->mutex);
            strncpy(car_mem->status, "Closed", STATUS_STR_SIZE - 1);
            car_mem->status[STATUS_STR_SIZE - 1] = '\0';
            car_mem->status_changed = 1;
            pthread_cond_broadcast(&car_mem->cond);

            pthread_mutex_unlock(&car_mem->mutex);
            continue;
        }

        // Handle door obstruction
        if (car_mem->door_obstruction && strcmp(car_mem->status, "Closing") == 0) {
            // Reopen doors
            strncpy(car_mem->status, "Opening", STATUS_STR_SIZE - 1);
            car_mem->status[STATUS_STR_SIZE - 1] = '\0';
            car_mem->status_changed = 1;
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
                strncpy(car_mem->status, "Open", STATUS_STR_SIZE - 1);
                car_mem->status_changed = 1;
                pthread_cond_broadcast(&car_mem->cond);
                pthread_mutex_unlock(&car_mem->mutex);
                continue;
            }

            // Start moving
            strncpy(car_mem->status, "Between", STATUS_STR_SIZE - 1);
            car_mem->status[STATUS_STR_SIZE - 1] = '\0';
            car_mem->status_changed = 1;
            pthread_cond_broadcast(&car_mem->cond);

            pthread_mutex_unlock(&car_mem->mutex);

            while (keep_running &&
                   compare_floors(car_mem->current_floor, car_mem->destination_floor) != 0) {
                sleep_ms(delay);

                pthread_mutex_lock(&car_mem->mutex);

                // Re-check status
                if (car_mem->emergency_stop || car_mem->emergency_mode || car_mem->individual_service_mode) {
                    // Stop moving
                    strncpy(car_mem->status, "Closed", STATUS_STR_SIZE - 1);
                    car_mem->status_changed = 1;
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

                strncpy(car_mem->current_floor, next_floor, sizeof(car_mem->current_floor) - 1);
                car_mem->current_floor[sizeof(car_mem->current_floor) - 1] = '\0';
                car_mem->status_changed = 1;
                pthread_cond_broadcast(&car_mem->cond);

                pthread_mutex_unlock(&car_mem->mutex);
            }

            pthread_mutex_lock(&car_mem->mutex);
            // Arrived at destination
            strncpy(car_mem->status, "Closed", STATUS_STR_SIZE - 1);
            car_mem->status_changed = 1;
            pthread_cond_broadcast(&car_mem->cond);

            // Open doors
            strncpy(car_mem->status, "Opening", STATUS_STR_SIZE - 1);
            car_mem->status_changed = 1;
            pthread_cond_broadcast(&car_mem->cond);

            pthread_mutex_unlock(&car_mem->mutex);
            sleep_ms(delay);

            pthread_mutex_lock(&car_mem->mutex);
            strncpy(car_mem->status, "Open", STATUS_STR_SIZE - 1);
            car_mem->status_changed = 1;
            pthread_cond_broadcast(&car_mem->cond);

            pthread_mutex_unlock(&car_mem->mutex);
            sleep_ms(delay);

            pthread_mutex_lock(&car_mem->mutex);
            strncpy(car_mem->status, "Closing", STATUS_STR_SIZE - 1);
            car_mem->status_changed = 1;
            pthread_cond_broadcast(&car_mem->cond);

            pthread_mutex_unlock(&car_mem->mutex);
            sleep_ms(delay);

            pthread_mutex_lock(&car_mem->mutex);
            strncpy(car_mem->status, "Closed", STATUS_STR_SIZE - 1);
            car_mem->status_changed = 1;
            pthread_cond_broadcast(&car_mem->cond);
            pthread_mutex_unlock(&car_mem->mutex);

        } else if (compare_floors(car_mem->current_floor, car_mem->destination_floor) == 0 &&
                   strcmp(car_mem->status, "Closed") == 0) {
            // Open doors
            strncpy(car_mem->status, "Opening", STATUS_STR_SIZE - 1);
            car_mem->status_changed = 1;
            pthread_cond_broadcast(&car_mem->cond);

            pthread_mutex_unlock(&car_mem->mutex);
            sleep_ms(delay);

            pthread_mutex_lock(&car_mem->mutex);
            strncpy(car_mem->status, "Open", STATUS_STR_SIZE - 1);
            car_mem->status_changed = 1;
            pthread_cond_broadcast(&car_mem->cond);

            pthread_mutex_unlock(&car_mem->mutex);
            sleep_ms(delay);

            pthread_mutex_lock(&car_mem->mutex);
            strncpy(car_mem->status, "Closing", STATUS_STR_SIZE - 1);
            car_mem->status_changed = 1;
            pthread_cond_broadcast(&car_mem->cond);

            pthread_mutex_unlock(&car_mem->mutex);
            sleep_ms(delay);

            pthread_mutex_lock(&car_mem->mutex);
            strncpy(car_mem->status, "Closed", STATUS_STR_SIZE - 1);
            car_mem->status_changed = 1;
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
        fprintf(stderr, "Invalid arguments.\n");
        return EXIT_FAILURE;
    }

    setup_signal_handler(int_handler);
    run_car(name, lowest_floor, highest_floor, delay);

    return EXIT_SUCCESS;
}