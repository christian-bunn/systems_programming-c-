// call.c

#include "../headers/network.h"
#include "../headers/call.h"
#include "../headers/utils.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

void run_call(const char *source_floor, const char *destination_floor) {
    int sockfd;
    char message[256];
    char *response = NULL;

    if (!is_valid_floor(source_floor) || !is_valid_floor(destination_floor)) {
        printf("Invalid floor(s) specified.\n");
        return;
    }

    if (strcmp(source_floor, destination_floor) == 0) {
        printf("You are already on that floor!\n");
        return;
    }

    sockfd = connect_to_controller();
    if (sockfd == -1) {
        printf("Unable to connect to elevator system.\n");
        return;
    }

    // Send CALL message
    snprintf(message, sizeof(message), "CALL %s %s", source_floor, destination_floor);
    if (send_message(sockfd, message) != 0) {
        printf("Unable to connect to elevator system.\n");
        close(sockfd);
        return;
    }

    // Receive response
    if (receive_message(sockfd, &response) != 0) {
        printf("Unable to connect to elevator system.\n");
        close(sockfd);
        return;
    }

    if (strncmp(response, "CAR ", 4) == 0) {
        // Elevator dispatched
        char car_name[32];
        sscanf(response + 4, "%31s", car_name);
        printf("Car %s is arriving.\n", car_name);
    } else if (strcmp(response, "UNAVAILABLE") == 0) {
        printf("Sorry, no car is available to take this request.\n");
    } else {
        printf("Unexpected response from elevator system.\n");
    }

    free(response);
    close(sockfd);
}

int main(int argc, char *argv[]) {
    if (argc != 3) {
        fprintf(stderr, "Usage: %s {source floor} {destination floor}\n", argv[0]);
        return EXIT_FAILURE;
    }

    run_call(argv[1], argv[2]);
    return EXIT_SUCCESS;
}
