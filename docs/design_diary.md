# Design Diary — IT23584990

## 6 October 2026 — Setup and messaging

Set up the project on CentOS Stream 10 with GCC, Make and Git.
Personalised the application with TCP port 10990 and response tag
NID:5849. Built and tested the initial TCP connection before adding
registration, user listing and presence notifications.

Used one pthread per client to keep each connection's command handling
straightforward. A mutex protects shared users and rooms. Added
broadcast and private messaging, followed by room joining, leaving,
listing and messaging. Manual tests checked successful delivery,
duplicate usernames, unknown recipients and room membership errors.

## 6–7 October 2026 — File transfer and logging

Added binary file transfer with an explicit byte count and a 1 MiB
limit. Newline parsing handles command headers; exact-length reads
handle payloads. The client uses a dedicated receiver thread so it
can receive messages and files while accepting keyboard input.

Complete uploads are saved on the server and forwarded to recipients.
Temporary files and rename avoid exposing partially written files.
Private and room transfers were checked using cmp against the originals.

Added timestamped logs for connections, commands, responses, file
storage, forwarding and disconnects. A separate mutex keeps log records
from different threads together.

## 7 October 2026 — Verification and review

Verified five simultaneous clients and broadcast delivery to the other
four users. Used ss to confirm the listening port and established
connections. A 4096-byte binary room transfer reached Bob and Carol;
their copies and the server copy matched the original. Nonmembers
Dave and Erin received no file.

Stopped Carol abruptly, checked her removal from LIST, then reconnected
with the same name. A room message was rejected until she rejoined,
confirming that disconnect cleanup removed the old membership.

Ran three Python socket test scripts on CentOS. They passed checks for
split and combined commands, binary payload framing, a command following
file bytes, rejected uploads, oversized files, incomplete uploads and
server responsiveness afterward. These scripts test the C application;
the server and client remain implemented in C.

Updated the README to describe the completed features, build/run steps,
tests and limits. Development changes were committed and pushed in
stages. One remaining design limitation is that outgoing sends hold
the shared server mutex, so a slow recipient can delay other clients.
