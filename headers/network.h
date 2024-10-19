#ifndef NETWORK_H
#define NETWORK_H

#include <stdint.h>

// Networking constants
#define CONTROLLER_IP "127.0.0.1"
#define CONTROLLER_PORT 3000

// Function to create a TCP connection to the controller
int connect_to_controller();

// Function to send a length-prefixed message
int send_message(int sockfd, const char *message);

// Function to receive a length-prefixed message
int receive_message(int sockfd, char **message);

#endif // NETWORK_H
