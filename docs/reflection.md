# Reflection — IT23584990

Developing NetMessenger gave me practical experience with a TCP
application beyond a simple connection or echo example. The work
progressed from connecting a client to supporting registration,
messaging, rooms and binary file transfers. Building and testing each
stage on CentOS made it easier to identify which feature was working
before moving to the next stage.

An important lesson was that TCP provides a byte stream rather than
separate application messages. A command can arrive in several reads,
and several commands can arrive together. File data can also contain
newlines or bytes that resemble commands. The implementation therefore
uses newline-delimited headers and exact byte counts for file payloads.
The framing tests checked these situations, including a LIST command
immediately after uploaded file bytes.

Concurrency also required more than starting several threads. Shared
user and room information needs protection, and outgoing file headers
and payloads must remain together. The server uses a mutex for these
operations. Testing five clients demonstrated concurrent connections,
while the room tests checked that messages and files reached the
intended members. However, the shared mutex is also a limitation:
a slow recipient can delay other clients during an outgoing send.

The error tests provided useful evidence beyond successful transfers.
An oversized upload was rejected, an incomplete upload did not leave
a saved file, and a new client could still use the server afterward.
Stopping Carol's client abruptly and reconnecting confirmed that the
old room membership was cleared. Comparing binary files with cmp
provided stronger evidence of correctness than a success message alone.

I used ChatGPT/Codex extensively for explanations, generated code,
test scripts and documentation assistance. My practical work included
applying the supplied changes, compiling on CentOS, running tests,
checking outputs and providing screenshots for review. AI assistance
helped me progress, but successful compilation alone was insufficient;
the behaviour needed to be checked through actual execution.

Git commits recorded the development stages, while logs and screenshots
supported the test results. If extending this project, I would separate
outgoing delivery from the global state lock and add authentication,
encryption and recipient acknowledgements. Before the viva, I need to
practise tracing the socket lifecycle, framing logic and cleanup paths
so that I can explain and modify the implementation independently.
