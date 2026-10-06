#include <arpa/inet.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <unistd.h>

#define SERVER_PORT 10990
#define BACKLOG 10

int main(void)
{
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd == -1) {
        perror("socket");
        return EXIT_FAILURE;
    }

    int reuse = 1;
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR,
                   &reuse, sizeof(reuse)) == -1) {
        perror("setsockopt");
        close(server_fd);
        return EXIT_FAILURE;
    }

    struct sockaddr_in address = {0};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(SERVER_PORT);

    if (bind(server_fd, (struct sockaddr *)&address,
             sizeof(address)) == -1) {
        perror("bind");
        close(server_fd);
        return EXIT_FAILURE;
    }

    if (listen(server_fd, BACKLOG) == -1) {
        perror("listen");
        close(server_fd);
        return EXIT_FAILURE;
    }

    printf("NetMessenger - IT23584990\n");
    printf("Listening on 0.0.0.0:%d\n", SERVER_PORT);
    fflush(stdout);

    for (;;) {
        struct sockaddr_in peer = {0};
        socklen_t peer_length = sizeof(peer);

        int client_fd = accept(server_fd,
                               (struct sockaddr *)&peer,
                               &peer_length);

        if (client_fd == -1) {
            if (errno == EINTR) {
                continue;
            }
            perror("accept");
            close(server_fd);
            return EXIT_FAILURE;
        }

        char peer_ip[INET_ADDRSTRLEN];
        if (inet_ntop(AF_INET, &peer.sin_addr,
                      peer_ip, sizeof(peer_ip)) != NULL) {
            printf("Connection accepted from %s:%u\n",
                   peer_ip, (unsigned)ntohs(peer.sin_port));
            fflush(stdout);
        }

        /* Initial scaffold: command handling will be added next. */
        close(client_fd);
    }
}
