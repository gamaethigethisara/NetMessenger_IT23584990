#define _POSIX_C_SOURCE 200809L
#include <arpa/inet.h>
#include <errno.h>
#include <pthread.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>
#include <fcntl.h>

#define PORT 10990
#define LINE_SIZE 2048
#define FILE_LIMIT (1024UL * 1024UL)

#define ACK_QUEUE_SIZE 128
#define ACK_LINE_SIZE 80
/* Main is the only socket writer. Receiver queues ACKs, never blocks on an
   upload send mutex and never inserts an ACK into an outgoing binary payload. */
typedef struct {
    int fd, wake, status, finished;
    pthread_mutex_t queue_lock;
    char acks[ACK_QUEUE_SIZE][ACK_LINE_SIZE];
    size_t head, count;
} Receiver;

static void wake_main(Receiver *state)
{
    char signal = 'x';
    ssize_t n;
    do { n = write(state->wake, &signal, 1); } while (n < 0 && errno == EINTR);
    /* EAGAIN means the pipe already contains a wakeup; main will drain it. */
}

static int queue_ack(Receiver *state, unsigned long long id, int saved)
{
    pthread_mutex_lock(&state->queue_lock);
    if (state->count == ACK_QUEUE_SIZE) {
        pthread_mutex_unlock(&state->queue_lock);
        fprintf(stderr, "File acknowledgement queue full; closing connection.\n");
        return -1;
    }
    size_t tail = (state->head + state->count) % ACK_QUEUE_SIZE;
    snprintf(state->acks[tail], ACK_LINE_SIZE, "FILEACK %llu %s\n",
             id, saved ? "SAVED" : "SAVE_FAILED");
    ++state->count;
    pthread_mutex_unlock(&state->queue_lock);
    wake_main(state);
    return 0;
}

static int send_all(int fd, const void *buffer, size_t length)
{
    const unsigned char *data = buffer;
    while (length) {
        ssize_t n = send(fd, data, length, MSG_NOSIGNAL);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return -1;
        data += n; length -= (size_t)n;
    }
    return 0;
}

static int safe_component(const char *name, size_t maximum)
{
    size_t n = strlen(name);
    if (!n || n > maximum || name[0] == '.') return 0;
    for (size_t i = 0; i < n; ++i) {
        char c = name[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.')) return 0;
    }
    return 1;
}

static int directory(const char *path)
{
    struct stat st;
    if (mkdir(path, 0700) < 0 && errno != EEXIST) return -1;
    return lstat(path, &st) == 0 && S_ISDIR(st.st_mode) ? 0 : -1;
}

static int read_line(int fd, char *line, size_t capacity)
{
    size_t used = 0;
    for (;;) {
        char c;
        ssize_t n = recv(fd, &c, 1, 0);
        if (n < 0 && errno == EINTR) continue;
        if (!n) return used ? -1 : 0;
        if (n < 0 || !c || used == capacity - 1) return -1;
        if (c == '\n') { line[used] = '\0'; return 1; }
        line[used++] = c;
    }
}

/* Only this thread reads the socket: file bytes cannot be mistaken for chat. */
static void *receive_loop(void *argument)
{
    Receiver *state = argument;
    char line[LINE_SIZE + 256], username[32] = "";
    int bye = 0, result;
    unsigned long long transfer_id = 0;
    while ((result = read_line(state->fd, line, sizeof(line))) == 1) {
        if (!strncmp(line, "FILEID ", 7)) {
            if (transfer_id || !line[7]) { result = -1; break; }
            int valid = 1;
            for (const char *p = line + 7; *p; ++p)
                if (*p < '0' || *p > '9') valid = 0;
            errno = 0;
            transfer_id = strtoull(line + 7, NULL, 10);
            if (!valid || errno || !transfer_id) { result = -1; break; }
            continue;
        }
        if (transfer_id && strncmp(line, "FILE ", 5)) { result = -1; break; }
        if (!strncmp(line, "OK REGISTERED ", 14)) {
            char name[32];
            if (sscanf(line + 14, "%31s", name) == 1 && safe_component(name, 31))
                strcpy(username, name);
        }
        if (!strcmp(line, "OK BYE NID:5849")) bye = 1;
        if (strncmp(line, "FILE ", 5)) { puts(line); continue; }
        char sender[32], filename[128], number[32], extra;
        if (!username[0] || sscanf(line, "FILE %31s %127s %31s %c",
            sender, filename, number, &extra) != 3 ||
            !safe_component(sender, 31) || !safe_component(filename, 127)) {
            result = -1; break;
        }
        char *end;
        errno = 0;
        unsigned long size = strtoul(number, &end, 10);
        if (errno || *end || number[0] < '0' || number[0] > '9' || size > FILE_LIMIT) {
            result = -1; break;
        }
        char parent[64], folder[128], path[512], temporary[512];
        snprintf(parent, sizeof(parent), "received/%s", username);
        snprintf(folder, sizeof(folder), "%s/%s", parent, sender);
        snprintf(path, sizeof(path), "%s/%s", folder, filename);
        snprintf(temporary, sizeof(temporary), "%s/.download-XXXXXX", folder);
        int out = -1, failed = 0;
        if (directory("received") || directory(parent) || directory(folder)) failed = 1;
        else { out = mkstemp(temporary); if (out < 0) failed = 1; }
        struct timeval timeout = {.tv_sec = 15};
        if (setsockopt(state->fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout))) {
            if (out >= 0) { close(out); unlink(temporary); }
            result = -1; break;
        }
        unsigned long left = size;
        while (left) {
            unsigned char buffer[8192];
            size_t amount = left < sizeof(buffer) ? (size_t)left : sizeof(buffer);
            ssize_t n = recv(state->fd, buffer, amount, 0);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) { result = -1; break; }
            left -= (unsigned long)n;
            size_t offset = 0;
            while (!failed && offset < (size_t)n) {
                ssize_t w = write(out, buffer + offset, (size_t)n - offset);
                if (w < 0 && errno == EINTR) continue;
                if (w <= 0) { failed = 1; break; }
                offset += (size_t)w;
            }
        }
        if (out >= 0 && !left && !failed && fsync(out)) failed = 1;
        if (out >= 0 && close(out)) failed = 1;
        if (left || failed) {
            if (out >= 0) unlink(temporary);
            fprintf(stderr, "File receive failed: %s\n", filename);
        } else if (rename(temporary, path)) {
            failed = 1; unlink(temporary); perror("save received file");
        } else printf("Received %lu bytes: %s\n", size, path);
        timeout.tv_sec = 0;
        if (setsockopt(state->fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout))) {
            result = -1; break;
        }
        if (left) break;
        if (transfer_id && queue_ack(state, transfer_id, !failed)) { result = -1; break; }
        transfer_id = 0;
    }
    state->status = bye && result == 0 && !transfer_id ? EXIT_SUCCESS : EXIT_FAILURE;
    if (state->status) fprintf(stderr, "Server disconnected unexpectedly or sent an incomplete frame.\n");
    else puts("Server closed the connection.");
    shutdown(state->fd, SHUT_RDWR);
    pthread_mutex_lock(&state->queue_lock);
    state->finished = 1;
    pthread_mutex_unlock(&state->queue_lock);
    wake_main(state);
    return NULL;
}

/* Local syntax uses a path; the wire header uses basename and exact byte count. */
static int submit(int fd, char *line)
{
    if (!strncmp(line, "SENDFILE ", 9)) {
        char target[64], path[1024], extra;
        if (sscanf(line + 9, "%63s %1023s %c", target, path, &extra) != 2) {
            fprintf(stderr, "Usage: SENDFILE <user-or-#room> <path-without-spaces>\n"); return 0;
        }
        const char *name = strrchr(path, '/'); name = name ? name + 1 : path;
        const char *target_name = target + (target[0] == '#');
        if (!safe_component(name, 127) || !safe_component(target_name, 31)) {
            fprintf(stderr, "Invalid target or filename.\n"); return 0;
        }
        FILE *in = fopen(path, "rb");
        if (!in) { perror("open upload"); return 0; }
        struct stat st;
        if (fstat(fileno(in), &st) || !S_ISREG(st.st_mode) || st.st_size < 0 ||
            (unsigned long long)st.st_size > FILE_LIMIT) {
            fprintf(stderr, "Choose a regular file of at most 1048576 bytes.\n"); fclose(in); return 0;
        }
        size_t size = (size_t)st.st_size;
        unsigned char *data = malloc(size ? size : 1);
        if (!data) { fclose(in); return 0; }
        if (fread(data, 1, size, in) != size) {
            fprintf(stderr, "Could not read the complete upload.\n"); free(data); fclose(in); return 0;
        }
        fclose(in);
        char header[1200];
        snprintf(header, sizeof(header), "SENDFILE %s %s %zu\n", target, name, size);
        int error = send_all(fd, header, strlen(header)) || send_all(fd, data, size);
        free(data);
        return error ? -1 : 0;
    }
    return send_all(fd, line, strlen(line)) || send_all(fd, "\n", 1) ? -1 : 0;
}

int main(int argc, char *argv[])
{
    if (argc != 2) { fprintf(stderr, "Usage: %s <server_ipv4>\n", argv[0]); return EXIT_FAILURE; }
    setvbuf(stdout, NULL, _IOLBF, 0);
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { perror("socket"); return EXIT_FAILURE; }
    struct sockaddr_in server = {0};
    server.sin_family = AF_INET; server.sin_port = htons(PORT);
    if (inet_pton(AF_INET, argv[1], &server.sin_addr) != 1) {
        fprintf(stderr, "Invalid IPv4 address.\n"); close(fd); return EXIT_FAILURE;
    }
    if (connect(fd, (struct sockaddr *)&server, sizeof(server))) {
        perror("connect"); close(fd); return EXIT_FAILURE;
    }
    struct timeval timeout = {.tv_sec = 15};
    if (setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout))) {
        perror("send timeout"); close(fd); return EXIT_FAILURE;
    }
    int wake[2];
    if (pipe(wake)) { perror("pipe"); close(fd); return EXIT_FAILURE; }
    for (int i = 0; i < 2; ++i) {
        int flags = fcntl(wake[i], F_GETFL);
        if (flags == -1 || fcntl(wake[i], F_SETFL, flags | O_NONBLOCK) == -1) {
            perror("nonblocking wake pipe"); close(wake[0]); close(wake[1]); close(fd);
            return EXIT_FAILURE;
        }
    }
    Receiver state = {.fd = fd, .wake = wake[1], .status = EXIT_FAILURE,
                      .queue_lock = PTHREAD_MUTEX_INITIALIZER};
    pthread_t thread;
    int error = pthread_create(&thread, NULL, receive_loop, &state);
    if (error) {
        fprintf(stderr, "pthread_create: %s\n", strerror(error));
        close(wake[0]); close(wake[1]); close(fd); return EXIT_FAILURE;
    }
    printf("Connected to %s:%d\nFirst enter: REGISTER <username>\n", argv[1], PORT);
    puts("Commands: REGISTER LIST BCAST PMSG JOIN LEAVE ROOMS RMSG SENDFILE QUIT");
    puts("Upload: SENDFILE bob /tmp/sample.txt (room: SENDFILE #lab /tmp/sample.txt)");
    puts("Maximum file size: 1048576 bytes. Press Enter after each command.");
    puts("File receipts: SAVED confirms recipient save; TIMEOUT means unconfirmed.");
    struct pollfd events[2] = {{STDIN_FILENO, POLLIN, 0}, {wake[0], POLLIN, 0}};
    char line[LINE_SIZE]; size_t used = 0; int dropping = 0, status = EXIT_SUCCESS;
    /* Negotiate before REGISTER; legacy servers can reject this optional command. */
    if (submit(fd, "CAPS FILE_ACK")) {
        perror("capability negotiation"); status = EXIT_FAILURE;
        shutdown(fd, SHUT_RDWR);
    }
    int quitting = 0;
    for (; status == EXIT_SUCCESS;) {
        int ready = poll(events, 2, -1);
        if (ready < 0 && errno == EINTR) continue;
        if (ready < 0) { perror("poll"); status = EXIT_FAILURE; break; }
        if (events[1].revents) {
            char signals[128];
            while (read(wake[0], signals, sizeof(signals)) > 0) {}
            int finished;
            for (;;) {
                char ack[ACK_LINE_SIZE];
                pthread_mutex_lock(&state.queue_lock);
                finished = state.finished;
                int available = state.count > 0;
                if (available) {
                    strcpy(ack, state.acks[state.head]);
                    state.head = (state.head + 1) % ACK_QUEUE_SIZE;
                    --state.count;
                }
                pthread_mutex_unlock(&state.queue_lock);
                if (finished || !available) break;
                if (!quitting && send_all(fd, ack, strlen(ack))) {
                    perror("send file acknowledgement"); status = EXIT_FAILURE; break;
                }
            }
            if (finished || status != EXIT_SUCCESS) break;
        }
        if (events[0].revents & (POLLIN | POLLHUP)) {
            char c;
            ssize_t n = read(STDIN_FILENO, &c, 1);
            if (n < 0 && errno == EINTR) continue;
            if (n < 0) { perror("stdin"); status = EXIT_FAILURE; break; }
            if (!n) {
                /* EOF requests graceful shutdown; discard an unfinished line. */
                if (submit(fd, "QUIT")) { status = EXIT_FAILURE; break; }
                quitting = 1; events[0].fd = -1; continue;
            }
            if (c == '\n') {
                if (!dropping) {
                    if (used && line[used-1] == '\r') --used;
                    line[used] = '\0';
                    if (used && submit(fd, line)) { perror("send"); status = EXIT_FAILURE; break; }
                    if (!strcmp(line, "QUIT")) { quitting = 1; events[0].fd = -1; }
                } else fprintf(stderr, "Command too long or contains NUL; discarded.\n");
                used = 0; dropping = 0;
            } else if (!c || used >= sizeof(line)-1) dropping = 1;
            else if (!dropping) line[used++] = c;
        }
    }
    shutdown(fd, SHUT_RDWR);
    pthread_join(thread, NULL);
    close(wake[0]); close(wake[1]); close(fd);
    pthread_mutex_destroy(&state.queue_lock);
    return status == EXIT_SUCCESS ? state.status : status;
}
