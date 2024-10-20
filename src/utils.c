#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE
#include "utils.h"
#include "shared_memory.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>
#include <signal.h>
#include <time.h>

int is_valid_floor(const char *floor) {
    if (floor == NULL || strlen(floor) == 0 || strlen(floor) >= FLOOR_STR_SIZE) {
        return 0;
    }
    size_t len = strlen(floor);
    if (len == 0 || len >= FLOOR_STR_SIZE) {
        return 0;
    }

    int floor_num = 0;
    if (floor[0] == 'B') {
        if (len < 2 || len > 4) { // B1 to B99
            return 0;
        }
        for (size_t i = 1; i < len; ++i) {
            if (!isdigit((unsigned char)floor[i])) {
                return 0;
            }
        }
        floor_num = atoi(&floor[1]);
        if (floor_num < 1 || floor_num > 99) {
            return 0;
        }
    } else {
        if (len > 3) { // 1 to 999
            return 0;
        }
        for (size_t i = 0; i < len; ++i) {
            if (!isdigit((unsigned char)floor[i])) {
                return 0;
            }
        }
        floor_num = atoi(floor);
        if (floor_num < 1 || floor_num > 999) {
            return 0;
        }
    }
    return 1;
}

int is_valid_status(const char *status) {
    if (status == NULL) {
        return 0;
    }
    if (strcmp(status, "Opening") == 0 ||
        strcmp(status, "Open") == 0 ||
        strcmp(status, "Closing") == 0 ||
        strcmp(status, "Closed") == 0 ||
        strcmp(status, "Between") == 0) {
        return 1;
    }
    return 0;
}

void sleep_ms(int milliseconds) {
    usleep(milliseconds * 1000);
}

void setup_signal_handler(void (*handler)(int)) {
    struct sigaction sa;
    sa.sa_handler = handler;
    sa.sa_flags = 0;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);
}

int floor_to_int(const char *floor) {
    if (floor == NULL || !is_valid_floor(floor)) {
        return 0;
    }
    if (floor[0] == 'B') {
        return -atoi(floor + 1);
    } else {
        return atoi(floor);
    }
}

void int_to_floor(int floor_int, char *floor_str) {
    if (floor_str == NULL) {
        return;
    }
    if (floor_int < 0) {
        snprintf(floor_str, FLOOR_STR_SIZE, "B%d", -floor_int);
    } else {
        snprintf(floor_str, FLOOR_STR_SIZE, "%d", floor_int);
    }
}

int compare_floors(const char *floor1, const char *floor2) {
    int f1 = floor_to_int(floor1);
    int f2 = floor_to_int(floor2);

    // printf("Comparing floors: %s (%d) and %s (%d)\n", floor1, f1, floor2, f2);  // Debug logging for floor comparison

    if (f1 < f2) return -1;
    if (f1 > f2) return 1;
    return 0;
}

void get_next_floor_up(const char *current_floor, char *next_floor, const char *highest_floor) {
    int curr = floor_to_int(current_floor);
    int high = floor_to_int(highest_floor);

    // printf("Getting next floor up from %s (current: %d, highest: %d)\n", current_floor, curr, high);  // Debug

    if (curr >= high) {
        strncpy(next_floor, current_floor, FLOOR_STR_SIZE);
        next_floor[FLOOR_STR_SIZE - 1] = '\0';
        return;
    }
    curr++;
    int_to_floor(curr, next_floor);
}

void get_next_floor_down(const char *current_floor, char *next_floor, const char *lowest_floor) {
    int curr = floor_to_int(current_floor);
    int low = floor_to_int(lowest_floor);

    // printf("Getting next floor down from %s (current: %d, lowest: %d)\n", current_floor, curr, low);  // Debug

    if (curr <= low) {
        strncpy(next_floor, current_floor, FLOOR_STR_SIZE);
        next_floor[FLOOR_STR_SIZE - 1] = '\0';
        return;
    }
    curr--;
    int_to_floor(curr, next_floor);
}

int is_floor_in_range(const char *floor, const char *lowest_floor, const char *highest_floor) {
    int f = floor_to_int(floor);
    int low = floor_to_int(lowest_floor);
    int high = floor_to_int(highest_floor);
    return (f >= low && f <= high);
}
