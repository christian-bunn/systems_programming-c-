/*
Note: My get_next_floor_up and get_next_floor_down functions used in this file have been implemented in utils.c and declared in utils.h.
      This allows for them to be shared across components.
*/

#include "../headers/shared_memory.h"  // Include shared memory-related functions and data structures
#include "../headers/utils.h"          // Include utility functions (like compare_floors)
#include "../headers/internal.h"       // Include internal elevator control functions
#include <stdio.h>                     // Standard I/O library for printing errors and messages
#include <stdlib.h>                    // Standard library for memory management, process control
#include <string.h>                    // String manipulation functions
#include <pthread.h>                   // POSIX threads for multi-threading and synchronization
#include <fcntl.h>                     // File control options (for shared memory)
#include <sys/mman.h>                  // Memory mapping for shared memory

// Function to print an error message and exit the program, ensuring the mutex is unlocked
void print_error(const char *message, car_shared_mem *shared_mem) {
    printf("%s\n", message);              // Print the provided error message
    pthread_mutex_unlock(&shared_mem->mutex); // Unlock the shared memory mutex before exiting
    exit(EXIT_FAILURE);                   // Exit with failure status
}

int main(int argc, char *argv[]) {
    // Check for proper command-line arguments (car name and operation)
    if (argc != 3) {
        fprintf(stderr, "Usage: ./internal {car name} {operation}\n");
        exit(EXIT_FAILURE);               // Exit if the arguments are incorrect
    }

    // Store the car name and the operation (open, close, stop, etc.) provided by the user
    char *car_name = argv[1];
    char *operation = argv[2];

    // Construct the shared memory name based on the car name
    char shm_name[64];
    snprintf(shm_name, sizeof(shm_name), "/car%s", car_name);

    // Attempt to open the shared memory object for the car
    int fd = shm_open(shm_name, O_RDWR, 0666);
    if (fd == -1) {                       // If unable to access the shared memory
        printf("Unable to access car %s.\n", car_name);
        exit(EXIT_FAILURE);               // Exit with failure status
    }

    // Map the shared memory object into the process's address space
    car_shared_mem *shared_mem = mmap(NULL, sizeof(car_shared_mem), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (shared_mem == MAP_FAILED) {       // If memory mapping fails
        printf("Unable to access car %s.\n", car_name);
        exit(EXIT_FAILURE);               // Exit with failure status
    }

    // Lock the mutex before accessing the shared memory
    pthread_mutex_lock(&shared_mem->mutex);

    // Perform the operation specified by the user
    if (strcmp(operation, "open") == 0) {
        // Open the elevator doors
        shared_mem->open_button = 1;
    } else if (strcmp(operation, "close") == 0) {
        // Close the elevator doors
        shared_mem->close_button = 1;
    } else if (strcmp(operation, "stop") == 0) {
        // Trigger the emergency stop
        shared_mem->emergency_stop = 1;
    } else if (strcmp(operation, "service_on") == 0) {
        // Enable individual service mode (manual operation)
        shared_mem->individual_service_mode = 1;
        shared_mem->emergency_mode = 0;  // Ensure emergency mode is off
    } else if (strcmp(operation, "service_off") == 0) {
        // Disable individual service mode (automatic operation resumes)
        shared_mem->individual_service_mode = 0;
    } else if (strcmp(operation, "up") == 0 || strcmp(operation, "down") == 0) {
        // Handle manual movement in service mode (up or down)
        
        // Ensure the elevator is in individual service mode
        if (shared_mem->individual_service_mode == 0) {
            print_error("Operation only allowed in service mode.", shared_mem);
        }
        // Ensure the doors are closed before moving
        if (strcmp(shared_mem->status, "Closed") != 0) {
            print_error("Operation not allowed while doors are open.", shared_mem);
        }
        // Ensure the elevator is not currently moving
        if (strcmp(shared_mem->status, "Between") == 0) {
            print_error("Operation not allowed while elevator is moving.", shared_mem);
        }

        // Determine the next floor based on the current floor and direction (up or down)
        char next_floor[4];
        if (strcmp(operation, "up") == 0) {
            // Calculate the next floor up
            int floor_num;
            if (shared_mem->current_floor[0] == 'B') {  // If current floor is a basement floor (e.g., B1)
                floor_num = -atoi(shared_mem->current_floor + 1);  // Convert to a negative number
                floor_num++;  // Move up
                if (floor_num == 0) {
                    strcpy(next_floor, "1");  // Ground floor
                } else if (floor_num > 0) {
                    snprintf(next_floor, sizeof(next_floor), "%d", floor_num);  // Positive floor number
                } else {
                    snprintf(next_floor, sizeof(next_floor), "B%d", -floor_num);  // Back to a basement floor
                }
            } else {
                floor_num = atoi(shared_mem->current_floor);  // Convert the current floor to an integer
                floor_num++;  // Move up to the next floor
                snprintf(next_floor, sizeof(next_floor), "%d", floor_num);
            }
        } else {
            // Calculate the next floor down
            int floor_num;
            if (shared_mem->current_floor[0] == 'B') {  // If currently on a basement floor
                floor_num = -atoi(shared_mem->current_floor + 1);  // Convert to a negative number
                floor_num--;  // Move down
                snprintf(next_floor, sizeof(next_floor), "B%d", -floor_num);  // Go to the next basement floor
            } else {
                floor_num = atoi(shared_mem->current_floor);  // Convert to an integer
                floor_num--;  // Move down to the next floor
                if (floor_num == 0) {
                    strcpy(next_floor, "B1");  // Basement floor B1
                } else if (floor_num > 0) {
                    snprintf(next_floor, sizeof(next_floor), "%d", floor_num);  // Above ground
                } else {
                    snprintf(next_floor, sizeof(next_floor), "B%d", -floor_num);  // Below ground
                }
            }
        }

        // Ensure the next floor is within the elevator's range and not the same as the current or destination floor
        if (compare_floors(next_floor, shared_mem->current_floor) == 0 ||
            (compare_floors(next_floor, shared_mem->destination_floor) == 0)) {
            print_error("Cannot move beyond elevator's range.", shared_mem);
        }

        // Set the destination floor to the next floor
        strcpy(shared_mem->destination_floor, next_floor);
    } else {
        // If the operation is invalid, print an error
        print_error("Invalid operation.", shared_mem);
    }

    // Broadcast a signal to notify other threads that the shared memory has been updated
    pthread_cond_broadcast(&shared_mem->cond);

    // Unlock the mutex after updating the shared memory
    pthread_mutex_unlock(&shared_mem->mutex);

    return 0;  // Program exits successfully
}
