# NetMessenger — IT23584990

IE3010 Network Programming individual assignment. A C TCP client/server application for concurrent messaging, chat rooms and binary file transfer.

## Personalisation

| Item | Value |
| --- | --- |
| Registration number | IT23584990 |
| TCP port | 10990 (6000 + 4990) |
| OK/ERR response suffix | NID:5849 |
| Server / client sources | server_4990.c / client_4990.c |
| Build file | Makefile_4990 |
| Server log | netmsg_IT23584990.log |

## Requirements and build

Developed and tested on CentOS Stream 10 with GCC 14.4.1, GNU Make 4.4.1 and Git 2.52.0. Requires Linux/POSIX sockets and pthreads. Python 3 is needed only for the automated tests.

From the project directory:

```bash
make -f Makefile_4990
```

Both executables are built with `-std=c11 -Wall -Wextra -Wpedantic -g -pthread`.

## Run

Start the server in one terminal, from the project directory:

```bash
./server_4990
```

In each additional terminal, open the same project directory and run:

```bash
./client_4990 127.0.0.1
```

Register a different username in each client. Press Enter after each command and wait for its reply. For another machine, supply the server's IPv4 address; TCP port 10990 must be reachable. The server listens on all IPv4 interfaces.

Example with two clients:

1. Client A: `REGISTER alice`
2. Client B: `REGISTER bob`
3. Client A: `LIST`, then `BCAST Hello everyone`, then `PMSG bob Hello Bob`
4. Both clients: `JOIN lab`
5. Client A: `RMSG lab Hello room`
6. Each client: `QUIT` when finished.

Stop the server with Ctrl+C after the clients exit.

## Client commands

| Command | Behaviour |
| --- | --- |
| `REGISTER alice` | Register a unique, case-sensitive username. |
| `LIST` | List registered users. |
| `BCAST text` | Send to all other registered users. |
| `PMSG bob text` | Send privately to one registered user. |
| `JOIN lab` | Create or join a room; repeated joining is harmless. |
| `LEAVE lab` | Leave a room; delete it when its last member leaves. |
| `ROOMS` | List existing rooms. |
| `RMSG lab text` | Send to other members of a room you have joined. |
| `SENDFILE bob /tmp/sample.txt` | Upload a local file to a user. |
| `SENDFILE #lab /tmp/sample.txt` | Upload to other members of a room you have joined. |
| `QUIT` | Receive an acknowledgement and disconnect. |

The local file must already exist. File paths cannot contain spaces. Prefer `#room` for room transfers: a bare target first matches a username, then a room.

## Protocol and framing

Commands are newline-delimited. Every server `OK` or `ERR` reply ends with ` NID:5849` followed by a newline. Examples:

```text
OK REGISTERED alice NID:5849
OK SENT NID:5849
ERR 001 USERNAME_TAKEN NID:5849
MSG BCAST alice Hello everyone
MSG PRIV alice Hello Bob
MSG ROOM lab alice Hello room
MSG INFO bob JOINED
MSG INFO bob LEFT
```

The client converts its local SENDFILE command into `SENDFILE <target> <filename> <size>\n` followed by exactly `<size>` raw bytes. The server forwards `FILE <sender> <filename> <size>\n` and exactly that many bytes. No separator is added after the payload. Binary data is not interpreted as commands. Partial and combined TCP reads are handled by newline parsing and exact-length payload reception; send loops handle partial writes.

Successful storage and forwarding returns `OK FILE_RECEIVED <filename> NID:5849`. This is not confirmation that the recipient saved the file successfully to disk.

## Storage and logs

All paths are relative to the directory from which the programs are started:

- Server copies: `storage/IT23584990/<sender>/<filename>`
- Client copies: `received/<recipient>/<sender>/<filename>`
- Server log: `netmsg_IT23584990.log`

Complete files are saved using temporary files and rename. A later file with the same sender and filename replaces the earlier copy. Incomplete uploads are not stored. Logs contain timestamps with the local UTC offset, connections, commands, replies, storage/forwarding events and disconnects. Message text is logged; raw file payloads are not.

## Concurrency and cleanup

The server creates one detached pthread per connected client. A shared mutex protects user/room state and serializes outgoing frames so that file bytes and chat messages do not interleave. Upload reception and file storage happen outside that mutex. A separate mutex serializes log records.

The client has one receiver thread that exclusively reads its socket. Its main thread uses poll for keyboard input and receiver completion notification. Disconnect cleanup releases the client's slot, removes room memberships, deletes empty rooms and notifies other registered users.

This model keeps per-client command handling straightforward for the assignment's small client population. Outgoing sends hold the shared server mutex, so a slow recipient can delay other clients. It is not a fully nonblocking server.

## Limits and errors

- Maximum 32 simultaneous connections, including unregistered clients; maximum 32 rooms.
- User/room names: 1–31 ASCII letters, digits, underscores or hyphens; case-sensitive.
- Command lines: at most 2047 bytes before the newline, including command names and arguments. Message capacity depends on the command prefix.
- Files: at most 1,048,576 bytes (1 MiB); filenames 1–127 ASCII letters, digits, underscores, hyphens or dots, with no leading dot.
- Server socket send timeout: 3 seconds; payload receive timeout: 15 seconds. These socket timeouts apply to blocking operations, not a total transfer deadline.
- Oversized or malformed upload headers close the connection after an error. An incomplete payload also closes the connection. Bounded rejected uploads are consumed before the next command is read.
- Errors include 001 duplicate username, 002 unknown user, 003 unknown room, 004 oversized file, 005 invalid command/state, 006 capacity limit and 007 storage/delivery failure.
- Broadcast and room `OK SENT` replies do not guarantee delivery to every recipient.
- No TLS, password authentication, offline message queue or rate limiting is implemented. Registration reserves a name only for the active session.

## Tests

Keep the server running. From the project directory in a separate shell, run these scripts sequentially:

```bash
python3 tests/tcp_framing_test.py
python3 tests/file_framing_test.py
python3 tests/upload_errors_test.py
```

The scripts connect to 127.0.0.1:10990. The file-framing test also checks the server's local storage, so run it from the server's working directory. The command-framing script uses the username `framinguser`; leave that name free.

The tests cover split/combined commands, incomplete next commands, binary upload framing, a command following payload bytes, stored-file equality, rejected-room framing, oversized uploads, incomplete uploads and server responsiveness afterward.

Manual CentOS tests also verified five simultaneous clients, broadcast delivery, private and room messaging, binary room delivery to two recipients, isolation from nonmembers and abrupt disconnect cleanup. See `docs/test_results.md` and the report screenshots for execution evidence.

## Development records

- `docs/design_diary.md`: development milestones and observations.
- `docs/prompt_log.md`: AI assistance summaries.
- `docs/test_results.md`: recorded test results.

## Clean build outputs

```bash
make -f Makefile_4990 clean
```

This removes only the two executables. Stored files, received files and logs remain.
