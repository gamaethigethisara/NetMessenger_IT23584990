# NetMessenger — IT23584990

IE3010 Network Programming individual assignment. A C TCP client/server application for concurrent messaging, chat rooms and binary file transfer. The file receipt acknowledgement extension reports whether each recipient saved a transferred file, for both private and room transfers.

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
| `SENDFILE bob /tmp/sample.txt` | Upload a local file to a user and receive a save result with receipt-capable clients. |
| `SENDFILE #lab /tmp/sample.txt` | Upload to other members of a room you have joined and receive individual results and a completion summary. |
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

The client converts its local SENDFILE command into `SENDFILE <target> <filename> <size>\n` followed by exactly `<size>` raw bytes. The server forwards `FILE <sender> <filename> <size>\n` and exactly that many bytes. For a tracked transfer to a receipt-capable recipient, a separate `FILEID <id>\n` line precedes the `FILE` header. No separator is added after the payload. Binary data is not interpreted as commands. Partial and combined TCP reads are handled by newline parsing and exact-length payload reception; send loops handle partial writes.

Successful storage and forwarding returns `OK FILE_RECEIVED <filename> NID:5849`. Recipient save confirmation is reported separately through `MSG FILE_RECEIPT`, followed by a `MSG FILE_COMPLETE` summary.

## File receipt acknowledgement

This optional extension distinguishes server acceptance from a recipient's local save result. The sender receives one result per recipient and a final summary. The normal `SENDFILE` syntax remains unchanged.

The updated client automatically negotiates `CAPS FILE_ACK` before registration. After receiving the complete file, it checks file writes, `fsync`, `close` and the final rename before sending `FILEACK <id> SAVED`. If a local save operation fails, it consumes the remaining payload and reports `SAVE_FAILED`. Users do not type `CAPS` or `FILEACK` commands themselves.

With Alice and Bob registered, create a file in a separate shell:

```bash
printf 'NetMessenger receipt example\n' > /tmp/sample.txt
```

Then type this command inside Alice's running client:

```text
SENDFILE bob /tmp/sample.txt
```

Example sender output for a successful save:

```text
OK FILE_TRANSFER 1 sample.txt recipients=1 NID:5849
OK FILE_RECEIVED sample.txt NID:5849
MSG FILE_RECEIPT 1 bob SAVED NID:5849
MSG FILE_COMPLETE 1 saved=1 failed=0 unknown=0 NID:5849
```

Transfer IDs are assigned by the server and may differ from the example. Keep the sender connected until `FILE_COMPLETE` arrives.

For a room transfer, join the same room with each client and use `SENDFILE #lab /tmp/sample.txt`. Each original recipient connection has its own result. For example, one successful save and one local save failure produce `saved=1 failed=1 unknown=0`. The order of recipient results may vary.

| Receipt status | Meaning | Completion count |
| --- | --- | --- |
| `SAVED` | Recipient reports that the complete file was saved successfully. | `saved` |
| `SAVE_FAILED` | Recipient consumed the payload but could not save it. | `failed` |
| `DELIVERY_FAILED` | Server could not complete forwarding to that recipient. | `failed` |
| `TIMEOUT` | No valid acknowledgement was processed within the waiting interval. | `unknown` |
| `DISCONNECTED` | Recipient disconnected before its save was confirmed. | `unknown` |
| `UNSUPPORTED` | Recipient did not negotiate receipt support. | `unknown` |

An `unknown` result means the save could not be confirmed; it does not establish that the file was lost. A room with no other members completes with all three counts at zero.

Receipts are matched to the pending transfer and the original recipient session. Invalid, duplicate and late acknowledgements are rejected. Reconnecting with the same username cannot acknowledge an old transfer. If the sender disconnects, its pending tracking is cancelled.

The original file protocol is retained when the sender does not negotiate receipt support. Legacy recipients still receive the original `FILE` frame, but their save result is `UNSUPPORTED` for a tracked transfer.

See [the extension documentation](docs/file_receipts.md) for the full protocol, implementation details and manual success/failure demonstrations, and [the CentOS verification record](docs/file_receipt_results.md) for observed results.

## Storage and logs

All paths are relative to the directory from which the programs are started:

- Server copies: `storage/IT23584990/<sender>/<filename>`
- Client copies: `received/<recipient>/<sender>/<filename>`
- Server log: `netmsg_IT23584990.log`

Complete files are saved using temporary files and rename. A later file with the same sender and filename replaces the earlier copy. Incomplete uploads are not stored. Logs contain timestamps with the local UTC offset, connections, commands, replies, storage/forwarding events and disconnects. Message text is logged; raw file payloads are not. Receipt events use `FILE_RECEIPT`, completion summaries use `FILE_COMPLETE`, and sender cancellation uses `FILE_TRACKING_CANCELLED`. A committed example is available in [the sample server log](docs/sample_logs/netmsg_IT23584990.log).

## Concurrency and cleanup

The server creates one detached pthread per connected client. A shared mutex protects user/room state and serializes outgoing frames so that file bytes and chat messages do not interleave. Upload reception and file storage happen outside that mutex. A separate mutex serializes log records.

The client has one receiver thread that exclusively reads its socket. Its main thread uses poll for keyboard input and receiver notifications and remains the only socket writer. The receiver queues file acknowledgements and wakes the main thread through a nonblocking pipe. The main thread sends queued acknowledgements after any current upload frame, preventing ACK text from being inserted into file bytes.

Disconnect cleanup releases the client's slot, removes room memberships, deletes empty rooms and notifies other registered users. Pending receipt tracking is also resolved or cancelled as appropriate.

This model keeps per-client command handling straightforward for the assignment's small client population. Outgoing sends hold the shared server mutex, so a slow recipient can delay other clients. It is not a fully nonblocking server.

## Limits and errors

- Maximum 32 simultaneous connections, including unregistered clients; maximum 32 rooms.
- User/room names: 1–31 ASCII letters, digits, underscores or hyphens; case-sensitive.
- Command lines: at most 2047 bytes before the newline, including command names and arguments. Message capacity depends on the command prefix.
- Files: at most 1,048,576 bytes (1 MiB); filenames 1–127 ASCII letters, digits, underscores, hyphens or dots, with no leading dot.
- Server socket send timeout: 3 seconds; payload receive timeout: 15 seconds. These socket timeouts apply to blocking operations, not a total transfer deadline.
- Oversized or malformed upload headers close the connection after an error. An incomplete payload also closes the connection. Bounded rejected uploads are consumed before the next command is read.
- Errors include 001 duplicate username, 002 unknown user, 003 unknown room, 004 oversized file, 005 invalid command/state, 006 capacity limit and 007 storage/delivery failure.
- Receipt tracking holds at most 64 active transfers, with at most 32 recipient entries per transfer. A full tracking table returns `ERR 006 RECEIPT_LIMIT_REACHED` after consuming the bounded upload, without storing or forwarding it.
- The default receipt waiting interval is 30 seconds, starting after the forwarding batch. A background timer checks for expiry even when no new commands arrive; scheduling and serialized sends can delay reporting.
- Receipt state is held in memory and is lost on server restart. `SAVED` is a statement from the recipient program, not cryptographic proof. The extension does not add automatic retries or resumable uploads.
- Broadcast and room `OK SENT` replies do not guarantee delivery to every recipient.
- No TLS, password authentication, offline message queue or rate limiting is implemented. Registration reserves a name only for the active session.

## Tests

### Framing and upload regression tests

Keep the server running. From the project directory in a separate shell, run these scripts sequentially:

```bash
python3 tests/tcp_framing_test.py
python3 tests/file_framing_test.py
python3 tests/upload_errors_test.py
```

The scripts connect to 127.0.0.1:10990. The file-framing test also checks the server's local storage, so run it from the server's working directory. The command-framing script uses the username `framinguser`; leave that name free.

The tests cover split/combined commands, incomplete next commands, binary upload framing, a command following payload bytes, stored-file equality, rejected-room framing, oversized uploads, incomplete uploads and server responsiveness afterward.

Manual CentOS tests also verified five simultaneous clients, broadcast delivery, private and room messaging, binary room delivery to two recipients, isolation from nonmembers and abrupt disconnect cleanup. See `docs/test_results.md` and the report screenshots for execution evidence.

### File receipt tests

Before running the receipt suite, quit the manual clients and stop the manual server so that port 10990 is free. This suite starts its own isolated server and clients. From the project directory, run:

```bash
make -f Makefile_4990
python3 tests/file_receipt_test.py
```

The suite checks 16 groups, including real-client save confirmation and byte equality, zero-byte files, repeated filenames, local save failures, mixed room results, invalid acknowledgements, disconnect/reconnect handling, legacy compatibility, tracking limits, simultaneous bidirectional transfers, timeout handling and incomplete download cleanup. The timeout case intentionally waits about 30 seconds.

Expected final output:

```text
ALL 16 FILE RECEIPT TEST GROUPS PASSED
```

All 16 groups passed in the recorded CentOS run. Separate manual checks covered private delivery, file comparisons using `cmp`, a controlled save failure, mixed room results and receipt log events. See [the file receipt results](docs/file_receipt_results.md) for this evidence.

## Development records

- [Design diary](docs/design_diary.md): development milestones and observations.
- [Prompt log](docs/prompt_log.md): AI assistance summaries.
- [Reflection](docs/reflection.md): learning and evaluation of the development process.
- [Core test results](docs/test_results.md): recorded messaging, framing and upload tests.
- [File receipt documentation](docs/file_receipts.md): extension protocol and manual test instructions.
- [File receipt results](docs/file_receipt_results.md): automated and manual CentOS verification.
- [Sample server log](docs/sample_logs/netmsg_IT23584990.log): an excerpt containing receipt events.

## Clean build outputs

```bash
make -f Makefile_4990 clean
```

This removes only the two executables. Stored files, received files and logs remain.
