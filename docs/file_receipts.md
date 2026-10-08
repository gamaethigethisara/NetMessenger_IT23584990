# File receipt acknowledgement extension

## Purpose

The original `OK FILE_RECEIVED <filename>` means the server accepted and stored
the upload and completed its forwarding attempts successfully. It does not prove
that another client saved the file. The extension adds a separate recipient
acknowledgement and an individual result for every recipient of a room transfer.

The user-facing upload syntax remains:

```text
SENDFILE bob /tmp/sample.txt
SENDFILE #lab /tmp/sample.txt
```

## Protocol additions

All control lines end in `\n`. File payloads remain exact-length raw bytes.

| Direction | Line | Meaning |
| --- | --- | --- |
| Client → server | `CAPS FILE_ACK` | Opt in to this extension; the updated client sends this automatically before registration |
| Server → client | `OK CAPS FILE_ACK NID:5849` | Capability accepted |
| Server → sender | `OK FILE_TRANSFER 12 sample.txt recipients=1 NID:5849` | Identifies the tracked upload and its recipient count |
| Server → recipient | `FILEID 12` | Transfer ID; immediately followed by the unchanged `FILE` header and its bytes |
| Recipient → server | `FILEACK 12 SAVED` | Recipient reports successful save |
| Recipient → server | `FILEACK 12 SAVE_FAILED` | Entire payload consumed, but local save failed |
| Server → sender | `MSG FILE_RECEIPT 12 bob SAVED NID:5849` | One recipient's terminal result |
| Server → sender | `MSG FILE_COMPLETE 12 saved=1 failed=0 unknown=0 NID:5849` | All recipients now have terminal results |

Example forwarded frame:

```text
FILEID 12\n
FILE alice sample.txt 5\n
hello
```

Here the payload is exactly the five bytes `hello`; it has no extra newline.
The ID is not part of the file content. A room transfer uses one transfer ID and
one receipt per original recipient connection. IDs increase during a server run
and may restart after the server restarts.

If the sender does not negotiate the extension, the original protocol is used.
If a tracked transfer includes a recipient without receipt capability, that
recipient gets the original `FILE` frame and the sender sees `UNSUPPORTED`.
There is no fabricated `SAVED` confirmation for legacy clients.

## Results and completion counts

| Status | Meaning | Summary bucket |
| --- | --- | --- |
| `SAVED` | Recipient reported a complete successful local save | `saved` |
| `SAVE_FAILED` | Recipient consumed the payload but could not save it | `failed` |
| `DELIVERY_FAILED` | Server could not finish forwarding that recipient's frame | `failed` |
| `TIMEOUT` | No valid ACK was processed before the waiting interval expired | `unknown` |
| `DISCONNECTED` | Recipient connection ended before confirmation | `unknown` |
| `UNSUPPORTED` | Recipient did not negotiate receipt support | `unknown` |

Unknown results do not prove a failed save: a recipient may have saved the file
and then lost its connection before its ACK arrived. An empty room has zero
recipients and therefore completes with all three counts at zero.

## Implementation

The server assigns every connection an internal session number. A receipt must
come from the original recipient session, refer to a pending transfer ID, and
contain a supported status. Unknown, forged, duplicate and late ACKs are rejected
with `ERR 005 INVALID_FILE_ACK`. Reconnecting with the same username does not
restore an old session. A sender disconnect cancels its outstanding tracking.

The server retains at most 64 active transfer records, each with at most 32
recipient entries. If the tracking table is full, it consumes the bounded upload
payload and responds `ERR 006 RECEIPT_LIMIT_REACHED` without storing or forwarding
that upload. Terminal records are released. Receipts are logged as `FILE_RECEIPT`,
summaries as `FILE_COMPLETE`, and sender cancellations as `FILE_TRACKING_CANCELLED`.

A background timer checks receipts every 200 ms using a monotonic clock. The
default waiting interval is 30 seconds, starting after the forwarding batch, so
ACK handlers are not expected to run while that batch holds the shared lock.
The timer works even if no new commands arrive. Scheduling and the existing
serialized socket writes can delay reporting; this is not a real-time deadline.

The receiver writes to a temporary file, checks all writes, flushes its file
contents with `fsync`, checks `close`, and renames the completed temporary file
to the final path. Only then does it queue `SAVED`. A local storage error drains
the remaining payload and queues `SAVE_FAILED`. An interrupted payload removes
the temporary file and closes the connection without a success ACK.

The client main thread remains the only socket writer. The receiver thread puts
ACK lines into a bounded queue and wakes the main thread through a nonblocking
pipe. Main sends the queued ACK after any current complete upload frame. This
prevents an ACK from appearing inside outgoing file bytes and keeps the receiver
free to drain simultaneous incoming transfers. Queue exhaustion closes the
connection rather than silently discarding acknowledgements.

## Manual CentOS demonstration

### 1. Successful private transfer

From the project folder, start the server in terminal 1:

```bash
./server_4990
```

In terminal 2, start Alice, then type the registration command inside the client:

```bash
./client_4990 127.0.0.1
```

```text
REGISTER alice
```

In terminal 3, start another client and register Bob:

```bash
./client_4990 127.0.0.1
```

```text
REGISTER bob
```

In a separate shell (not inside a running client), create a test file:

```bash
printf 'NetMessenger receipt test IT23584990\n' > /tmp/receipt_demo.txt
```

In Alice's client:

```text
SENDFILE bob /tmp/receipt_demo.txt
```

Expected Alice output, with an ID chosen by the server:

```text
OK FILE_TRANSFER 1 receipt_demo.txt recipients=1 NID:5849
OK FILE_RECEIVED receipt_demo.txt NID:5849
MSG FILE_RECEIPT 1 bob SAVED NID:5849
MSG FILE_COMPLETE 1 saved=1 failed=0 unknown=0 NID:5849
```

Bob should print a `Received ... bytes` line. In the project shell, if both
clients were started in that folder, verify the bytes:

```bash
cmp /tmp/receipt_demo.txt storage/IT23584990/alice/receipt_demo.txt && echo 'SERVER COPY MATCHES'
cmp /tmp/receipt_demo.txt received/bob/alice/receipt_demo.txt && echo 'BOB COPY MATCHES'
```

### 2. Controlled save failure

Use a separate temporary working directory for a third client. A regular file
named `received` deliberately prevents that client from creating its receive
directory. Existing project files are not moved or deleted.

In a new shell:

```bash
receipt_fail_dir=$(mktemp -d /tmp/netmsg-receipt-fail.XXXXXX)
printf 'Deliberate test blocker\n' > "$receipt_fail_dir/received"
cd "$receipt_fail_dir"
"$HOME/Desktop/NetMessenger_IT23584990/client_4990" 127.0.0.1
```

In this client:

```text
REGISTER failuser
```

In Alice's client:

```text
SENDFILE failuser /tmp/receipt_demo.txt
```

Expected receipt and summary:

```text
MSG FILE_RECEIPT 2 failuser SAVE_FAILED NID:5849
MSG FILE_COMPLETE 2 saved=0 failed=1 unknown=0 NID:5849
```

The actual ID can differ. Type `LIST` in `failuser` to show that the connection
still parses commands correctly after draining the failed file transfer.

### 3. Mixed room results

Type `JOIN receiptlab` in Alice, Bob and failuser. Then in Alice:

```text
SENDFILE #receiptlab /tmp/receipt_demo.txt
```

Expected outcomes: Bob `SAVED`, failuser `SAVE_FAILED`, then
`saved=1 failed=1 unknown=0`. Recipient notification order may vary.

### 4. Automated edge cases and logs

Quit the clients and stop the manual server before running:

```bash
python3 tests/file_receipt_test.py
```

The script starts its own isolated server, tests forged/duplicate/late ACKs,
disconnect/reconnect, legacy compatibility, bounded tracking, simultaneous
uploads, an idle timeout and an incomplete download. To inspect the log from
your manual demonstration, run from the project folder:

```bash
grep -E 'event=FILE_(RECEIPT|COMPLETE|TRACKING_CANCELLED)' netmsg_IT23584990.log | tail -20
```

Capture the sender/receiver outputs, comparisons, mixed-room summary and the
automated test's final line as evidence of your own VM run. Explain the feature's
purpose and demonstrate its behaviour against the assignment's extension criteria.

## Scope and limitations

Receipts are application-level statements from the recipient program, not a
cryptographic proof that an untrusted client saved a file. This extension does
not add user authentication, encryption, checksums, automatic retry, resumable
uploads or offline delivery. `fsync` is applied to file contents; parent-directory
durability across power loss is not promised. Same-name received files keep the
existing overwrite behaviour. Tracking is in memory and does not survive a
server restart. A sender should remain connected until it sees `FILE_COMPLETE`.
