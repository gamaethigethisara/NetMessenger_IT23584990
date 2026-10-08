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
#include <stdarg.h>
#include <time.h>
#include <limits.h>

#define PORT 10990
#define MAX_CLIENTS 32
#define MAX_ROOMS 32
#define NAME_SIZE 32
#define LINE_SIZE 2048
#define TAG " NID:5849\n"

typedef struct {
    int fd;
    char name[NAME_SIZE];
    unsigned long long session;
    int receipt_capable;
} Client;

static Client clients[MAX_CLIENTS];
static unsigned long long next_session = 1;

typedef struct {
    char name[NAME_SIZE];
    unsigned char members[MAX_CLIENTS];
} Room;

static Room rooms[MAX_ROOMS];
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

#define LOG_PATH "netmsg_IT23584990.log"
static pthread_mutex_t log_lock = PTHREAD_MUTEX_INITIALIZER;

/* Separate lock keeps complete log records together across client threads.
   Control characters are escaped to keep each event on one physical line. */
static void log_event(const char *event, int fd, const char *format, ...)
{
    char detail[4096], stamp[64];
    va_list args;
    va_start(args, format);
    vsnprintf(detail, sizeof(detail), format, args);
    va_end(args);
    pthread_mutex_lock(&log_lock);
    time_t now = time(NULL);
    struct tm local;
    if (localtime_r(&now, &local) == NULL ||
        strftime(stamp, sizeof(stamp), "%Y-%m-%dT%H:%M:%S%z", &local) == 0)
        strcpy(stamp, "TIME_UNAVAILABLE");
    FILE *out = fopen(LOG_PATH, "a");
    if (out == NULL) {
        perror("open log");
    } else {
        int failed = fprintf(out, "[%s] event=%s fd=%d ", stamp, event, fd) < 0;
        for (const unsigned char *p = (const unsigned char *)detail; *p; ++p) {
            if (*p < 32 || *p == 127) {
                if (fprintf(out, "\\x%02X", (unsigned int)*p) < 0) failed = 1;
            } else if (fputc(*p, out) == EOF) failed = 1;
        }
        if (fputc('\n', out) == EOF) failed = 1;
        if (fclose(out) == EOF) failed = 1;
        if (failed) fprintf(stderr, "Could not write a complete log record.\n");
    }
    pthread_mutex_unlock(&log_lock);
}

/* Caller holds lock when accessing a client socket. */
static int send_text(int fd, const char *text)
{
    const char *original = text;
    size_t remaining = strlen(text);

    while (remaining > 0) {
        ssize_t n = send(fd, text, remaining, MSG_NOSIGNAL);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0) {
            log_event("SEND_ERROR", fd, "errno=%d", errno);
            shutdown(fd, SHUT_RDWR);
            return -1;
        }
        text += n;
        remaining -= (size_t)n;
    }
    log_event("SENT", fd, "%s", original);
    return 0;
}

/* Optional FILE_ACK extension. All records and session lookups use lock.
   IDs are unique within this server run; no IDs are reused on overflow. */
#define MAX_TRANSFERS 64
#ifndef ACK_TIMEOUT_SECONDS
#define ACK_TIMEOUT_SECONDS 30
#endif
#if ACK_TIMEOUT_SECONDS < 1
#error ACK_TIMEOUT_SECONDS must be positive
#endif

typedef enum {
    RECEIPT_PENDING, RECEIPT_SAVED, RECEIPT_SAVE_FAILED,
    RECEIPT_DISCONNECTED, RECEIPT_DELIVERY_FAILED,
    RECEIPT_UNSUPPORTED, RECEIPT_TIMEOUT
} ReceiptStatus;

typedef struct {
    unsigned long long session;
    char name[NAME_SIZE];
    int capable, reported;
    ReceiptStatus status;
    struct timespec deadline;
} Receipt;

typedef struct {
    int active, ready, count;
    unsigned long long id, sender_session;
    char sender[NAME_SIZE], filename[128];
    Receipt recipients[MAX_CLIENTS];
} Transfer;

static Transfer transfers[MAX_TRANSFERS];
static unsigned long long next_transfer = 1;

static Client *find_session(unsigned long long session)
{
    for (int i = 0; i < MAX_CLIENTS; ++i)
        if (clients[i].fd != -1 && clients[i].session == session)
            return &clients[i];
    return NULL;
}

static const char *receipt_status(ReceiptStatus status)
{
    static const char *names[] = {
        "PENDING", "SAVED", "SAVE_FAILED", "DISCONNECTED",
        "DELIVERY_FAILED", "UNSUPPORTED", "TIMEOUT"
    };
    return names[status];
}

static int expired(const struct timespec *deadline, const struct timespec *now)
{
    return now->tv_sec > deadline->tv_sec ||
           (now->tv_sec == deadline->tv_sec && now->tv_nsec >= deadline->tv_nsec);
}

/* Report each terminal result once, then release the bounded tracking slot.
   TIMEOUT/DISCONNECTED/UNSUPPORTED mean unknown, never proof of failed save. */
static void publish_receipts(Transfer *transfer)
{
    if (!transfer->active || !transfer->ready) return;
    Client *sender = find_session(transfer->sender_session);
    if (!sender) { transfer->active = 0; return; }
    char text[512];
    int pending = 0, saved = 0, failed = 0, unknown = 0;
    for (int i = 0; i < transfer->count; ++i) {
        Receipt *receipt = &transfer->recipients[i];
        if (receipt->status == RECEIPT_PENDING) { ++pending; continue; }
        if (receipt->status == RECEIPT_SAVED) ++saved;
        else if (receipt->status == RECEIPT_SAVE_FAILED ||
                 receipt->status == RECEIPT_DELIVERY_FAILED) ++failed;
        else ++unknown;
        if (!receipt->reported) {
            receipt->reported = 1;
            snprintf(text, sizeof(text), "MSG FILE_RECEIPT %llu %s %s" TAG,
                     transfer->id, receipt->name, receipt_status(receipt->status));
            send_text(sender->fd, text);
            log_event("FILE_RECEIPT", sender->fd, "id=%llu sender=%s recipient=%s status=%s",
                      transfer->id, transfer->sender, receipt->name,
                      receipt_status(receipt->status));
        }
    }
    if (!pending) {
        snprintf(text, sizeof(text),
                 "MSG FILE_COMPLETE %llu saved=%d failed=%d unknown=%d" TAG,
                 transfer->id, saved, failed, unknown);
        send_text(sender->fd, text);
        log_event("FILE_COMPLETE", sender->fd,
                  "id=%llu saved=%d failed=%d unknown=%d", transfer->id, saved, failed, unknown);
        transfer->active = 0;
    }
}

static void expire_receipts(void)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now)) return;
    for (int i = 0; i < MAX_TRANSFERS; ++i) {
        Transfer *transfer = &transfers[i];
        if (!transfer->active || !transfer->ready) continue;
        for (int j = 0; j < transfer->count; ++j) {
            Receipt *receipt = &transfer->recipients[j];
            if (receipt->status == RECEIPT_PENDING && expired(&receipt->deadline, &now))
                receipt->status = RECEIPT_TIMEOUT;
        }
        publish_receipts(transfer);
    }
}

static void *receipt_timer(void *unused)
{
    (void)unused;
    for (;;) {
        struct timespec delay = {.tv_sec = 0, .tv_nsec = 200000000L};
        while (nanosleep(&delay, &delay) && errno == EINTR) {}
        pthread_mutex_lock(&lock);
        expire_receipts();
        pthread_mutex_unlock(&lock);
    }
    return NULL;
}

static Transfer *reserve_transfer(Client *sender, const char *filename)
{
    expire_receipts();
    if (!next_transfer) return NULL;
    for (int i = 0; i < MAX_TRANSFERS; ++i) {
        if (transfers[i].active) continue;
        Transfer *transfer = &transfers[i];
        memset(transfer, 0, sizeof(*transfer));
        transfer->active = 1;
        transfer->id = next_transfer++;
        transfer->sender_session = sender->session;
        strcpy(transfer->sender, sender->name);
        strcpy(transfer->filename, filename);
        return transfer;
    }
    return NULL;
}

static int parse_id(const char *text, unsigned long long *id)
{
    if (!*text) return 0;
    for (const char *p = text; *p; ++p) if (*p < '0' || *p > '9') return 0;
    errno = 0;
    *id = strtoull(text, NULL, 10);
    return !errno && *id != 0;
}

/* Only the original recipient connection can acknowledge a pending transfer. */
static void handle_file_ack(Client *client, const char *line)
{
    char number[32], status[32], extra;
    unsigned long long id;
    if (!client->receipt_capable ||
        sscanf(line, "FILEACK %31s %31s %c", number, status, &extra) != 2 ||
        !parse_id(number, &id) || (strcmp(status, "SAVED") && strcmp(status, "SAVE_FAILED"))) {
        send_text(client->fd, "ERR 005 INVALID_FILE_ACK" TAG); return;
    }
    expire_receipts();
    for (int i = 0; i < MAX_TRANSFERS; ++i) {
        Transfer *transfer = &transfers[i];
        if (!transfer->active || !transfer->ready || transfer->id != id) continue;
        for (int j = 0; j < transfer->count; ++j) {
            Receipt *receipt = &transfer->recipients[j];
            if (receipt->session != client->session || !receipt->capable ||
                receipt->status != RECEIPT_PENDING) continue;
            receipt->status = !strcmp(status, "SAVED") ? RECEIPT_SAVED : RECEIPT_SAVE_FAILED;
            publish_receipts(transfer);
            return;
        }
    }
    send_text(client->fd, "ERR 005 INVALID_FILE_ACK" TAG);
}

static void disconnect_receipts(Client *client)
{
    for (int i = 0; i < MAX_TRANSFERS; ++i) {
        Transfer *transfer = &transfers[i];
        if (!transfer->active || !transfer->ready) continue;
        if (transfer->sender_session == client->session) {
            log_event("FILE_TRACKING_CANCELLED", client->fd,
                      "id=%llu reason=SENDER_DISCONNECTED", transfer->id);
            transfer->active = 0; continue;
        }
        for (int j = 0; j < transfer->count; ++j) {
            Receipt *receipt = &transfer->recipients[j];
            if (receipt->session == client->session && receipt->status == RECEIPT_PENDING)
                receipt->status = RECEIPT_DISCONNECTED;
        }
        publish_receipts(transfer);
    }
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
        if (n <= 0) {
            log_event("UPLOAD_INCOMPLETE", fd, "user=%s file=%s received=%zu expected=%lu errno=%d",
                      client->name, filename, used, size, n < 0 ? errno : 0);
            free(data); return -1;
        }
        used += (size_t)n;
    }
    timeout.tv_sec = 0;
    if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout))) {
        free(data); return -1;
    }

    /* Snapshot recipients; a reused slot must not receive an old transfer. */
    int recipients[MAX_CLIENTS], count = 0;
    Receipt snapshots[MAX_CLIENTS] = {0};
    Transfer *transfer = NULL;
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
                        else {
                            recipients[count] = copy;
                            snapshots[count].session = clients[i].session;
                            snapshots[count].capable = clients[i].receipt_capable;
                            strcpy(snapshots[count].name, clients[i].name);
                            ++count;
                        }
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
                        recipients[count] = copy;
                        snapshots[count].session = clients[i].session;
                        snapshots[count].capable = clients[i].receipt_capable;
                        strcpy(snapshots[count].name, clients[i].name);
                        ++count;
                    }
                }
            }
        }
    }
    if (!error && client->receipt_capable) {
        transfer = reserve_transfer(client, filename);
        if (!transfer) error = "ERR 006 RECEIPT_LIMIT_REACHED" TAG;
        else {
            transfer->count = count;
            memcpy(transfer->recipients, snapshots, (size_t)count * sizeof(Receipt));
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
    if (!error) log_event("FILE_STORED", fd, "user=%s target=%s path=%s bytes=%lu",
                          client->name, target, path, size);
    /* Serialize header + raw bytes with all other outgoing frames.
       Upload reception and disk IO above do not hold the shared mutex. */
    pthread_mutex_lock(&lock);
    if (!error) {
        char header[512];
        if (transfer) {
            snprintf(header, sizeof(header), "OK FILE_TRANSFER %llu %s recipients=%d" TAG,
                     transfer->id, filename, count);
            send_text(fd, header);
        }
        for (int i = 0; i < count; ++i) {
            Receipt *receipt = transfer ? &transfer->recipients[i] : NULL;
            int delivery_failed = 0;
            if (receipt && !find_session(receipt->session)) {
                receipt->status = RECEIPT_DISCONNECTED;
                error = "ERR 007 DELIVERY_FAILED" TAG;
                continue;
            }
            if (receipt && receipt->capable) {
                /* This metadata and the original FILE frame are serialized together. */
                snprintf(header, sizeof(header), "FILEID %llu\n", transfer->id);
                delivery_failed = send_bytes(recipients[i], header, strlen(header));
            }
            snprintf(header, sizeof(header), "FILE %s %s %lu\n", client->name, filename, size);
            if (!delivery_failed)
                delivery_failed = send_bytes(recipients[i], header, strlen(header)) ||
                                  send_bytes(recipients[i], data, size);
            if (delivery_failed) error = "ERR 007 DELIVERY_FAILED" TAG;
            if (receipt) {
                if (delivery_failed) receipt->status = RECEIPT_DELIVERY_FAILED;
                else if (!receipt->capable) receipt->status = RECEIPT_UNSUPPORTED;
            }
        }
        if (!error) {
            snprintf(header, sizeof(header), "OK FILE_RECEIVED %s" TAG, filename);
            send_text(fd, header);
        }
        if (transfer) {
            /* ACK handlers cannot acquire lock during room forwarding. Start
               the waiting interval after that batch, not before a slow peer. */
            struct timespec deadline;
            int clock_failed = clock_gettime(CLOCK_MONOTONIC, &deadline);
            if (!clock_failed) deadline.tv_sec += ACK_TIMEOUT_SECONDS;
            for (int i = 0; i < count; ++i) {
                Receipt *receipt = &transfer->recipients[i];
                if (receipt->status != RECEIPT_PENDING) continue;
                if (clock_failed) receipt->status = RECEIPT_TIMEOUT;
                else receipt->deadline = deadline;
            }
            transfer->ready = 1;
        }
    }
    if (transfer) {
        if (transfer->ready) publish_receipts(transfer);
        else transfer->active = 0;  /* Storage failed before any forwarding. */
    }
    log_event(error ? "FILE_FAILED" : "FILE_FORWARDED", fd,
              "user=%s target=%s file=%s bytes=%lu recipients=%d result=%s",
              client->name, target, filename, size, count, error ? error : "OK");
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
        log_event("COMMAND", fd, "user=%s command=%s",
                  client->name[0] ? client->name : "unregistered", line);
        if (strncmp(line, "SENDFILE ", 9) == 0) {
            if (receive_upload(client, line) < 0) { result = 0; break; }
            continue;
        }
        int finished = 0;
        pthread_mutex_lock(&lock);

        if (strcmp(line, "QUIT") == 0) {
            send_text(fd, "OK BYE" TAG);
            finished = 1;
        } else if (strcmp(line, "CAPS FILE_ACK") == 0) {
            client->receipt_capable = 1;
            send_text(fd, "OK CAPS FILE_ACK" TAG);
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

        } else if (!strncmp(line, "FILEACK ", 8) || !strcmp(line, "FILEACK")) {
            handle_file_ack(client, line);
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

    disconnect_receipts(client);
    log_event("DISCONNECT", fd, "user=%s memberships_cleared=1",
              client->name[0] ? client->name : "unregistered");
    close(fd);
    client->fd = -1;
    client->name[0] = '\0';
    pthread_mutex_unlock(&lock);
    return NULL;
}

int main(void)
{
    struct timespec clock_check;
    if (clock_gettime(CLOCK_MONOTONIC, &clock_check)) {
        perror("monotonic clock"); return EXIT_FAILURE;
    }
    FILE *check_log = fopen(LOG_PATH, "a");
    if (!check_log) { perror("open log"); return EXIT_FAILURE; }
    if (fclose(check_log)) { perror("close log"); return EXIT_FAILURE; }
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

    pthread_t timer;
    int timer_error = pthread_create(&timer, NULL, receipt_timer, NULL);
    if (timer_error) {
        fprintf(stderr, "receipt timer: %s\n", strerror(timer_error));
        close(listener); return EXIT_FAILURE;
    }
    pthread_detach(timer);
    log_event("START", listener, "student=IT23584990 port=%d", PORT);
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

        log_event("CONNECT", fd, "accepted=1");
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

        if (slot == NULL || next_session == 0) {
            send_text(fd, "ERR 006 SERVER_FULL" TAG);
            close(fd);
        } else {
            slot->fd = fd;
            slot->name[0] = '\0';
            slot->session = next_session++;
            slot->receipt_capable = 0;
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
