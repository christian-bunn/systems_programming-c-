/*
Note: My get_next_floor_up and get_next_floor_down functions used in this file have been implemented in utils.c and declared in utils.h.
      This allows for them to be shared across components.
*/

#include "../headers/shared_memory.h"
#include "../headers/utils.h"
#include "../headers/internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <fcntl.h>
#include <sys/mman.h>

void print_error(const char *message, car_shared_mem *shared_mem) {
    printf("%s\n", message);
    pthread_mutex_unlock(&shared_mem->mutex); // Unlock mutex
    exit(EXIT_FAILURE);
}

int main(int argc, char *argv[]) {
	if (argc != 3) {
		fprintf(stderr, "Usage: ./internal {car name} {operation}\n");
		exit(EXIT_FAILURE);
	}

	char *car_name = argv[1];
	char *operation = argv[2];
	char shm_name[64];
	snprintf(shm_name, sizeof(shm_name), "/car%s", car_name);

	int fd = shm_open(shm_name, O_RDWR, 0666);
	if (fd == -1) {
		printf("Unable to access car %s.\n", car_name);
		exit(EXIT_FAILURE);
	}

	car_shared_mem *shared_mem = mmap(NULL, sizeof(car_shared_mem), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (shared_mem == MAP_FAILED) {
		printf("Unable to access car %s.\n", car_name);
		exit(EXIT_FAILURE);
	}

	pthread_mutex_lock(&shared_mem->mutex);

	if (strcmp(operation, "open") == 0) {
		shared_mem->open_button = 1;
	} else if (strcmp(operation, "close") == 0) {
		shared_mem->close_button = 1;
	} else if (strcmp(operation, "stop") == 0) {
		shared_mem->emergency_stop = 1;
	} else if (strcmp(operation, "service_on") == 0) {
		shared_mem->individual_service_mode = 1;
		shared_mem->emergency_mode = 0;
	} else if (strcmp(operation, "service_off") == 0) {
		shared_mem->individual_service_mode = 0;
	} else if (strcmp(operation, "up") == 0 || strcmp(operation, "down") == 0) {
		if (shared_mem->individual_service_mode == 0) {
			print_error("Operation only allowed in service mode.", shared_mem);
		}
		if (strcmp(shared_mem->status, "Closed") != 0) {
			print_error("Operation not allowed while doors are open.", shared_mem);
		}
		if (strcmp(shared_mem->status, "Between") == 0) {
			print_error("Operation not allowed while elevator is moving.", shared_mem);
		}
		// set destination floor
		char next_floor[4];
		if (strcmp(operation, "up") == 0) {
			// alculate next floor up
			int floor_num;
			if (shared_mem->current_floor[0] == 'B') {
				floor_num = -atoi(shared_mem->current_floor + 1);
				floor_num++;
				if (floor_num == 0) {
					strcpy(next_floor, "1");
				} else if (floor_num > 0) {
					snprintf(next_floor, sizeof(next_floor), "%d", floor_num);
				} else {
					snprintf(next_floor, sizeof(next_floor), "B%d", -floor_num);
				}
			} else {
				floor_num = atoi(shared_mem->current_floor);
				floor_num++;
				snprintf(next_floor, sizeof(next_floor), "%d", floor_num);
			}
		} else {
			// alculate next floor down
			int floor_num;
			if (shared_mem->current_floor[0] == 'B') {
				floor_num = -atoi(shared_mem->current_floor + 1);
				floor_num--;
				snprintf(next_floor, sizeof(next_floor), "B%d", -floor_num);
			} else {
				floor_num = atoi(shared_mem->current_floor);
				floor_num--;
				if (floor_num == 0) {
					strcpy(next_floor, "B1");
				} else if (floor_num > 0) {
					snprintf(next_floor, sizeof(next_floor), "%d", floor_num);
				} else {
					snprintf(next_floor, sizeof(next_floor), "B%d", -floor_num);
				}
			}
		}
		// heck if next_floor is within elevator's range
		if (compare_floors(next_floor, shared_mem->current_floor) == 0 ||
		    (compare_floors(next_floor, shared_mem->destination_floor) == 0)) {
			print_error("Cannot move beyond elevator's range.", shared_mem);
		}
		strcpy(shared_mem->destination_floor, next_floor);
	} else {
		print_error("Invalid operation.", shared_mem);
	}

	pthread_cond_broadcast(&shared_mem->cond);
	pthread_mutex_unlock(&shared_mem->mutex);

	return 0;
}
