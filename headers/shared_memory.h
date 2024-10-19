/// shared_memory.h

#ifndef SHARED_MEMORY_H
#define SHARED_MEMORY_H

#include <pthread.h>
#include <stdint.h>

#define STATUS_STR_SIZE 8
#define FLOOR_STR_SIZE 4

typedef struct {
    pthread_mutex_t mutex;                  // Locked while accessing struct contents
    pthread_cond_t cond;                    // Signalled when the contents change
    char current_floor[FLOOR_STR_SIZE];     // C string in the range B99-B1 and 1-999
    char destination_floor[FLOOR_STR_SIZE]; // Same format as above
    char status[STATUS_STR_SIZE];           // C string indicating the elevator's status
    uint8_t open_button;                    // 1 if open doors button is pressed, else 0
    uint8_t close_button;                   // 1 if close doors button is pressed, else 0
    uint8_t door_obstruction;               // 1 if obstruction detected, else 0
    uint8_t overload;                       // 1 if overload detected
    uint8_t emergency_stop;                 // 1 if stop button has been pressed, else 0
    uint8_t individual_service_mode;        // 1 if in individual service mode, else 0
    uint8_t emergency_mode;                 // 1 if in emergency mode, else 0
} car_shared_mem;

// Function to initialise shared memory 
int init_shared_memory(const char *shm_name, car_shared_mem **car_mem);

// Function to open existing shared memory
int open_shared_memory(const char *shm_name, car_shared_mem **car_mem);

// Function to close shared memory
void close_shared_memory(car_shared_mem *car_mem);

// Function to unlink shared memory
void unlink_shared_memory(const char *shm_name);

#endif // SHARED_MEMORY_H
