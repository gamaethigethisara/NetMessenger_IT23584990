#include <arpa/inet.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

#define PORT 10990
#define BUFFER_SIZE 2048

static int send_all(int fd, const char *data, size_t length)
{
    while (length > 0) {
        ssize_t n = send(fd, data, length, MSG_NOSIGNAL);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            return -1;

        data += n;
        length -= (size_t)n;
    }
    return 0;
}

int main(int argc, char *argv[])
{
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <server_ipv4>\n", argv[0]);
        return EXIT_FAILURE;
    }

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd == -1) {
        perror("socket");
        return EXIT_FAILURE;
    }

    if (fd >= FD_SETSIZE) {
        fprintf(stderr, "Socket descriptor exceeds select limit.\n");
        close(fd);
        return EXIT_FAILURE;
    }

    struct sockaddr_in server = {0};
    server.sin_family = AF_INET;
    server.sin_port = htons(PORT);

    if (inet_pton(AF_INET, argv[1], &server.sin_addr) != 1) {
        fprintf(stderr, "Invalid IPv4 address: %s\n", argv[1]);
        close(fd);
        return EXIT_FAILURE;
    }

    if (connect(fd, (struct sockaddr *)&server,
                sizeof(server)) == -1) {
        perror("connect");
        close(fd);
        return EXIT_FAILURE;
    }

    printf("Connected to %s:%d\n", argv[1], PORT);
    printf("First enter: REGISTER <username>\n");
    printf("Available commands: REGISTER, LIST, QUIT\n");
    fflush(stdout);

    int input_open = 1;
    int status = EXIT_SUCCESS;

    for (;;) {
        fd_set readers;
        FD_ZERO(&readers);
        FD_SET(fd, &readers);

        if (input_open)
            FD_SET(STDIN_FILENO, &readers);

        int highest = fd > STDIN_FILENO ? fd : STDIN_FILENO;
        int ready = select(highest + 1, &readers, NULL, NULL, NULL);

        if (ready == -1) {
            if (errno == EINTR)
                continue;
            perror("select");
            status = EXIT_FAILURE;
            break;
        }

        if (FD_ISSET(fd, &readers)) {
            char buffer[BUFFER_SIZE];
            ssize_t n = recv(fd, buffer, sizeof(buffer), 0);

            if (n == 0) {
                printf("Server closed the connection.\n");
                break;
            }
            if (n < 0) {
                if (errno == EINTR)
                    continue;
                perror("recv");
                status = EXIT_FAILURE;
                break;
            }

            if (fwrite(buffer, 1, (size_t)n, stdout) != (size_t)n) {
                fprintf(stderr, "Could not display server output.\n");
                status = EXIT_FAILURE;
                break;
            }
            fflush(stdout);
        }

        if (input_open && FD_ISSET(STDIN_FILENO, &readers)) {
            char buffer[BUFFER_SIZE];
            ssize_t n = read(STDIN_FILENO, buffer, sizeof(buffer));

            if (n < 0) {
                if (errno == EINTR)
                    continue;
                perror("read");
                status = EXIT_FAILURE;
                break;
            }

            if (n == 0) {
                input_open = 0;
                if (shutdown(fd, SHUT_WR) == -1) {
                    perror("shutdown");
                    status = EXIT_FAILURE;
                    break;
                }
            } else if (send_all(fd, buffer, (size_t)n) == -1) {
                perror("send");
                status = EXIT_FAILURE;
                break;
            }
        }
    }

    close(fd);
    return status;
}
