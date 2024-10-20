#include "../headers/network.h"  // Include the network-related functions (like connecting to the controller)
#include "../headers/call.h"     // Include the call-related functions (validating floors, etc.)
#include "../headers/utils.h"    // Include utility functions (helpers for sending/receiving messages)
#include <stdio.h>               // Standard I/O library for printing messages
#include <stdlib.h>              // Standard library for memory management, exit codes
#include <string.h>              // String library for comparing/manipulating strings
#include <unistd.h>              // UNIX standard library for closing sockets, among other things

// Function to handle making a call request to the elevator system
void run_call(const char *source_floor, const char *destination_floor) {
    int sockfd;                 // Socket file descriptor for the network connection
    char message[256];          // Buffer for the message we'll send to the controller
    char *response = NULL;      // Pointer to hold the response from the controller

    // Check if both the source and destination floors are valid
    if (!is_valid_floor(source_floor) || !is_valid_floor(destination_floor)) {
        printf("Invalid floor(s) specified.\n");
        return;
    }

    // Check if the source and destination floors are the same
    if (strcmp(source_floor, destination_floor) == 0) {
        printf("You are already on that floor!\n");
        return;
    }

    // Try to connect to the elevator system controller
    sockfd = connect_to_controller();
    if (sockfd == -1) {  // If the connection fails
        printf("Unable to connect to elevator system.\n");
        return;
    }

    // Prepare the message to send (format: CALL {source floor} {destination floor})
    snprintf(message, sizeof(message), "CALL %s %s", source_floor, destination_floor);

    // Send the message to the controller
    if (send_message(sockfd, message) != 0) {  // Check if the send operation failed
        printf("Unable to connect to elevator system.\n");
        close(sockfd);  // Close the socket before returning
        return;
    }

    // Receive the response from the controller
    if (receive_message(sockfd, &response) != 0) {  // Check if receiving failed
        printf("Unable to connect to elevator system.\n");
        close(sockfd);  // Close the socket
        return;
    }

    // Process the response
    if (strncmp(response, "CAR ", 4) == 0) {  // If the response starts with "CAR ", a car is coming
        char car_name[32];                   // Buffer for the car name
        sscanf(response + 4, "%31s", car_name);  // Extract the car name from the response
        printf("Car %s is arriving.\n", car_name);
    } else if (strcmp(response, "UNAVAILABLE") == 0) {  // If no cars are available
        printf("Sorry, no car is available to take this request.\n");
    } else {  // If the response is unexpected
        printf("Unexpected response from elevator system.\n");
    }

    // Clean up: free the response memory and close the socket
    free(response);
    close(sockfd);
}

// Main function to handle command-line input and call the run_call function
int main(int argc, char *argv[]) {
    // Check if the user provided the correct number of arguments (source and destination floors)
    if (argc != 3) {
        fprintf(stderr, "Usage: %s {source floor} {destination floor}\n", argv[0]);
        return EXIT_FAILURE;  // Exit with an error code if the arguments are incorrect
    }

    // Call the run_call function with the user-specified source and destination floors
    run_call(argv[1], argv[2]);
    return EXIT_SUCCESS;  // Exit successfully
}
