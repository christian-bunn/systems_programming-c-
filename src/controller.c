// controller.c

#include "../headers/network.h"
#include "../headers/utils.h"
#include "../headers/controller.h"
#include "shared_memory.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <signal.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <errno.h>
#include <arpa/inet.h>
#include <limits.h>

static volatile sig_atomic_t keep_running = 1;

static void int_handler(int dummy) {
    (void)dummy;
    keep_running = 0;
}

typedef enum { UP = 1, DOWN = -1, IDLE = 0 } Direction;

typedef struct floor_request {
    char floor[FLOOR_STR_SIZE];
    Direction direction;
    struct floor_request *next;
} floor_request;

typedef struct {
    int sockfd;
    char name[32];
    char lowest_floor[FLOOR_STR_SIZE];
    char highest_floor[FLOOR_STR_SIZE];
    char status[16];
    char current_floor[FLOOR_STR_SIZE];
    char destination_floor[FLOOR_STR_SIZE];
    Direction direction; // 1 for UP, -1 for DOWN, 0 for IDLE
    floor_request *queue_head;
    pthread_mutex_t queue_mutex;
} car_info;

typedef struct {
    char source_floor[FLOOR_STR_SIZE];
    char dest_floor[FLOOR_STR_SIZE];
    Direction direction;
} call_request;

typedef struct {
    int sockfd;
    char *message;
} client_arg_t;

// Data structures to keep track of cars
#define MAX_CARS 10
static car_info cars[MAX_CARS];
static int num_cars = 0;

// Mutex to protect shared data
pthread_mutex_t data_mutex = PTHREAD_MUTEX_INITIALIZER;

// Function to handle car disconnection and cleanup
void remove_car_from_service(car_info *car) {
    pthread_mutex_lock(&data_mutex);
    // Clean up queue
    pthread_mutex_lock(&car->queue_mutex);
    floor_request *req = car->queue_head;
    while (req) {
        floor_request *tmp = req;
        req = req->next;
        free(tmp);
    }
    pthread_mutex_unlock(&car->queue_mutex);
    pthread_mutex_destroy(&car->queue_mutex);

    // Remove car from array
    for (int i = 0; i < num_cars; ++i) {
        if (&cars[i] == car) {
            cars[i] = cars[--num_cars];
            break;
        }
    }
    pthread_mutex_unlock(&data_mutex);
    close(car->sockfd);
}

// Function to handle car connections
void *handle_car(void *arg) {
    client_arg_t *car_arg = (client_arg_t *)arg;
    int sockfd = car_arg->sockfd;
    char *message = car_arg->message;
    free(arg); // Free the struct but not the message
    car_info *car = NULL;

    if (strncmp(message, "CAR ", 4) == 0) {
        // Parse CAR message
        char car_name[32], low_floor[FLOOR_STR_SIZE], high_floor[FLOOR_STR_SIZE];
        if (sscanf(message + 4, "%31s %11s %11s", car_name, low_floor, high_floor) != 3) {
            free(message);
            close(sockfd);
            return NULL;
        }

        free(message); // Now we can free the message

        // Add car to cars array
        pthread_mutex_lock(&data_mutex);
        if (num_cars >= MAX_CARS) {
            pthread_mutex_unlock(&data_mutex);
            close(sockfd);
            return NULL;
        }
        car = &cars[num_cars++];
        car->sockfd = sockfd;
        strncpy(car->name, car_name, sizeof(car->name) - 1);
        car->name[sizeof(car->name) - 1] = '\0';
        strncpy(car->lowest_floor, low_floor, sizeof(car->lowest_floor) - 1);
        car->lowest_floor[sizeof(car->lowest_floor) - 1] = '\0';
        strncpy(car->highest_floor, high_floor, sizeof(car->highest_floor) - 1);
        car->highest_floor[sizeof(car->highest_floor) - 1] = '\0';
        car->queue_head = NULL;
        pthread_mutex_init(&car->queue_mutex, NULL);

        // Initialize car status
        strncpy(car->status, "Closed", sizeof(car->status) - 1);
        car->status[sizeof(car->status) - 1] = '\0';
        strncpy(car->current_floor, low_floor, sizeof(car->current_floor) - 1);
        car->current_floor[sizeof(car->current_floor) - 1] = '\0';
        strncpy(car->destination_floor, low_floor, sizeof(car->destination_floor) - 1);
        car->destination_floor[sizeof(car->destination_floor) - 1] = '\0';
        car->direction = IDLE;

        pthread_mutex_unlock(&data_mutex);

        // Continue to receive messages from car
        while (keep_running) {
            if (receive_message(sockfd, &message) != 0) {
                break;
            }
            if (strncmp(message, "STATUS ", 7) == 0) {
                // Update car status
                pthread_mutex_lock(&car->queue_mutex);
                char prev_current_floor[FLOOR_STR_SIZE];
                strncpy(prev_current_floor, car->current_floor, FLOOR_STR_SIZE);

                sscanf(message + 7, "%15s %11s %11s", car->status, car->current_floor, car->destination_floor);

                // Determine car's direction
                int cmp = compare_floors(car->destination_floor, car->current_floor);
                if (cmp > 0) {
                    car->direction = UP;
                } else if (cmp < 0) {
                    car->direction = DOWN;
                } else {
                    car->direction = IDLE;
                }

                // Check if the car has arrived at the destination floor
                if ((strcmp(car->status, "Opening") == 0 || strcmp(car->status, "Open") == 0) &&
                    car->queue_head && strcmp(car->queue_head->floor, car->current_floor) == 0) {
                    // Remove the floor from the queue
                    floor_request *old_head = car->queue_head;
                    car->queue_head = car->queue_head->next;
                    free(old_head);

                    // Send next FLOOR message if queue is not empty
                    if (car->queue_head) {
                        char floor_msg[20];
                        snprintf(floor_msg, sizeof(floor_msg), "FLOOR %s", car->queue_head->floor);
                        send_message(car->sockfd, floor_msg);
                        // Update car's destination_floor
                        strncpy(car->destination_floor, car->queue_head->floor, sizeof(car->destination_floor) - 1);
                        car->destination_floor[sizeof(car->destination_floor) - 1] = '\0';
                    } else {
                        // No more requests, set direction to IDLE
                        car->direction = IDLE;
                    }
                }

                pthread_mutex_unlock(&car->queue_mutex);
            } else if (strcmp(message, "INDIVIDUAL SERVICE") == 0 || strcmp(message, "EMERGENCY") == 0) {
                // Remove car from service
                remove_car_from_service(car);
                free(message);
                return NULL;
            }
            free(message);
        }
    } else {
        free(message);
    }

    // Handle unexpected disconnection
    remove_car_from_service(car);
    return NULL;
}

// Function to select the best car for a call request
car_info *select_best_car(call_request *call) {
    car_info *best_car = NULL;
    int min_distance = INT_MAX;

    pthread_mutex_lock(&data_mutex);
    for (int i = 0; i < num_cars; ++i) {
        car_info *car = &cars[i];

        // Check if car can service both source and destination floors
        if (is_floor_in_range(call->source_floor, car->lowest_floor, car->highest_floor) &&
            is_floor_in_range(call->dest_floor, car->lowest_floor, car->highest_floor)) {

            // Calculate distance from car's current floor to source floor
            int distance = abs(compare_floors(car->current_floor, call->source_floor));

            // Choose the car with the minimum distance to the source floor
            if (distance < min_distance) {
                min_distance = distance;
                best_car = car;
            }
        }
    }
    pthread_mutex_unlock(&data_mutex);

    return best_car;
}

// Function to insert floors into car's queue
void insert_into_queue(car_info *car, call_request *call) {
    pthread_mutex_lock(&car->queue_mutex);

    Direction direction = car->direction;
    // If the elevator is idle, set its direction to the call's direction
    if (direction == IDLE) {
        direction = compare_floors(car->current_floor, call->source_floor) < 0 ? UP : DOWN;
        car->direction = direction;
    }

    // Create floor requests for source and destination
    floor_request *from_request = malloc(sizeof(floor_request));
    strncpy(from_request->floor, call->source_floor, sizeof(from_request->floor) - 1);
    from_request->floor[sizeof(from_request->floor) - 1] = '\0';
    from_request->direction = direction;
    from_request->next = NULL;

    floor_request *to_request = malloc(sizeof(floor_request));
    strncpy(to_request->floor, call->dest_floor, sizeof(to_request->floor) - 1);
    to_request->floor[sizeof(to_request->floor) - 1] = '\0';
    to_request->direction = compare_floors(call->source_floor, call->dest_floor) < 0 ? UP : DOWN;
    to_request->next = NULL;

    // Insert 'from_request' into the queue
    floor_request **current = &car->queue_head;
    floor_request *prev = NULL;
    int inserted = 0;

    while (*current) {
        floor_request *req = *current;
        int cmp = compare_floors(from_request->floor, req->floor);
        if ((direction == UP && cmp < 0) || (direction == DOWN && cmp > 0)) {
            // Insert here
            from_request->next = req;
            if (prev) {
                prev->next = from_request;
            } else {
                car->queue_head = from_request;
            }
            inserted = 1;
            break;
        }
        prev = req;
        current = &req->next;
    }

    if (!inserted) {
        // Insert at the end
        if (prev) {
            prev->next = from_request;
        } else {
            car->queue_head = from_request;
        }
    }

    // Now insert 'to_request' after 'from_request' or at the end
    current = &from_request->next;
    prev = from_request;
    inserted = 0;

    while (*current) {
        floor_request *req = *current;
        int cmp = compare_floors(to_request->floor, req->floor);
        if ((to_request->direction == UP && cmp < 0) || (to_request->direction == DOWN && cmp > 0)) {
            // Insert here
            to_request->next = req;
            prev->next = to_request;
            inserted = 1;
            break;
        }
        prev = req;
        current = &req->next;
    }

    if (!inserted) {
        // Insert at the end
        prev->next = to_request;
    }

    // If the car is idle, or if the new 'from_request' is at the head of the queue, send FLOOR message
    if (car->queue_head == from_request) {
        char floor_msg[20];
        snprintf(floor_msg, sizeof(floor_msg), "FLOOR %s", car->queue_head->floor);
        send_message(car->sockfd, floor_msg);
        // Update car's destination_floor
        strncpy(car->destination_floor, car->queue_head->floor, sizeof(car->destination_floor) - 1);
        car->destination_floor[sizeof(car->destination_floor) - 1] = '\0';
    }

    pthread_mutex_unlock(&car->queue_mutex);
}

// Function to handle call pad connections
void *handle_call(void *arg) {
    client_arg_t *call_arg = (client_arg_t *)arg;
    int sockfd = call_arg->sockfd;
    char *message = call_arg->message;
    free(arg); // Free the struct but not the message

    if (strncmp(message, "CALL ", 5) == 0) {
        // Parse CALL message
        char source_floor[FLOOR_STR_SIZE], dest_floor[FLOOR_STR_SIZE];
        if (sscanf(message + 5, "%11s %11s", source_floor, dest_floor) != 2) {
            send_message(sockfd, "UNAVAILABLE");
            free(message);
            close(sockfd);
            return NULL;
        }

        if (!is_valid_floor(source_floor) || !is_valid_floor(dest_floor)) {
            send_message(sockfd, "UNAVAILABLE");
            free(message);
            close(sockfd);
            return NULL;
        }

        // Determine direction
        Direction dir = (compare_floors(source_floor, dest_floor) < 0) ? UP : DOWN;

        // Create call request
        call_request call;
        strncpy(call.source_floor, source_floor, sizeof(call.source_floor) - 1);
        call.source_floor[sizeof(call.source_floor) - 1] = '\0';
        strncpy(call.dest_floor, dest_floor, sizeof(call.dest_floor) - 1);
        call.dest_floor[sizeof(call.dest_floor) - 1] = '\0';
        call.direction = dir;

        // Find the best car to service the request
        car_info *selected_car = select_best_car(&call);

        if (selected_car) {
            // Insert floors into car's queue
            insert_into_queue(selected_car, &call);

            // Send CAR response to call pad
            char response[64];
            snprintf(response, sizeof(response), "CAR %s", selected_car->name);
            send_message(sockfd, response);
        } else {
            // No available car
            send_message(sockfd, "UNAVAILABLE");
        }
    } else {
        send_message(sockfd, "UNAVAILABLE");
    }

    free(message);
    close(sockfd);
    return NULL;
}

void run_controller() {
    int listen_sock;
    struct sockaddr_in serv_addr;
    int opt_enable = 1;

    // Create listening socket
    listen_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_sock == -1) {
        perror("socket");
        exit(EXIT_FAILURE);
    }

    // Enable address reuse
    setsockopt(listen_sock, SOL_SOCKET, SO_REUSEADDR, &opt_enable, sizeof(opt_enable));

    // Bind socket
    memset(&serv_addr, 0, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_addr.s_addr = inet_addr(CONTROLLER_IP);
    serv_addr.sin_port = htons(CONTROLLER_PORT);

    if (bind(listen_sock, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) != 0) {
        perror("bind");
        close(listen_sock);
        exit(EXIT_FAILURE);
    }

    // Listen
    if (listen(listen_sock, 5) != 0) {
        perror("listen");
        close(listen_sock);
        exit(EXIT_FAILURE);
    }

    // Accept connections
    while (keep_running) {
        int *new_sock = malloc(sizeof(int));
        *new_sock = accept(listen_sock, NULL, NULL);
        if (*new_sock == -1) {
            if (errno == EINTR) {
                free(new_sock);
                break;
            }
            perror("accept");
            free(new_sock);
            continue;
        }

        // Read the first message to determine client type
        char *message = NULL;
        if (receive_message(*new_sock, &message) != 0) {
            close(*new_sock);
            free(new_sock);
            continue;
        }

        if (strncmp(message, "CAR ", 4) == 0) {
            // Car connection
            client_arg_t *car_arg = malloc(sizeof(client_arg_t));
            car_arg->sockfd = *new_sock;
            car_arg->message = message; // Pass the message
            pthread_t car_thread;
            pthread_create(&car_thread, NULL, handle_car, car_arg);
            pthread_detach(car_thread);
            free(new_sock); // The sockfd is now managed by the thread
        } else if (strncmp(message, "CALL ", 5) == 0) {
            // Call pad connection
            client_arg_t *call_arg = malloc(sizeof(client_arg_t));
            call_arg->sockfd = *new_sock;
            call_arg->message = message; // Pass the message
            pthread_t call_thread;
            pthread_create(&call_thread, NULL, handle_call, call_arg);
            pthread_detach(call_thread);
            free(new_sock); // The sockfd is now managed by the thread
        } else {
            // Unknown connection
            close(*new_sock);
            free(new_sock);
            free(message);
        }
    }

    // Close listening socket
    close(listen_sock);
}

int main(int argc, char *argv[]) {
    (void)argc;  // Suppress unused parameter warning
    (void)argv;  // Suppress unused parameter warning
    setup_signal_handler(int_handler);
    signal(SIGPIPE, SIG_IGN); // Ignore SIGPIPE to prevent crashes on write failures
    run_controller();
    return EXIT_SUCCESS;
}