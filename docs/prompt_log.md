# AI Assistance Log - IT23584990

## Entry 1 - Assignment and environment setup
Tool: ChatGPT / Codex
Request summary: Explain the assignment and provide step-by-step
guidance for CentOS and Git setup using registration IT23584990.
Assistance: Requirements explanation, personalised values,
repository setup and README instructions.
Validation: Checked terminal output and Git status.

## Entry 2 - Initial TCP connection
Tool: ChatGPT / Codex
Request summary: Continue with the first server and client implementation.
Assistance: Generated the initial C server and client, compilation
commands, Makefile and executable ignore rules.
Validation: Compiled both programs on CentOS, ran a loopback
connection test and rebuilt using make. Shared screenshots for review.
Current limitation: This version only establishes a TCP connection.
It does not yet implement chat commands or concurrent client sessions.

These entries summarise the interaction. Preserve the original
conversation as the detailed prompt and response record.

## Entry 3 - Registration and presence
Tool: ChatGPT / Codex
Retrospective assistance summary, not a verbatim prompt.
AI supplied the concurrent registration and presence implementation,
including per-client threads, shared-state locking, newline-delimited
commands, complete-send handling, REGISTER, LIST and QUIT.
I compiled and ran the programs on CentOS and supplied screenshots.
Tests showed simultaneous registered users, user listing, join/leave
notifications, duplicate-name rejection and graceful QUIT responses.
The early select-based client was later replaced by the file-transfer
client with a dedicated receiver thread and poll-based input handling.

## Entry 4 - Broadcast and private messaging
Requested the next implementation step after registration and listing.
AI supplied BCAST and PMSG handlers and manual two-client test instructions.
Applied the changes, rebuilt the server, and tested broadcast delivery,
private-message delivery and an unknown recipient.
Shared terminal screenshots for review; the observed results matched
the expected responses. Three-client privacy testing remains pending.

## Entry 5 - Room messaging
Uploaded the current server source and requested room support.
AI added JOIN, LEAVE, ROOMS, RMSG and membership cleanup, and reported
compilation and automated checks in its own environment.
Installed the updated source in CentOS and manually tested room joining,
listing, message delivery, leaving and rejection after leaving.
Shared screenshots and repeated the initial commands individually
after input-format problems. The repeated tests matched expected results.

## Entry 6 - File transfer
Uploaded the current client source and requested SENDFILE support.
AI provided updated C server/client files and reported automated checks
in its own environment.
Built both files on CentOS and manually tested private and room transfers.
Verified the server and recipient copies using cmp and shared screenshots.
All four local file comparisons passed.

### 7. Server logging assistance — 2026-10-07
AI assistance summary: Codex supplied a server update for timestamped event logging and commands to verify it. The update was compiled and tested on CentOS. Screenshots confirmed command/error records, a 52-byte file storage and forwarding event, and Bob's normal disconnect. This entry summarizes the assistance; the original conversation contains the full prompts and responses.

### 8. Verification assistance — 2026-10-07
Codex provided manual test steps for five connected clients, binary room transfer, room isolation and abrupt disconnect cleanup. It also supplied Python socket scripts for command and file framing tests. I ran these tests on CentOS and supplied screenshots of the outputs for review. Both framing scripts reported all checks passed. This is a summary; full prompts and responses are in the original conversation.

### 9. Upload error test assistance — 2026-10-07
Codex supplied tests/upload_errors_test.py to check oversized upload rejection, incomplete upload handling and server responsiveness afterward. I ran the script on CentOS; all three checks passed, and I supplied the output screenshot for review.

## Entry 10 - Code review and documentation — 2026-10-07
Tool: ChatGPT / Codex
I uploaded NetMessenger_IT23584990_review.zip for review.
Codex inspected the C sources, Makefile, tests and documentation.
It reported a warning-free build and successful execution of the
three supplied test scripts in its own environment. These checks
were separate from my earlier CentOS test runs.

Codex supplied an updated README covering implemented features,
build/run instructions, protocol framing, storage, tests and limits.
It also supplied a consolidated design diary based on the recorded
development milestones and observed tests. I applied the documentation
updates and pushed them to GitHub.

This entry and the registration entry are summaries of assistance,
not verbatim transcripts of the original prompts and responses.
