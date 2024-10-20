#include "../headers/network.h"       // Include functions for network communication
#include "../headers/utils.h"         // Include utility functions (helpers for signals, time, etc.)
#include "../headers/controller.h"    // Include controller-specific functions and definitions
#include "shared_memory.h"            // Include shared memory functions
#include <stdio.h>                    // Standard I/O library
#include <stdlib.h>                   // Standard library for memory allocation, process control
#include <string.h>                   // String manipulation functions
#include <pthread.h>                  // POSIX threads for multi-threading
#include <unistd.h>                   // UNIX standard functions
#include <signal.h>                   // Signal handling (e.g., for SIGINT)
#include <netinet/in.h>               // Internet address family for sockets
#include <sys/socket.h>               // Socket-related functions
#include <sys/types.h>                // Types for sockets
#include <errno.h>                    // Error numbers for socket functions
#include <arpa/inet.h>                // Functions for internet operations (like `inet_addr()`)
#include <limits.h>                   // Definitions for integer limits (e.g., INT_MAX)

// Global flag to control whether the program should keep running
static volatile sig_atomic_t keep_running = 1;

// Signal handler to gracefully stop the program when interrupted (e.g., by SIGINT)
static void int_handler(int dummy) {
    (void)dummy;  // Avoid unused parameter warning
    keep_running = 0;  // Set flag to stop the main loop
}

// Enumeration for elevator direction states: UP, DOWN, or IDLE (not moving)
typedef enum { UP = 1, DOWN = -1, IDLE = 0 } Direction;

// Structure for a floor request in the elevator system
typedef struct floor_request {
    char floor[FLOOR_STR_SIZE];      // Requested floor
    Direction direction;             // Direction of the request (UP or DOWN)
    struct floor_request *next;      // Pointer to the next request in the queue
} floor_request;

// Structure to store information about each elevator car
typedef struct {
    int sockfd;                      // Socket file descriptor for network communication
    char name[32];                   // Name of the car
    char lowest_floor[FLOOR_STR_SIZE]; // Lowest accessible floor
    char highest_floor[FLOOR_STR_SIZE]; // Highest accessible floor
    char status[16];                 // Current status (e.g., "Open", "Closed", etc.)
    char current_floor[FLOOR_STR_SIZE]; // Current floor
    char destination_floor[FLOOR_STR_SIZE]; // Destination floor
    Direction direction;             // Direction of movement (UP, DOWN, or IDLE)
    floor_request *queue_head;       // Head of the queue for floor requests
    pthread_mutex_t queue_mutex;     // Mutex for synchronizing access to the request queue
} car_info;

// Structure for handling incoming call requests
typedef struct {
    char source_floor[FLOOR_STR_SIZE]; // Source floor of the call
    char dest_floor[FLOOR_STR_SIZE];   // Destination floor of the call
    Direction direction;               // Direction of the call
} call_request;

// Structure for passing arguments to client threads
typedef struct {
    int sockfd;                       // Socket file descriptor for network communication
    char *message;                    // Message received from the client (car or call)
} client_arg_t;

#define MAX_CARS 10                   // Maximum number of cars supported
static car_info cars[MAX_CARS];        // Array to store all car information
static int num_cars = 0;               // Current number of cars in the system

pthread_mutex_t data_mutex = PTHREAD_MUTEX_INITIALIZER;  // Mutex to protect car data

// Function to remove a car from service (called when a car goes into emergency or individual service mode)
void remove_car_from_service(car_info *car) {
    pthread_mutex_lock(&data_mutex);  // Lock to synchronize access to car data

    // Remove all floor requests for this car
    pthread_mutex_lock(&car->queue_mutex);
    floor_request *req = car->queue_head;
    while (req) {
        floor_request *tmp = req;
        req = req->next;
        free(tmp);
    }
    pthread_mutex_unlock(&car->queue_mutex);
    pthread_mutex_destroy(&car->queue_mutex);

    // Remove the car from the array
    for (int i = 0; i < num_cars; ++i) {
        if (&cars[i] == car) {
            cars[i] = cars[--num_cars];  // Replace the car with the last car in the array
            break;
        }
    }
    pthread_mutex_unlock(&data_mutex);  // Unlock the data mutex
    close(car->sockfd);  // Close the socket for the car
}

// Thread function to handle communication with a car
void *handle_car(void *arg) {
    client_arg_t *car_arg = (client_arg_t *)arg;  // Cast the argument to the expected type
    int sockfd = car_arg->sockfd;
    char *message = car_arg->message;
    free(arg);
    car_info *car = NULL;

    // If the message starts with "CAR", process the car information
    if (strncmp(message, "CAR ", 4) == 0) {
        char car_name[32], low_floor[FLOOR_STR_SIZE], high_floor[FLOOR_STR_SIZE];
        if (sscanf(message + 4, "%31s %11s %11s", car_name, low_floor, high_floor) != 3) {
            free(message);
            close(sockfd);
            return NULL;
        }

        free(message);

        pthread_mutex_lock(&data_mutex);
        if (num_cars >= MAX_CARS) {  // If maximum cars are already in the system
            pthread_mutex_unlock(&data_mutex);
            close(sockfd);
            return NULL;
        }

        // Add the new car to the system
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

        strncpy(car->status, "Closed", sizeof(car->status) - 1);
        car->status[sizeof(car->status) - 1] = '\0';
        strncpy(car->current_floor, low_floor, sizeof(car->current_floor) - 1);
        car->current_floor[sizeof(car->current_floor) - 1] = '\0';
        strncpy(car->destination_floor, low_floor, sizeof(car->destination_floor) - 1);
        car->destination_floor[sizeof(car->destination_floor) - 1] = '\0';
        car->direction = IDLE;

        pthread_mutex_unlock(&data_mutex);

        // Main loop to handle car messages
        while (keep_running) {
            if (receive_message(sockfd, &message) != 0) {  // Receive message from car
                break;  // If there's an error, exit the loop
            }
            if (strncmp(message, "STATUS ", 7) == 0) {  // If the message is a status update
                pthread_mutex_lock(&car->queue_mutex);
                char prev_current_floor[FLOOR_STR_SIZE];
                strncpy(prev_current_floor, car->current_floor, FLOOR_STR_SIZE);

                sscanf(message + 7, "%15s %11s %11s", car->status, car->current_floor, car->destination_floor);

                // Update car direction based on current and destination floors
                int cmp = compare_floors(car->destination_floor, car->current_floor);
                if (cmp > 0) {
                    car->direction = UP;
                } else if (cmp < 0) {
                    car->direction = DOWN;
                } else {
                    car->direction = IDLE;
                }

                // Check if the car has arrived at a requested floor
                if ((strcmp(car->status, "Opening") == 0 || strcmp(car->status, "Open") == 0) &&
                    car->queue_head && strcmp(car->queue_head->floor, car->current_floor) == 0) {
                    floor_request *old_head = car->queue_head;
                    car->queue_head = car->queue_head->next;
                    free(old_head);

                    // If there are more requests in the queue, update the destination floor
                    if (car->queue_head) {
                        char floor_msg[20];
                        snprintf(floor_msg, sizeof(floor_msg), "FLOOR %s", car->queue_head->floor);
                        send_message(car->sockfd, floor_msg);
                        strncpy(car->destination_floor, car->queue_head->floor, sizeof(car->destination_floor) - 1);
                        car->destination_floor[sizeof(car->destination_floor) - 1] = '\0';
                    } else {
                        car->direction = IDLE;
                    }
                }

                pthread_mutex_unlock(&car->queue_mutex);
            } else if (strcmp(message, "INDIVIDUAL SERVICE") == 0 || strcmp(message, "EMERGENCY") == 0) {
                // If the car enters individual service or emergency mode, remove it from service
                remove_car_from_service(car);
                free(message);
                return NULL;
            }
            free(message);
        }
    } else {
        free(message);  // Free memory for invalid messages
    }

    // Remove the car from service when done
    remove_car_from_service(car);
    return NULL;
}

// Function to select the best car for a given call request
car_info *select_best_car(call_request *call) {
    car_info *best_car = NULL;
    int min_distance = INT_MAX;

    pthread_mutex_lock(&data_mutex);  // Lock to protect car data
    for (int i = 0; i < num_cars; ++i) {
        car_info *car = &cars[i];

        // Check if the car can service the request (based on floor range)
        if (is_floor_in_range(call->source_floor, car->lowest_floor, car->highest_floor) &&
            is_floor_in_range(call->dest_floor, car->lowest_floor, car->highest_floor)) {

            int distance = abs(compare_floors(car->current_floor, call->source_floor));

            // Select the closest car that is either idle or has no requests in its queue
            if (distance < min_distance && (car->direction == IDLE || car->queue_head == NULL)) {
                min_distance = distance;
                best_car = car;
            }
        }
    }
    pthread_mutex_unlock(&data_mutex);

    return best_car;
}

// Function to insert a call request into a car's queue
void insert_into_queue(car_info *car, call_request *call) {
    pthread_mutex_lock(&car->queue_mutex);  // Lock to protect the queue

    Direction direction = car->direction;
    if (direction == IDLE) {
        // Determine the direction based on current and source floors
        direction = compare_floors(car->current_floor, call->source_floor) < 0 ? UP : DOWN;
        car->direction = direction;
    }

    // Create the request for the source floor
    floor_request *from_request = malloc(sizeof(floor_request));
    strncpy(from_request->floor, call->source_floor, sizeof(from_request->floor) - 1);
    from_request->floor[sizeof(from_request->floor) - 1] = '\0';
    from_request->direction = direction;
    from_request->next = NULL;

    // Create the request for the destination floor
    floor_request *to_request = malloc(sizeof(floor_request));
    strncpy(to_request->floor, call->dest_floor, sizeof(to_request->floor) - 1);
    to_request->floor[sizeof(to_request->floor) - 1] = '\0';
    to_request->direction = compare_floors(call->source_floor, call->dest_floor) < 0 ? UP : DOWN;
    to_request->next = NULL;

    // Insert the source and destination requests into the car's queue
    floor_request **current = &car->queue_head;
    floor_request *prev = NULL;
    int inserted = 0;

    while (*current) {
        floor_request *req = *current;
        int cmp = compare_floors(from_request->floor, req->floor);
        if ((direction == UP && cmp < 0) || (direction == DOWN && cmp > 0)) {
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

    // If not inserted, add it at the end of the queue
    if (!inserted) {
        if (prev) {
            prev->next = from_request;
        } else {
            car->queue_head = from_request;
        }
    }

    // Insert the destination request in the appropriate position
    current = &from_request->next;
    prev = from_request;
    inserted = 0;

    while (*current) {
        floor_request *req = *current;
        int cmp = compare_floors(to_request->floor, req->floor);
        if ((to_request->direction == UP && cmp < 0) || (to_request->direction == DOWN && cmp > 0)) {
            to_request->next = req;
            prev->next = to_request;
            inserted = 1;
            break;
        }
        prev = req;
        current = &req->next;
    }

    // If not inserted, add it at the end of the queue
    if (!inserted) {
        prev->next = to_request;
    }

    // If this is the first request, notify the car to go to the source floor
    if (car->queue_head == from_request) {
        char floor_msg[20];
        snprintf(floor_msg, sizeof(floor_msg), "FLOOR %s", car->queue_head->floor);
        send_message(car->sockfd, floor_msg);
        strncpy(car->destination_floor, car->queue_head->floor, sizeof(car->destination_floor) - 1);
        car->destination_floor[sizeof(car->destination_floor) - 1] = '\0';
    }

    pthread_mutex_unlock(&car->queue_mutex);
}

// Thread function to handle call requests from clients
void *handle_call(void *arg) {
    client_arg_t *call_arg = (client_arg_t *)arg;  // Cast the argument to the expected type
    int sockfd = call_arg->sockfd;
    char *message = call_arg->message;
    free(arg);

    // Process the call request
    if (strncmp(message, "CALL ", 5) == 0) {
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

        // Determine the direction of the call
        Direction dir = (compare_floors(source_floor, dest_floor) < 0) ? UP : DOWN;

        call_request call;
        strncpy(call.source_floor, source_floor, sizeof(call.source_floor) - 1);
        call.source_floor[sizeof(call.source_floor) - 1] = '\0';
        strncpy(call.dest_floor, dest_floor, sizeof(call.dest_floor) - 1);
        call.dest_floor[sizeof(call.dest_floor) - 1] = '\0';
        call.direction = dir;

        // Select the best car for the call
        car_info *selected_car = select_best_car(&call);

        if (selected_car) {
            // Insert the call into the selected car's queue
            insert_into_queue(selected_car, &call);

            // Send a response to the client with the car name
            char response[64];
            snprintf(response, sizeof(response), "CAR %s", selected_car->name);
            send_message(sockfd, response);
        } else {
            send_message(sockfd, "UNAVAILABLE");  // No available car
        }
    } else {
        send_message(sockfd, "UNAVAILABLE");  // Invalid message format
    }

    free(message);
    close(sockfd);
    return NULL;
}

// Main controller loop that listens for incoming connections (either cars or calls)
void run_controller() {
    int listen_sock;
    struct sockaddr_in serv_addr;
    int opt_enable = 1;

    // Create a TCP socket for listening
    listen_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_sock == -1) {
        perror("socket");
        exit(EXIT_FAILURE);
    }

    // Set socket options to allow address reuse
    setsockopt(listen_sock, SOL_SOCKET, SO_REUSEADDR, &opt_enable, sizeof(opt_enable));

    // Configure server address structure
    memset(&serv_addr, 0, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_addr.s_addr = inet_addr(CONTROLLER_IP);
    serv_addr.sin_port = htons(CONTROLLER_PORT);

    // Bind the socket to the address and port
    if (bind(listen_sock, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) != 0) {
        perror("bind");
        close(listen_sock);
        exit(EXIT_FAILURE);
    }

    // Start listening for incoming connections
    if (listen(listen_sock, 5) != 0) {
        perror("listen");
        close(listen_sock);
        exit(EXIT_FAILURE);
    }

    // Main loop to accept and handle incoming connections
    while (keep_running) {
        int *new_sock = malloc(sizeof(int));  // Allocate memory for new socket
        *new_sock = accept(listen_sock, NULL, NULL);  // Accept a new connection
        if (*new_sock == -1) {
            if (errno == EINTR) {  // Handle interrupted system call (e.g., by SIGINT)
                free(new_sock);
                break;
            }
            perror("accept");
            free(new_sock);
            continue;
        }

        // Receive the initial message from the client (either a car or call request)
        char *message = NULL;
        if (receive_message(*new_sock, &message) != 0) {
            close(*new_sock);
            free(new_sock);
            continue;
        }

        // If the message is from a car, handle it in a new thread
        if (strncmp(message, "CAR ", 4) == 0) {
            client_arg_t *car_arg = malloc(sizeof(client_arg_t));
            car_arg->sockfd = *new_sock;
            car_arg->message = message;
            pthread_t car_thread;
            pthread_create(&car_thread, NULL, handle_car, car_arg);
            pthread_detach(car_thread);
            free(new_sock);
        } 
        // If the message is a call request, handle it in a new thread
        else if (strncmp(message, "CALL ", 5) == 0) {
            client_arg_t *call_arg = malloc(sizeof(client_arg_t));
            call_arg->sockfd = *new_sock;
            call_arg->message = message;
            pthread_t call_thread;
            pthread_create(&call_thread, NULL, handle_call, call_arg);
            pthread_detach(call_thread);
            free(new_sock);
        } 
        // If the message format is invalid, close the connection
        else {
            close(*new_sock);
            free(new_sock);
            free(message);
        }
    }
    close(listen_sock);  // Close the listening socket when done
}

int main(int argc, char *argv[]) {
    (void)argc;
    (void)argv;
    setup_signal_handler(int_handler);  // Set up signal handler for SIGINT
    signal(SIGPIPE, SIG_IGN);  // Ignore SIGPIPE to avoid crashes on broken pipes
    run_controller();  // Start the main controller loop
    return EXIT_SUCCESS;
}
