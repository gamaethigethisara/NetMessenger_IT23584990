#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <unistd.h>

#define SERVER_PORT 10990

int main(int argc, char *argv[])
{
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <server_ipv4>\n", argv[0]);
        return EXIT_FAILURE;
    }

    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd == -1) {
        perror("socket");
        return EXIT_FAILURE;
    }

    struct sockaddr_in server = {0};
    server.sin_family = AF_INET;
    server.sin_port = htons(SERVER_PORT);

    if (inet_pton(AF_INET, argv[1], &server.sin_addr) != 1) {
        fprintf(stderr, "Invalid IPv4 address: %s\n", argv[1]);
        close(sockfd);
        return EXIT_FAILURE;
    }

    if (connect(sockfd, (struct sockaddr *)&server,
                sizeof(server)) == -1) {
        perror("connect");
        close(sockfd);
        return EXIT_FAILURE;
    }

    printf("Connected to %s:%d\n", argv[1], SERVER_PORT);

    close(sockfd);
    printf("Connection test complete.\n");
    return EXIT_SUCCESS;
}
