# NetMessenger

IE3010 - Network Programming
Individual Assignment
Registration number: IT23584990

## Overview
A TCP client-server application in C for multiple users to exchange
messages and files through a central server.

## Personalisation
- Numeric registration part: 23584990
- Last four digits: 4990
- TCP port: 6000 + 4990 = 10990
- Middle four digits (positions 3-6): 5849
- Response tag: NID:5849
- Server source: server_4990.c
- Client source: client_4990.c
- Build file: Makefile_4990
- Server log: netmsg_IT23584990.log
- File storage: ./storage/IT23584990/<sender_username>/<filename>

## Development environment
- CentOS Stream 10
- GCC 14.4.1
- GNU Make 4.4.1
- Git 2.52.0

## Implementation plan
1. Establish TCP connections and implement command framing.
2. Support multiple clients, registration and user presence.
3. Implement broadcast and private messaging.
4. Implement chat rooms and room messaging.
5. Implement file transfer and server-side storage.
6. Add logging, error handling and disconnect cleanup.
7. Add rate limiting as an optional extension.
8. Test the system and document real execution evidence.

## Current status
Repository and development tools are ready.
Application implementation has not started.
Build and run instructions will be added with the working code.
