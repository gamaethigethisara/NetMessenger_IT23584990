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
