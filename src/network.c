// network.c

#include "../headers/network.h"
#include "../headers/utils.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <arpa/inet.h>

int connect_to_controller() {
    int sockfd;
    struct sockaddr_in serv_addr;

    sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd == -1) {
        perror("socket");
        return -1;
    }

    // Zero out the server address structure
    memset(&serv_addr, 0, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(CONTROLLER_PORT);

    if (inet_pton(AF_INET, CONTROLLER_IP, &serv_addr.sin_addr) <= 0) {
        perror("inet_pton");
        close(sockfd);
        return -1;
    }

    if (connect(sockfd, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) != 0) {
        perror("connect");
        close(sockfd);
        return -1;
    }

    return sockfd;
}

int send_message(int sockfd, const char *message) {
    uint32_t len = htonl(strlen(message));
    if (write(sockfd, &len, sizeof(len)) != sizeof(len)) {
        return -1;
    }
    if (write(sockfd, message, strlen(message)) != (ssize_t)strlen(message)) {
        return -1;
    }
    return 0;
}

int receive_message(int sockfd, char **message) {
    uint32_t len_net;
    ssize_t n = read(sockfd, &len_net, sizeof(len_net));
    if (n == 0) {
        return -1; // Connection closed
    }
    if (n != sizeof(len_net)) {
        return -1;
    }

    uint32_t len = ntohl(len_net);
    *message = malloc(len + 1);
    if (*message == NULL) {
        return -1;
    }

    size_t total_read = 0;
    while (total_read < len) {
        n = read(sockfd, *message + total_read, len - total_read);
        if (n <= 0) {
            free(*message);
            return -1;
        }
        total_read += n;
    }
    (*message)[len] = '\0';
    return 0;
}
