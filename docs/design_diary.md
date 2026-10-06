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
