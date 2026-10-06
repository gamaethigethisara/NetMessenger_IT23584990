#include <arpa/inet.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#define PORT 10990
#define MAX_CLIENTS 32
#define NAME_SIZE 32
#define LINE_SIZE 2048
#define TAG " NID:5849\n"

typedef struct {
    int fd;
    char name[NAME_SIZE];
} Client;

static Client clients[MAX_CLIENTS];
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

/* Caller holds lock when accessing a client socket. */
static int send_text(int fd, const char *text)
{
    size_t remaining = strlen(text);

    while (remaining > 0) {
        ssize_t n = send(fd, text, remaining, MSG_NOSIGNAL);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0) {
            shutdown(fd, SHUT_RDWR);
            return -1;
        }
        text += n;
        remaining -= (size_t)n;
    }
    return 0;
}

/* Read exactly one newline-delimited command.
   Bytes belonging to the next command remain in the socket. */
static int read_line(int fd, char *line, size_t capacity)
{
    size_t used = 0;

    for (;;) {
        char ch;
        ssize_t n = recv(fd, &ch, 1, 0);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            return 0;

        if (ch == '\n') {
            if (used > 0 && line[used - 1] == '\r')
                --used;
            line[used] = '\0';
            return 1;
        }

        if (ch == '\0' || used == capacity - 1)
            return -1;

        line[used++] = ch;
    }
}

static int valid_name(const char *name)
{
    size_t length = strlen(name);
    if (length == 0 || length >= NAME_SIZE)
        return 0;

    for (size_t i = 0; i < length; ++i) {
        char c = name[i];
        if (!((c >= 'a' && c <= 'z') ||
              (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') ||
              c == '_' || c == '-'))
            return 0;
    }
    return 1;
}

/* Presence notification format chosen for this implementation. */
static void notify_others(Client *sender, const char *event)
{
    char message[128];
    snprintf(message, sizeof(message), "MSG INFO %s %s\n",
             sender->name, event);

    for (int i = 0; i < MAX_CLIENTS; ++i) {
        if (&clients[i] != sender &&
            clients[i].fd != -1 && clients[i].name[0] != '\0')
            send_text(clients[i].fd, message);
    }
}

static void *serve_client(void *argument)
{
    Client *client = argument;
    int fd = client->fd;
    char line[LINE_SIZE];
    int result;

    while ((result = read_line(fd, line, sizeof(line))) == 1) {
        int finished = 0;
        pthread_mutex_lock(&lock);

        if (strcmp(line, "QUIT") == 0) {
            send_text(fd, "OK BYE" TAG);
            finished = 1;
        } else if (strncmp(line, "REGISTER ", 9) == 0) {
            const char *name = line + 9;
            int taken = 0;

            for (int i = 0; i < MAX_CLIENTS; ++i) {
                if (clients[i].fd != -1 &&
                    strcmp(clients[i].name, name) == 0)
                    taken = 1;
            }

            if (client->name[0] != '\0') {
                send_text(fd, "ERR 005 ALREADY_REGISTERED" TAG);
            } else if (!valid_name(name)) {
                send_text(fd, "ERR 005 INVALID_USERNAME" TAG);
            } else if (taken) {
                send_text(fd, "ERR 001 USERNAME_TAKEN" TAG);
            } else {
                strcpy(client->name, name);
                char response[128];
                snprintf(response, sizeof(response),
                         "OK REGISTERED %s" TAG, name);
                send_text(fd, response);
                notify_others(client, "JOINED");
                printf("Registered: %s\n", name);
                fflush(stdout);
            }
        } else if (client->name[0] == '\0') {
            send_text(fd, "ERR 005 REGISTER_REQUIRED" TAG);
        } else if (strcmp(line, "LIST") == 0) {
            char response[MAX_CLIENTS * NAME_SIZE + 64];
            strcpy(response, "OK USERS ");
            int first = 1;

            for (int i = 0; i < MAX_CLIENTS; ++i) {
                if (clients[i].fd != -1 &&
                    clients[i].name[0] != '\0') {
                    if (!first)
                        strcat(response, ",");
                    strcat(response, clients[i].name);
                    first = 0;
                }
            }
            strcat(response, TAG);
            send_text(fd, response);
        } else {
            send_text(fd, "ERR 005 INVALID_COMMAND" TAG);
        }

        pthread_mutex_unlock(&lock);
        if (finished)
            break;
    }

    pthread_mutex_lock(&lock);

    if (result == -1)
        send_text(fd, "ERR 005 INVALID_LINE" TAG);

    if (client->name[0] != '\0') {
        notify_others(client, "LEFT");
        printf("Disconnected: %s\n", client->name);
        fflush(stdout);
    }

    close(fd);
    client->fd = -1;
    client->name[0] = '\0';
    pthread_mutex_unlock(&lock);
    return NULL;
}

int main(void)
{
    for (int i = 0; i < MAX_CLIENTS; ++i)
        clients[i].fd = -1;

    int listener = socket(AF_INET, SOCK_STREAM, 0);
    if (listener == -1) {
        perror("socket");
        return EXIT_FAILURE;
    }

    int reuse = 1;
    if (setsockopt(listener, SOL_SOCKET, SO_REUSEADDR,
                   &reuse, sizeof(reuse)) == -1) {
        perror("setsockopt");
        close(listener);
        return EXIT_FAILURE;
    }

    struct sockaddr_in address = {0};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(PORT);

    if (bind(listener, (struct sockaddr *)&address,
             sizeof(address)) == -1) {
        perror("bind");
        close(listener);
        return EXIT_FAILURE;
    }

    if (listen(listener, MAX_CLIENTS) == -1) {
        perror("listen");
        close(listener);
        return EXIT_FAILURE;
    }

    printf("NetMessenger - IT23584990\n");
    printf("Listening on 0.0.0.0:%d\n", PORT);
    fflush(stdout);

    for (;;) {
        int fd = accept(listener, NULL, NULL);
        if (fd == -1) {
            if (errno == EINTR)
                continue;
            perror("accept");
            break;
        }

        struct timeval timeout = {.tv_sec = 3, .tv_usec = 0};
        if (setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO,
                       &timeout, sizeof(timeout)) == -1) {
            perror("send timeout");
            close(fd);
            continue;
        }

        pthread_mutex_lock(&lock);
        Client *slot = NULL;

        for (int i = 0; i < MAX_CLIENTS; ++i) {
            if (clients[i].fd == -1) {
                slot = &clients[i];
                break;
            }
        }

        if (slot == NULL) {
            send_text(fd, "ERR 006 SERVER_FULL" TAG);
            close(fd);
        } else {
            slot->fd = fd;
            slot->name[0] = '\0';
            pthread_t thread;
            int error = pthread_create(&thread, NULL,
                                       serve_client, slot);
            if (error != 0) {
                fprintf(stderr, "pthread_create: %s\n",
                        strerror(error));
                close(fd);
                slot->fd = -1;
            } else {
                pthread_detach(thread);
            }
        }
        pthread_mutex_unlock(&lock);
    }

    close(listener);
    return EXIT_FAILURE;
}
