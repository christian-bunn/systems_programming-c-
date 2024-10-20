#include "../headers/shared_memory.h"   // Include shared memory-related function and structure declarations
#include "../headers/safety.h"          // Include safety system-specific function declarations
#include "utils.h"                      // Include utility functions for validations
#include <stdio.h>                      // Standard I/O library for printing messages
#include <stdlib.h>                     // Standard library for memory allocation and process control
#include <string.h>                     // String manipulation functions
#include <pthread.h>                    // POSIX threads for synchronization and concurrency
#include <unistd.h>                     // UNIX standard library for system calls (e.g., sleep, close)
#include <signal.h>                     // Signal handling functions

// Global flag that indicates whether the program should keep running
static volatile sig_atomic_t keep_running = 1;

// Signal handler to stop the program when receiving a specific signal (e.g., SIGINT)
static void int_handler(int dummy) {
    (void)dummy;       // Avoid unused parameter warning
    keep_running = 0;  // Set the flag to stop the loop
}

// Function to run the safety system for a specific car
void run_safety_system(const char *car_name) {
    char shm_name[256];      // Buffer to hold the shared memory name
    car_shared_mem *car_mem; // Pointer to the shared memory for the car

    // Create the shared memory name based on the car's name
    if (snprintf(shm_name, sizeof(shm_name), "/car%s", car_name) >= (int)sizeof(shm_name)) {
        fprintf(stderr, "Car name too long.\n");  // If the car name is too long
        exit(EXIT_FAILURE);                       // Exit with failure status
    }

    // Open the shared memory for the car
    if (open_shared_memory(shm_name, &car_mem) != 0) {
        fprintf(stderr, "Unable to access car %s.\n", car_name);  // If the shared memory could not be opened
        exit(EXIT_FAILURE);                                       // Exit with failure status
    }

    // Main loop that continues running until the safety system is stopped (by signal)
    while (keep_running) {
        // Lock the shared memory's mutex to access the car's state safely
        if (pthread_mutex_lock(&car_mem->mutex) != 0) {
            perror("pthread_mutex_lock");  // Print an error if locking fails
            break;                         // Exit the loop on failure
        }

        // Wait for a condition variable signal (when car state changes)
        while (pthread_cond_wait(&car_mem->cond, &car_mem->mutex) != 0) {
            // Retry on failure (loop until the condition variable is signaled)
        }

        // Safety check: if the door is obstructed while closing, reopen the doors
        if (car_mem->door_obstruction == 1 && strcmp(car_mem->status, "Closing") == 0) {
            // Set the status to "Opening" to reopen the doors
            strncpy(car_mem->status, "Opening", sizeof(car_mem->status));
            car_mem->status[STATUS_STR_SIZE - 1] = '\0';  // Ensure the status string is null-terminated
            printf("Door obstruction detected! Opening doors.\n");
            pthread_cond_broadcast(&car_mem->cond);  // Notify other threads about the status change
        }

        // Safety check: if the emergency stop button has been pressed, enter emergency mode
        if (car_mem->emergency_stop == 1 && car_mem->emergency_mode == 0) {
            printf("The emergency stop button has been pressed!\n");
            car_mem->emergency_mode = 1;  // Enable emergency mode
            pthread_cond_broadcast(&car_mem->cond);  // Notify other threads
        }

        // Safety check: if the overload sensor is triggered, enter emergency mode
        if (car_mem->overload == 1 && car_mem->emergency_mode == 0) {
            printf("The overload sensor has been tripped!\n");
            car_mem->emergency_mode = 1;  // Enable emergency mode
            pthread_cond_broadcast(&car_mem->cond);  // Notify other threads
        }

        // If the car is not in emergency mode, perform additional data consistency checks
        if (car_mem->emergency_mode != 1) {
            int data_error = 0;  // Flag to indicate if there's a data consistency error

            // Check if the current floor and destination floor are valid
            if (!is_valid_floor(car_mem->current_floor) || !is_valid_floor(car_mem->destination_floor)) {
                data_error = 1;
            }

            // Check if the car's status is valid
            if (!is_valid_status(car_mem->status)) {
                data_error = 1;
            }

            // Check if any of the control flags have invalid values (should be 0 or 1)
            if (car_mem->open_button > 1 || car_mem->close_button > 1 || car_mem->door_obstruction > 1 ||
                car_mem->overload > 1 || car_mem->emergency_stop > 1 || car_mem->individual_service_mode > 1 ||
                car_mem->emergency_mode > 1) {
                data_error = 1;
            }

            // Check if the door is obstructed when the elevator is not opening or closing
            if (car_mem->door_obstruction == 1 && strcmp(car_mem->status, "Opening") != 0 &&
                strcmp(car_mem->status, "Closing") != 0) {
                data_error = 1;
            }

            // If any data consistency errors are detected, enter emergency mode
            if (data_error) {
                printf("Data consistency error!\n");
                car_mem->emergency_mode = 1;  // Enable emergency mode
                pthread_cond_broadcast(&car_mem->cond);  // Notify other threads
            }
        }

        // Unlock the mutex after performing the safety checks
        if (pthread_mutex_unlock(&car_mem->mutex) != 0) {
            perror("pthread_mutex_unlock");  // Print an error if unlocking fails
            break;                           // Exit the loop on failure
        }
    }

    // Close the shared memory when the safety system is done
    close_shared_memory(car_mem);
}

// Main function: sets up the safety system and starts it
int main(int argc, char *argv[]) {
    // Check if the user provided the correct number of arguments (car name)
    if (argc != 2) {
        fprintf(stderr, "Usage: %s {car name}\n", argv[0]);
        return EXIT_FAILURE;  // Exit with failure status if arguments are incorrect
    }

    // Set up the signal handler to catch SIGINT (Ctrl+C) and stop the system gracefully
    setup_signal_handler(int_handler);

    // Run the safety system for the specified car
    run_safety_system(argv[1]);

    return EXIT_SUCCESS;  // Exit with success status
}
