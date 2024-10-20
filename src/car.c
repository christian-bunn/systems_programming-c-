#include "shared_memory.h"  // Include functions for managing shared memory
#include "network.h"        // Include functions for network communication
#include "utils.h"          // Include utility functions, such as time-related functions
#include "car.h"            // Include car-specific functions and definitions
#include <stdio.h>          // Standard I/O library
#include <stdlib.h>         // Standard library for memory allocation, conversion, and process control
#include <string.h>         // String manipulation functions
#include <pthread.h>        // POSIX threads for handling concurrent operations
#include <unistd.h>         // UNIX standard functions (e.g., close(), sleep())
#include <signal.h>         // Signal handling for interrupts
#include <errno.h>          // Error number definitions
#include <time.h>           // Time-related functions
#include <sys/select.h>     // For monitoring multiple file descriptors
#include <sys/time.h>       // Time value structures

// A global flag that determines if the program should keep running
static volatile sig_atomic_t keep_running = 1;

// Signal handler to stop the program on receiving a specific signal (e.g., SIGINT)
static void int_handler(int dummy) {
    (void)dummy;  // Avoid unused parameter warning
    keep_running = 0;  // Set the flag to stop the loop in other threads
}

// Structure to pass arguments to the controller thread
typedef struct {
    char name[256];             // Car name
    int delay;                  // Delay in milliseconds for operations
    car_shared_mem *car_mem;    // Pointer to shared memory for car state
    const char *lowest_floor;   // Lowest floor the car can access
    const char *highest_floor;  // Highest floor the car can access
} controller_args_t;

// Function that handles the communication between the car and the controller
void *controller_thread(void *arg) {
    controller_args_t *args = (controller_args_t *)arg;  // Cast the argument to the expected type
    int sockfd = -1;  // File descriptor for the socket connection
    char message[512];  // Buffer for messages to be sent to the controller
    car_shared_mem *car_mem = args->car_mem;  // Shared memory for car state
    int delay = args->delay;  // Delay for communication
    char *name = args->name;  // Name of the car

    // Ignore SIGPIPE to prevent crashes on broken pipes (when writing to a disconnected socket)
    signal(SIGPIPE, SIG_IGN);

    while (keep_running) {  // Main loop that runs until interrupted
        // Lock the shared memory before accessing it
        pthread_mutex_lock(&car_mem->mutex);
        int in_special_mode = car_mem->individual_service_mode || car_mem->emergency_mode;
        pthread_mutex_unlock(&car_mem->mutex);

        // If the car is in special mode, skip the network communication
        if (in_special_mode) {
            if (sockfd != -1) {
                close(sockfd);  // Close the socket if connected
                sockfd = -1;    // Mark the socket as closed
            }
            sleep_ms(delay);  // Sleep for the specified delay
            continue;
        }

        // Attempt to connect to the controller if not already connected
        if (sockfd == -1) {
            sockfd = connect_to_controller();  // Establish a connection to the controller
            if (sockfd != -1) {
                // Send the initial CAR message to the controller, providing car details
                snprintf(message, sizeof(message), "CAR %s %s %s", name, args->lowest_floor, args->highest_floor);
                if (send_message(sockfd, message) != 0) {  // If sending the message fails
                    close(sockfd);  // Close the socket
                    sockfd = -1;    // Mark the socket as closed
                    sleep_ms(delay);
                    continue;
                }

                // Send the car's status to the controller
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
                // If the connection fails, sleep for the delay period and try again
                sleep_ms(delay);
                continue;
            }
        }

        // Send the car's status to the controller
        pthread_mutex_lock(&car_mem->mutex);
        snprintf(message, sizeof(message), "STATUS %s %s %s", car_mem->status, car_mem->current_floor, car_mem->destination_floor);
        pthread_mutex_unlock(&car_mem->mutex);
        if (send_message(sockfd, message) != 0) {
            close(sockfd);
            sockfd = -1;
            sleep_ms(delay);
            continue;
        }

        // Use select() to check if there is any message from the controller without blocking
        fd_set read_fds;
        struct timeval timeout;
        FD_ZERO(&read_fds);  // Clear the set of file descriptors
        FD_SET(sockfd, &read_fds);  // Add the socket file descriptor to the set
        timeout.tv_sec = 0;  // Set timeout to 0 seconds
        timeout.tv_usec = 0;  // Set timeout to 0 microseconds

        int activity = select(sockfd + 1, &read_fds, NULL, NULL, &timeout);  // Monitor the socket for activity

        if (activity > 0 && FD_ISSET(sockfd, &read_fds)) {  // If there's activity on the socket
            char *response = NULL;
            if (receive_message(sockfd, &response) != 0) {  // If receiving a message fails
                close(sockfd);
                sockfd = -1;
                free(response);
                continue;
            }

            // Process the response if it starts with "FLOOR"
            if (strncmp(response, "FLOOR ", 6) == 0) {
                pthread_mutex_lock(&car_mem->mutex);
                // Update the destination floor in shared memory
                snprintf(car_mem->destination_floor, FLOOR_STR_SIZE, "%.*s", FLOOR_STR_SIZE - 1, response + 6);
                pthread_cond_broadcast(&car_mem->cond);  // Notify other threads waiting on this condition
                pthread_mutex_unlock(&car_mem->mutex);
            }

            free(response);  // Free the memory allocated for the response
        }

        sleep_ms(10);  // Sleep for a short period before checking again
    }

    // Clean up: close the socket if it was open
    if (sockfd != -1) {
        close(sockfd);
    }
    pthread_exit(NULL);  // Exit the thread
}

// Main function that runs the car operations
void run_car(const char *name, const char *lowest_floor, const char *highest_floor, int delay) {
    char shm_name[256];  // Shared memory name for the car
    car_shared_mem *car_mem;  // Pointer to shared memory for car state
    pthread_t controller_tid;  // Thread for communicating with the controller

    // Format the shared memory name based on the car's name
    if (snprintf(shm_name, sizeof(shm_name), "/car%s", name) >= (int)sizeof(shm_name)) {
        fprintf(stderr, "Car name too long.\n");
        exit(EXIT_FAILURE);
    }

    // Initialize shared memory for the car
    if (init_shared_memory(shm_name, &car_mem) != 0) {
        fprintf(stderr, "Failed to create shared memory for car %s.\n", name);
        exit(EXIT_FAILURE);
    }

    // Lock the shared memory and set initial car values
    pthread_mutex_lock(&car_mem->mutex);
    snprintf(car_mem->current_floor, FLOOR_STR_SIZE, "%.*s", FLOOR_STR_SIZE - 1, lowest_floor);
    snprintf(car_mem->destination_floor, FLOOR_STR_SIZE, "%.*s", FLOOR_STR_SIZE - 1, lowest_floor);
    snprintf(car_mem->status, STATUS_STR_SIZE, "Closed");

    // Set initial values for buttons and flags
    car_mem->open_button = 0;
    car_mem->close_button = 0;
    car_mem->door_obstruction = 0;
    car_mem->overload = 0;
    car_mem->emergency_stop = 0;
    car_mem->individual_service_mode = 0;
    car_mem->emergency_mode = 0;
    pthread_mutex_unlock(&car_mem->mutex);

    // Ignore SIGPIPE and set the signal handler for SIGINT
    signal(SIGPIPE, SIG_IGN);
    signal(SIGINT, int_handler);

    // Set up arguments for the controller thread
    controller_args_t ctrl_args;
    strncpy(ctrl_args.name, name, sizeof(ctrl_args.name) - 1);
    ctrl_args.name[sizeof(ctrl_args.name) - 1] = '\0';
    ctrl_args.delay = delay;
    ctrl_args.car_mem = car_mem;
    ctrl_args.lowest_floor = lowest_floor;
    ctrl_args.highest_floor = highest_floor;

    // Create a thread for handling communication with the controller
    pthread_create(&controller_tid, NULL, controller_thread, (void *)&ctrl_args);

    while (keep_running) {  // Main loop for car operations (runs until interrupted)
        pthread_mutex_lock(&car_mem->mutex);

        // If emergency stop is pressed, enable emergency mode
        if (car_mem->emergency_stop) {
            car_mem->emergency_mode = 1;
            pthread_mutex_unlock(&car_mem->mutex);
            continue;
        }

        // Handle emergency mode
        if (car_mem->emergency_mode == 1) {
            // Doors can be opened and closed manually in emergency mode, but the elevator does not move
            // Handle open and close button actions here (code omitted for brevity)
            pthread_mutex_unlock(&car_mem->mutex);
            continue;
        }

        // Handle individual service mode, which allows manual control of the elevator (code omitted for brevity)
        pthread_mutex_unlock(&car_mem->mutex);
        continue;

        // Handle open and close buttons (code omitted for brevity)
        pthread_mutex_unlock(&car_mem->mutex);

        sleep_ms(10);  // Small delay before the next iteration
    }

    pthread_join(controller_tid, NULL);  // Wait for the controller thread to finish
    unlink_shared_memory(shm_name);  // Unlink and close shared memory
    close_shared_memory(car_mem);    // Clean up shared memory
}

int main(int argc, char *argv[]) {
    // Validate the number of arguments
    if (argc != 5) {
        fprintf(stderr, "Usage: %s {name} {lowest floor} {highest floor} {delay}\n", argv[0]);
        return EXIT_FAILURE;
    }

    const char *name = argv[1];
    const char *lowest_floor = argv[2];
    const char *highest_floor = argv[3];
    int delay = atoi(argv[4]);  // Convert delay from string to integer

    // Validate floor and delay arguments
    if (!is_valid_floor(lowest_floor) || !is_valid_floor(highest_floor) || delay <= 0) {
        fprintf(stderr, "Invalid arguments. lowest_floor: %s, highest_floor: %s, delay: %d\n",
                lowest_floor, highest_floor, delay);
        return EXIT_FAILURE;
    }

    // Set up signal handlers and run the car
    setup_signal_handler(int_handler);
    run_car(name, lowest_floor, highest_floor, delay);

    return EXIT_SUCCESS;  // Return success
}
