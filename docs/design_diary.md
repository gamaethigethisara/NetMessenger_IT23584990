# Design Diary - IT23584990

## 2026-10-06 - Initial TCP connection

Set up the project on CentOS Stream 10 using GCC, GNU Make and Git.
Recorded the personalised TCP port 10990 and response tag NID:5849
in the README.

Implemented an initial TCP server using socket, bind, listen and
accept. Implemented a client using socket and connect. The server
currently accepts each connection and closes it immediately.

Validation:
- Both programs compiled without displayed warnings or errors.
- The client connected to 127.0.0.1:10990.
- The server displayed the accepted connection.
- Both programs rebuilt successfully using Makefile_4990.

Added the compiled executable names to .gitignore and committed
the source files and Makefile.

Next:
Keep connections open, implement newline-delimited command parsing,
and support multiple clients with REGISTER, LIST and QUIT.

## Registration and presence milestone
Implemented one thread per client and mutex-protected shared user data.
Added newline-delimited command reading, complete-send handling,
REGISTER, LIST, QUIT and join/leave notifications.
Updated the client to monitor keyboard and socket input using select.

Observed tests:
- Alice and Bob remained registered simultaneously.
- LIST returned alice,bob.
- Alice received Bob's JOINED and LEFT notifications.
- Bob received OK BYE NID:5849 before disconnection.
- Observed OK responses included the correct NID:5849 suffix.

Duplicate-name rejection and five simultaneous clients still need testing.

## Broadcast and private messaging milestone
Added BCAST to send a message to other registered clients.
Added PMSG to find a registered recipient and deliver a private message.

Observed tests:
- Alice's broadcast reached Bob and Alice received OK SENT.
- Bob's private message reached Alice and Bob received OK SENT.
- PMSG to nobody returned ERR 002 USER_NOT_FOUND.
- These OK and ERR replies included NID:5849.
- The server compiled without displayed warnings or errors.
- Earlier duplicate registration testing returned ERR 001 USERNAME_TAKEN;
  retrying with a different username succeeded.

Still to verify: private-message isolation with a third client and
at least five simultaneous clients.

## Room messaging milestone
Implemented JOIN, LEAVE, ROOMS and RMSG with room membership flags.
Room state is protected by the existing mutex.
Disconnect cleanup removes memberships; empty rooms are deleted.

Observed manual tests on CentOS:
- Alice and Bob joined lab successfully.
- ROOMS returned lab.
- Alice received OK SENT and Bob received Alice's room message.
- Bob received OK LEFT after leaving lab.
- Bob's later RMSG was rejected with ERR 005 NOT_IN_ROOM.
- An unknown room returned ERR 003 ROOM_NOT_FOUND.

Initial pasted commands produced errors; repeating commands individually
with Enter and waiting for each response completed the tests successfully.
Room isolation and disconnect cleanup still need local evidence.

## File transfer milestone
Implemented SENDFILE for users and rooms with a 1 MiB file limit.
The client calculates the byte count and sends a header followed by raw bytes.
A receiver thread handles incoming messages and files during uploads.
The server stores complete files under storage/IT23584990/<sender>/.
Clients save received files under received/<recipient>/<sender>/.

Observed CentOS tests:
- Private transfer: Alice sent sample_4990.txt to Bob (52 bytes).
- Room transfer: Alice sent room_4990.txt to #filelab (37 bytes).
- Bob received both files.
- cmp confirmed both server copies and both Bob copies were identical
  to the corresponding originals.

Local evidence for multiple room recipients, binary files, size-limit
rejection and interrupted uploads remains to be collected.

### 2026-10-07 — Timestamped server logging
Added timestamped logging for connections, commands, responses, file storage, forwarding and disconnects. A separate mutex serializes log writes. Verified on CentOS that a 52-byte Alice-to-Bob transfer produced FILE_STORED and FILE_FORWARDED records with result=OK. Bob received the file and exited using QUIT; the server recorded DISCONNECT and notified Alice. An unknown-user private message also produced a logged ERR 002 response.
