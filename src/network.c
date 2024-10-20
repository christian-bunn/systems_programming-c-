#include "network.h"   // Include network-related function declarations
#include <stdio.h>      // Standard I/O library for printing error messages
#include <stdlib.h>     // Standard library for memory allocation and process control
#include <string.h>     // String manipulation functions
#include <unistd.h>     // UNIX standard library for system calls (like close())
#include <fcntl.h>      // File control options (used for socket manipulation)
#include <arpa/inet.h>  // Functions for internet operations (like inet_pton)

// Function to establish a connection to the elevator controller
int connect_to_controller() {
    int sockfd;  // Socket file descriptor
    struct sockaddr_in serv_addr;  // Structure to hold the server's address

    // Create a socket for communication over IPv4 and TCP
    sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd == -1) {  // Check if socket creation failed
        perror("socket");  // Print an error message
        return -1;         // Return an error code
    }

    // Zero out the serv_addr structure and set the connection parameters
    memset(&serv_addr, 0, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;        // Set the address family to IPv4
    serv_addr.sin_port = htons(CONTROLLER_PORT);  // Set the port number, converting to network byte order

    // Convert the controller's IP address from text to binary form
    if (inet_pton(AF_INET, CONTROLLER_IP, &serv_addr.sin_addr) <= 0) {
        perror("inet_pton");  // Print an error message if the IP address conversion fails
        close(sockfd);        // Close the socket
        return -1;            // Return an error code
    }

    // Attempt to connect to the controller
    if (connect(sockfd, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) != 0) {
        close(sockfd);  // Close the socket if the connection fails
        return -1;      // Return an error code
    }

    return sockfd;  // Return the socket file descriptor if successful
}

// Function to send a message over a socket
int send_message(int sockfd, const char *message) {
    // Get the length of the message and convert it to network byte order
    uint32_t len = htonl(strlen(message));

    // Send the length of the message first
    if (write(sockfd, &len, sizeof(len)) != sizeof(len)) {
        return -1;  // Return an error if the length couldn't be sent
    }

    // Send the actual message
    if (write(sockfd, message, strlen(message)) != (ssize_t)strlen(message)) {
        return -1;  // Return an error if the message couldn't be sent completely
    }

    return 0;  // Return success
}

// Function to receive a message from a socket
int receive_message(int sockfd, char **message) {
    uint32_t len_net;  // Variable to hold the length of the incoming message (in network byte order)

    // Read the length of the message first
    ssize_t n = read(sockfd, &len_net, sizeof(len_net));
    if (n == 0) {
        return -1;  // Return an error if the connection was closed
    }
    if (n != sizeof(len_net)) {
        return -1;  // Return an error if we couldn't read the full length
    }

    // Convert the length from network byte order to host byte order
    uint32_t len = ntohl(len_net);

    // Allocate memory for the message (including space for the null terminator)
    *message = malloc(len + 1);
    if (*message == NULL) {
        return -1;  // Return an error if memory allocation failed
    }

    // Read the message data in a loop (since it may arrive in fragments)
    size_t total_read = 0;
    while (total_read < len) {
        n = read(sockfd, *message + total_read, len - total_read);  // Read a portion of the message
        if (n <= 0) {
            free(*message);  // Free the allocated memory if an error occurs
            return -1;       // Return an error
        }
        total_read += n;  // Update the number of bytes read
    }

    (*message)[len] = '\0';  // Null-terminate the message

    return 0;  // Return success
}
