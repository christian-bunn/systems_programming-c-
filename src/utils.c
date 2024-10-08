// utils.c
#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE
#include "../headers/shared_memory.h"
#include "../headers/utils.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>
#include <signal.h>
#include <time.h>

int is_valid_floor(const char *floor) {
    if (floor == NULL) {
        return 0;
    }
    size_t len = strlen(floor);
    if (len == 0 || len > 4) {
        return 0;
    }

    if (floor[0] == 'B') {
        if (len < 2 || len > 3) {
            return 0;
        }
        for (size_t i = 1; i < len; ++i) {
            if (!isdigit((unsigned char)floor[i])) {
                return 0;
            }
        }
        int floor_num = atoi(&floor[1]);
        if (floor_num < 1 || floor_num > 99) {
            return 0;
        }
    } else {
        for (size_t i = 0; i < len; ++i) {
            if (!isdigit((unsigned char)floor[i])) {
                return 0;
            }
        }
        int floor_num = atoi(floor);
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

int compare_floors(const char *floor1, const char *floor2) {
    // Same as in car.c
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

void get_next_floor_up(const char *current_floor, char *next_floor, const char *highest_floor) {
    // Same as in car.c
    int curr, high;
    int is_b_curr = (current_floor[0] == 'B');
    int is_b_high = (highest_floor[0] == 'B');
    curr = atoi(is_b_curr ? &current_floor[1] : current_floor);
    high = atoi(is_b_high ? &highest_floor[1] : highest_floor);

    if (is_b_curr) curr = -curr;
    if (is_b_high) high = -high;

    if (curr >= high) {
        // Already at highest floor
        strncpy(next_floor, current_floor, FLOOR_STR_SIZE);
        next_floor[FLOOR_STR_SIZE - 1] = '\0';
        return;
    }

    curr += 1;
    if (curr == 0) curr = 1;
    if (curr < 0) {
        snprintf(next_floor, FLOOR_STR_SIZE, "B%d", -curr);
    } else {
        snprintf(next_floor, FLOOR_STR_SIZE, "%d", curr);
    }
}

void get_next_floor_down(const char *current_floor, char *next_floor, const char *lowest_floor) {
    // Same as in car.c
    int curr, low;
    int is_b_curr = (current_floor[0] == 'B');
    int is_b_low = (lowest_floor[0] == 'B');
    curr = atoi(is_b_curr ? &current_floor[1] : current_floor);
    low = atoi(is_b_low ? &lowest_floor[1] : lowest_floor);

    if (is_b_curr) curr = -curr;
    if (is_b_low) low = -low;

    if (curr <= low) {
        // Already at lowest floor
        strncpy(next_floor, current_floor, 4);
        return;
    }

    curr -= 1;
    if (curr == 0) curr = -1;
    if (curr < 0) {
        snprintf(next_floor, FLOOR_STR_SIZE, "B%d", -curr);
    } else {
        snprintf(next_floor, FLOOR_STR_SIZE, "%d", curr);
    }
}

int is_floor_in_range(const char *floor, const char *lowest_floor, const char *highest_floor) {
    if (compare_floors(floor, lowest_floor) >= 0 && compare_floors(floor, highest_floor) <= 0) {
        return 1;
    }
    return 0;
}
