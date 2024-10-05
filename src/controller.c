// controller.c

#include "../headers/network.h"
#include "../headers/utils.h"
#include "../headers/controller.h"
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

static volatile sig_atomic_t keep_running = 1;

static void int_handler(int dummy) {
    (void)dummy;
    keep_running = 0;
}

typedef enum { UP, DOWN } Direction;

typedef struct floor_request {
    char floor[4];
    Direction direction; // UP or DOWN
    struct floor_request *next;
} floor_request;

typedef struct {
    int sockfd;
    char name[32];
    char lowest_floor[4];
    char highest_floor[4];
    char status[8];
    char current_floor[4];
    char destination_floor[4];
    floor_request *queue_head;
    pthread_mutex_t queue_mutex;
    // Additional fields as needed
} car_info;

typedef struct {
    char source_floor[4];
    char dest_floor[4];
    Direction direction; // Direction passenger wants to go
} call_request;

// Data structures to keep track of cars
#define MAX_CARS 10
static car_info cars[MAX_CARS];
static int num_cars = 0;

// Mutex to protect shared data
pthread_mutex_t data_mutex = PTHREAD_MUTEX_INITIALIZER;

// Function to compare floors
int compare_floors(const char *floor1, const char *floor2) {
    int f1, f2;
    int is_b1 = (floor1[0] == 'B');
    int is_b2 = (floor2[0] == 'B');
    f1 = atoi(is_b1 ? &floor1[1] : floor1);
    f2 = atoi(is_b2 ? &floor2[1] : floor2);
    if (is_b1) f1 = -f1;
    if (is_b2) f2 = -f2;

    if (f1 < f2) return -1;
    if (f1 > f2) return 1;
    return 0;
}

// Function to handle car connections
void *handle_car(void *arg) {
    int sockfd = *(int *)arg;
    free(arg);
    char *message = NULL;
    car_info *car = NULL;

    // Receive CAR message
    if (receive_message(sockfd, &message) != 0) {
        close(sockfd);
        return NULL;
    }

    if (strncmp(message, "CAR ", 4) == 0) {
        // Parse CAR message
        char car_name[32], low_floor[4], high_floor[4];
        if (sscanf(message + 4, "%31s %3s %3s", car_name, low_floor, high_floor) != 3) {
            free(message);
            close(sockfd);
            return NULL;
        }

        // Add car to cars array
        pthread_mutex_lock(&data_mutex);
        if (num_cars >= MAX_CARS) {
            pthread_mutex_unlock(&data_mutex);
            free(message);
            close(sockfd);
            return NULL;
        }
        car = &cars[num_cars++];
        car->sockfd = sockfd;
        strncpy(car->name, car_name, sizeof(car->name));
        strncpy(car->lowest_floor, low_floor, sizeof(car->lowest_floor));
        strncpy(car->highest_floor, high_floor, sizeof(car->highest_floor));
        car->queue_head = NULL;
        pthread_mutex_init(&car->queue_mutex, NULL);
        pthread_mutex_unlock(&data_mutex);

        free(message);

        // Continue to receive messages from car
        while (keep_running) {
            if (receive_message(sockfd, &message) != 0) {
                break;
            }
            if (strncmp(message, "STATUS ", 7) == 0) {
                // Update car status
                pthread_mutex_lock(&data_mutex);
                sscanf(message + 7, "%7s %3s %3s", car->status, car->current_floor, car->destination_floor);
                pthread_mutex_unlock(&data_mutex);
            } else if (strcmp(message, "INDIVIDUAL SERVICE") == 0 || strcmp(message, "EMERGENCY") == 0) {
                // Remove car from service
                pthread_mutex_lock(&data_mutex);
                // Remove car from array
                for (int i = 0; i < num_cars; ++i) {
                    if (&cars[i] == car) {
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

                        cars[i] = cars[--num_cars];
                        break;
                    }
                }
                pthread_mutex_unlock(&data_mutex);
                break;
            }
            free(message);
        }
    } else {
        free(message);
    }

    close(sockfd);
    return NULL;
}

// Function to select the best car for a call request
car_info *select_best_car(call_request *call) {
    car_info *best_car = NULL;
    int min_distance = 1000; // Arbitrary large number

    for (int i = 0; i < num_cars; ++i) {
        car_info *car = &cars[i];
        // Check if car can service both source and destination floors
        if (is_floor_in_range(call->source_floor, car->lowest_floor, car->highest_floor) &&
            is_floor_in_range(call->dest_floor, car->lowest_floor, car->highest_floor)) {
            // Calculate distance from car's current floor to source floor
            int distance = abs(compare_floors(car->current_floor, call->source_floor));
            if (distance < min_distance) {
                min_distance = distance;
                best_car = car;
            }
        }
    }
    return best_car;
}

// Function to insert floors into car's queue according to the scheduling approach
void insert_into_queue(car_info *car, call_request *call) {
    // Create floor requests for source and destination
    floor_request *from_request = malloc(sizeof(floor_request));
    strncpy(from_request->floor, call->source_floor, sizeof(from_request->floor));
    from_request->direction = call->direction;
    from_request->next = NULL;

    floor_request *to_request = malloc(sizeof(floor_request));
    strncpy(to_request->floor, call->dest_floor, sizeof(to_request->floor));
    to_request->direction = call->direction;
    to_request->next = NULL;

    pthread_mutex_lock(&car->queue_mutex);

    // Insert into car's queue following the suggested scheduling approach
    floor_request **current = &car->queue_head;
    floor_request *prev = NULL;

    // Find the correct position to insert 'from' floor
    while (*current) {
        floor_request *req = *current;
        int cmp = compare_floors(from_request->floor, req->floor);
        if (from_request->direction == req->direction) {
            if ((from_request->direction == UP && cmp < 0) ||
                (from_request->direction == DOWN && cmp > 0)) {
                break;
            }
        }
        prev = req;
        current = &req->next;
    }
    // Insert 'from' request
    from_request->next = *current;
    if (prev) {
        prev->next = from_request;
    } else {
        car->queue_head = from_request;
    }

    // Find the correct position to insert 'to' floor
    current = &from_request->next;
    prev = from_request;
    while (*current) {
        floor_request *req = *current;
        int cmp = compare_floors(to_request->floor, req->floor);
        if (to_request->direction == req->direction) {
            if ((to_request->direction == UP && cmp < 0) ||
                (to_request->direction == DOWN && cmp > 0)) {
                break;
            }
        }
        prev = req;
        current = &req->next;
    }
    // Insert 'to' request
    to_request->next = *current;
    prev->next = to_request;

    pthread_mutex_unlock(&car->queue_mutex);
}

// Function to send the next floor to the car
void update_car_destination(car_info *car) {
    pthread_mutex_lock(&car->queue_mutex);
    if (car->queue_head) {
        // Send FLOOR message to car
        char floor_msg[16];
        snprintf(floor_msg, sizeof(floor_msg), "FLOOR %s", car->queue_head->floor);
        send_message(car->sockfd, floor_msg);
        // Update car's destination_floor
        strncpy(car->destination_floor, car->queue_head->floor, sizeof(car->destination_floor));
    }
    pthread_mutex_unlock(&car->queue_mutex);
}

// Function to handle call pad connections
void *handle_call(void *arg) {
    int sockfd = *(int *)arg;
    free(arg);
    char *message = NULL;

    // Receive CALL message
    if (receive_message(sockfd, &message) != 0) {
        close(sockfd);
        return NULL;
    }

    if (strncmp(message, "CALL ", 5) == 0) {
        // Parse CALL message
        char source_floor[4], dest_floor[4];
        if (sscanf(message + 5, "%3s %3s", source_floor, dest_floor) != 2) {
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
        strncpy(call.source_floor, source_floor, sizeof(call.source_floor));
        strncpy(call.dest_floor, dest_floor, sizeof(call.dest_floor));
        call.direction = dir;

        // Find the best car to service the request
        pthread_mutex_lock(&data_mutex);
        car_info *selected_car = select_best_car(&call);

        if (selected_car) {
            // Insert floors into car's queue
            insert_into_queue(selected_car, &call);

            // Send CAR response to call pad
            char response[64];
            snprintf(response, sizeof(response), "CAR %s", selected_car->name);
            send_message(sockfd, response);

            // Update car destination if needed
            update_car_destination(selected_car);
        } else {
            // No available car
            send_message(sockfd, "UNAVAILABLE");
        }
        pthread_mutex_unlock(&data_mutex);
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
            pthread_t car_thread;
            // Since we've read the CAR message, we'll handle it in the thread
            // Push the message back into the socket buffer (simulate)
            // For simplicity, we'll pass the message to the thread
            pthread_create(&car_thread, NULL, handle_car, new_sock);
            pthread_detach(car_thread);
        } else if (strncmp(message, "CALL ", 5) == 0) {
            // Call pad connection
            // Handle the message in the thread
            pthread_t call_thread;
            pthread_create(&call_thread, NULL, handle_call, new_sock);
            pthread_detach(call_thread);
        } else {
            // Unknown connection
            close(*new_sock);
            free(new_sock);
        }
        free(message);
    }

    close(listen_sock);
}

int main(int argc, char *argv[]) {
    setup_signal_handler(int_handler);
    run_controller();
    return EXIT_SUCCESS;
}