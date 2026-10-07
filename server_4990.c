#define _POSIX_C_SOURCE 200809L
#include <arpa/inet.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#include <sys/stat.h>
#include <fcntl.h>

#define PORT 10990
#define MAX_CLIENTS 32
#define MAX_ROOMS 32
#define NAME_SIZE 32
#define LINE_SIZE 2048
#define TAG " NID:5849\n"

typedef struct {
    int fd;
    char name[NAME_SIZE];
} Client;

static Client clients[MAX_CLIENTS];
typedef struct {
    char name[NAME_SIZE];
    unsigned char members[MAX_CLIENTS];
} Room;

static Room rooms[MAX_ROOMS];
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

/* Room helpers are called with the shared state lock held.
   Empty rooms are removed; a later JOIN can recreate them. */
static int find_room(const char *name)
{
    for (int i = 0; i < MAX_ROOMS; ++i)
        if (rooms[i].name[0] != '\0' &&
            strcmp(rooms[i].name, name) == 0)
            return i;
    return -1;
}

static void remove_member(int room, int member)
{
    rooms[room].members[member] = 0;
    for (int i = 0; i < MAX_CLIENTS; ++i)
        if (rooms[room].members[i])
            return;
    memset(&rooms[room], 0, sizeof(rooms[room]));
}

/* Return 1 when this function has handled the command. */
static int handle_room_command(Client *client, char *line)
{
    int member = (int)(client - clients);
    int fd = client->fd;
    char response[MAX_ROOMS * NAME_SIZE + 64];

    if (strcmp(line, "ROOMS") == 0) {
        strcpy(response, "OK ROOMS ");
        int first = 1;
        for (int i = 0; i < MAX_ROOMS; ++i) {
            if (rooms[i].name[0] == '\0')
                continue;
            if (!first)
                strcat(response, ",");
            strcat(response, rooms[i].name);
            first = 0;
        }
        strcat(response, TAG);
        send_text(fd, response);
        return 1;
    }

    int joining = strncmp(line, "JOIN ", 5) == 0;
    int leaving = strncmp(line, "LEAVE ", 6) == 0;
    if (joining || leaving) {
        const char *name = line + (joining ? 5 : 6);
        if (!valid_name(name)) {
            send_text(fd, "ERR 005 INVALID_ROOM_NAME" TAG);
            return 1;
        }
        int room = find_room(name);
        if (joining) {
            if (room == -1) {
                for (int i = 0; i < MAX_ROOMS; ++i) {
                    if (rooms[i].name[0] == '\0') {
                        room = i;
                        memset(&rooms[i], 0, sizeof(rooms[i]));
                        strcpy(rooms[i].name, name);
                        break;
                    }
                }
            }
            if (room == -1) {
                send_text(fd, "ERR 006 ROOM_LIMIT_REACHED" TAG);
                return 1;
            }
            /* Repeated JOIN is harmless: membership is a flag. */
            rooms[room].members[member] = 1;
            snprintf(response, sizeof(response), "OK JOINED %s" TAG, name);
        } else {
            if (room == -1) {
                send_text(fd, "ERR 003 ROOM_NOT_FOUND" TAG);
                return 1;
            }
            if (!rooms[room].members[member]) {
                send_text(fd, "ERR 005 NOT_IN_ROOM" TAG);
                return 1;
            }
            remove_member(room, member);
            snprintf(response, sizeof(response), "OK LEFT %s" TAG, name);
        }
        send_text(fd, response);
        return 1;
    }

    if (strncmp(line, "RMSG ", 5) == 0) {
        char *name = line + 5;
        char *separator = strchr(name, ' ');
        if (separator == NULL || separator == name || separator[1] == '\0') {
            send_text(fd, "ERR 005 INVALID_FORMAT" TAG);
            return 1;
        }
        *separator = '\0';
        if (!valid_name(name)) {
            send_text(fd, "ERR 005 INVALID_ROOM_NAME" TAG);
            return 1;
        }
        int room = find_room(name);
        if (room == -1) {
            send_text(fd, "ERR 003 ROOM_NOT_FOUND" TAG);
        } else if (!rooms[room].members[member]) {
            send_text(fd, "ERR 005 NOT_IN_ROOM" TAG);
        } else {
            char outgoing[LINE_SIZE + 2 * NAME_SIZE + 32];
            snprintf(outgoing, sizeof(outgoing), "MSG ROOM %s %s %s\n",
                     name, client->name, separator + 1);
            for (int i = 0; i < MAX_CLIENTS; ++i) {
                if (i != member && rooms[room].members[i] &&
                    clients[i].fd != -1 && clients[i].name[0] != '\0')
                    send_text(clients[i].fd, outgoing);
            }
            send_text(fd, "OK SENT" TAG);
        }
        return 1;
    }
    if (strcmp(line, "JOIN") == 0 || strcmp(line, "LEAVE") == 0 ||
        strcmp(line, "RMSG") == 0 || strncmp(line, "ROOMS ", 6) == 0) {
        send_text(fd, "ERR 005 INVALID_FORMAT" TAG);
        return 1;
    }
    return 0;
}


#define FILE_LIMIT (1024UL * 1024UL)

/* File names are single safe path components, never paths. */
static int valid_file(const char *name)
{
    size_t n = strlen(name);
    if (!n || n > 127 || name[0] == '.') return 0;
    for (size_t i = 0; i < n; ++i) {
        char c = name[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.'))
            return 0;
    }
    return 1;
}

static int ensure_directory(const char *path)
{
    struct stat st;
    if (mkdir(path, 0700) == -1 && errno != EEXIST) return -1;
    return lstat(path, &st) == 0 && S_ISDIR(st.st_mode) ? 0 : -1;
}

static int send_bytes(int fd, const void *buffer, size_t size)
{
    const unsigned char *p = buffer;
    while (size) {
        ssize_t n = send(fd, p, size, MSG_NOSIGNAL);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { shutdown(fd, SHUT_RDWR); return -1; }
        p += n; size -= (size_t)n;
    }
    return 0;
}

/* A complete frame is consumed even when its target is invalid.
   Unbounded/ambiguous frames are rejected and the connection is closed. */
static int receive_upload(Client *client, char *line)
{
    char target[64], filename[128], number[32], extra;
    unsigned long size = 0;
    const char *error = NULL;
    int fd = client->fd;
    if (sscanf(line, "SENDFILE %63s %127s %31s %c",
               target, filename, number, &extra) != 3) {
        pthread_mutex_lock(&lock);
        send_text(fd, "ERR 005 INVALID_FORMAT" TAG);
        pthread_mutex_unlock(&lock);
        return -1;
    }
    for (size_t i = 0; number[i]; ++i) {
        if (number[i] < '0' || number[i] > '9') {
            pthread_mutex_lock(&lock);
            send_text(fd, "ERR 005 INVALID_FORMAT" TAG);
            pthread_mutex_unlock(&lock);
            return -1;
        }
    }
    errno = 0;
    size = strtoul(number, NULL, 10);
    if (errno || size > FILE_LIMIT) {
        pthread_mutex_lock(&lock);
        send_text(fd, "ERR 004 FILE_TOO_LARGE" TAG);
        pthread_mutex_unlock(&lock);
        return -1;
    }
    unsigned char *data = malloc(size ? size : 1);
    if (!data) return -1;
    struct timeval timeout = {.tv_sec = 15};
    if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout))) {
        free(data); return -1;
    }
    size_t used = 0;
    while (used < size) {
        ssize_t n = recv(fd, data + used, size - used, 0);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { free(data); return -1; }
        used += (size_t)n;
    }
    timeout.tv_sec = 0;
    if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout))) {
        free(data); return -1;
    }

    /* Snapshot recipients; a reused slot must not receive an old transfer. */
    int recipients[MAX_CLIENTS], count = 0;
    pthread_mutex_lock(&lock);
    if (!client->name[0]) error = "ERR 005 REGISTER_REQUIRED" TAG;
    else if (!valid_file(filename)) error = "ERR 005 INVALID_FILENAME" TAG;
    else {
        int room_only = target[0] == '#';
        const char *name = target + room_only;
        if (!valid_name(name)) error = "ERR 005 INVALID_FORMAT" TAG;
        else {
            if (!room_only) {
                for (int i = 0; i < MAX_CLIENTS; ++i)
                    if (clients[i].fd != -1 && !strcmp(clients[i].name, name)) {
                        int copy = dup(clients[i].fd);
                        if (copy < 0) error = "ERR 007 DELIVERY_FAILED" TAG;
                        else recipients[count++] = copy;
                        break;
                    }
            }
            if (!count && !error) {
                int room = find_room(name);
                if (room < 0) error = room_only ? "ERR 003 ROOM_NOT_FOUND" TAG :
                                                               "ERR 002 USER_NOT_FOUND" TAG;
                else if (!rooms[room].members[client - clients])
                    error = "ERR 005 NOT_IN_ROOM" TAG;
                else for (int i = 0; i < MAX_CLIENTS; ++i) {
                    if (&clients[i] != client && rooms[room].members[i] && clients[i].fd != -1) {
                        int copy = dup(clients[i].fd);
                        if (copy < 0) { error = "ERR 007 DELIVERY_FAILED" TAG; break; }
                        recipients[count++] = copy;
                    }
                }
            }
        }
    }
    pthread_mutex_unlock(&lock);

    char directory[256], path[512], temporary[512];
    temporary[0] = '\0';
    if (!error) {
        snprintf(directory, sizeof(directory), "storage/IT23584990/%s", client->name);
        snprintf(path, sizeof(path), "%s/%s", directory, filename);
        snprintf(temporary, sizeof(temporary), "%s/.upload-XXXXXX", directory);
        if (ensure_directory("storage") || ensure_directory("storage/IT23584990") ||
            ensure_directory(directory)) error = "ERR 007 STORAGE_FAILED" TAG;
        else {
            int out = mkstemp(temporary);
            if (out < 0) error = "ERR 007 STORAGE_FAILED" TAG;
            else {
                size_t written = 0;
                while (written < size) {
                    ssize_t n = write(out, data + written, size - written);
                    if (n < 0 && errno == EINTR) continue;
                    if (n <= 0) { error = "ERR 007 STORAGE_FAILED" TAG; break; }
                    written += (size_t)n;
                }
                if (close(out)) error = "ERR 007 STORAGE_FAILED" TAG;
                if (!error && rename(temporary, path)) error = "ERR 007 STORAGE_FAILED" TAG;
                if (error) unlink(temporary);
            }
        }
    }
    /* Serialize header + raw bytes with all other outgoing frames.
       Upload reception and disk IO above do not hold the shared mutex. */
    pthread_mutex_lock(&lock);
    if (!error) {
        char header[512];
        snprintf(header, sizeof(header), "FILE %s %s %lu\n", client->name, filename, size);
        for (int i = 0; i < count; ++i)
            if (send_bytes(recipients[i], header, strlen(header)) ||
                send_bytes(recipients[i], data, size)) error = "ERR 007 DELIVERY_FAILED" TAG;
        if (!error) {
            snprintf(header, sizeof(header), "OK FILE_RECEIVED %s" TAG, filename);
            send_text(fd, header);
        }
    }
    if (error) send_text(fd, error);
    pthread_mutex_unlock(&lock);
    for (int i = 0; i < count; ++i) close(recipients[i]);
    free(data);
    return 0;
}

static void *serve_client(void *argument)
{
    Client *client = argument;
    int fd = client->fd;
    char line[LINE_SIZE];
    int result;

    while ((result = read_line(fd, line, sizeof(line))) == 1) {
        if (strncmp(line, "SENDFILE ", 9) == 0) {
            if (receive_upload(client, line) < 0) { result = 0; break; }
            continue;
        }
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

        } else if (handle_room_command(client, line)) {
            /* The room handler already sent the response. */
        } else if (strncmp(line, "BCAST ", 6) == 0) {
            const char *message = line + 6;

            if (*message == '\0') {
                send_text(fd, "ERR 005 INVALID_FORMAT" TAG);
            } else {
                char outgoing[LINE_SIZE + NAME_SIZE + 32];
                snprintf(outgoing, sizeof(outgoing),
                         "MSG BCAST %s %s\n",
                         client->name, message);

                for (int i = 0; i < MAX_CLIENTS; ++i) {
                    if (&clients[i] != client &&
                        clients[i].fd != -1 &&
                        clients[i].name[0] != '\0') {
                        send_text(clients[i].fd, outgoing);
                    }
                }
                send_text(fd, "OK SENT" TAG);
            }

        } else if (strncmp(line, "PMSG ", 5) == 0) {
            char *target = line + 5;
            char *separator = strchr(target, ' ');

            if (separator == NULL || separator == target ||
                separator[1] == '\0') {
                send_text(fd, "ERR 005 INVALID_FORMAT" TAG);
            } else {
                *separator = '\0';
                const char *message = separator + 1;
                Client *recipient = NULL;

                if (!valid_name(target)) {
                    send_text(fd, "ERR 005 INVALID_FORMAT" TAG);
                } else {
                    for (int i = 0; i < MAX_CLIENTS; ++i) {
                        if (clients[i].fd != -1 &&
                            clients[i].name[0] != '\0' &&
                            strcmp(clients[i].name, target) == 0) {
                            recipient = &clients[i];
                            break;
                        }
                    }

                    if (recipient == NULL) {
                        send_text(fd, "ERR 002 USER_NOT_FOUND" TAG);
                    } else {
                        char outgoing[LINE_SIZE + NAME_SIZE + 32];
                        snprintf(outgoing, sizeof(outgoing),
                                 "MSG PRIV %s %s\n",
                                 client->name, message);

                        if (send_text(recipient->fd, outgoing) == 0)
                            send_text(fd, "OK SENT" TAG);
                        else
                            send_text(fd, "ERR 007 DELIVERY_FAILED" TAG);
                    }
                }
            }
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

    for (int i = 0; i < MAX_ROOMS; ++i)
        remove_member(i, (int)(client - clients));

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
