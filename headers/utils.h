#ifndef UTILS_H
#define UTILS_H

#include <stddef.h>

// Function to validate floor strings
int is_valid_floor(const char *floor);

// Function to validate status strings
int is_valid_status(const char *status);

// Function to sleep for the specified number of milliseconds
void sleep_ms(int milliseconds);

// Function to set up signal handling
void setup_signal_handler(void (*handler)(int));

// Function to compare floors
int compare_floors(const char *floor1, const char *floor2);

// Functions to get next floor up or down
void get_next_floor_up(const char *current_floor, char *next_floor, const char *highest_floor);
void get_next_floor_down(const char *current_floor, char *next_floor, const char *lowest_floor);

// Function to check if a floor is within a range
int is_floor_in_range(const char *floor, const char *lowest_floor, const char *highest_floor);

#endif // UTILS_H
