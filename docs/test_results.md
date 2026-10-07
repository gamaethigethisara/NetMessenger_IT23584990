
### Five connected clients and broadcast — 2026-10-07
Environment: CentOS, TCP port 10990.
Connected users: alice, bob, carol, dave and erin.
Alice's LIST response included all five users with NID:5849.
Alice sent BCAST Five-client test IT23584990 and received OK SENT.
Bob, Carol, Dave and Erin each received the matching MSG BCAST.
Result: PASS for five simultaneous connections and broadcast delivery.

### Port and connection evidence — 2026-10-07
ss confirmed server_4990 listening on 0.0.0.0:10990 and five established server-side TCP connections.
Result: PASS.

### Binary room transfer and isolation — 2026-10-07
Alice, Bob and Carol joined binarylab; Dave and Erin remained outside.
Alice sent room_binary_4990.bin containing 4096 bytes, including all byte values from 0 to 255.
Bob and Carol each received 4096 bytes. cmp confirmed that the server copy and both recipient copies matched the original.
Dave and Erin displayed no file receipt, and neither had the file in their receive directory.
Result: PASS.

### Abrupt disconnect and room membership cleanup — 2026-10-07
Carol's client was stopped with Ctrl+C while joined to binarylab.
Alice received MSG INFO carol LEFT, and LIST showed only alice,bob,dave,erin.
Carol reconnected and registered successfully. RMSG binarylab returned ERR 005 NOT_IN_ROOM, confirming that the old membership was removed.
After JOIN binarylab, Carol's room message reached Alice and Bob. Dave and Erin received presence notifications but no room message.
Result: PASS.

### TCP command framing — 2026-10-07
Ran tests/tcp_framing_test.py on CentOS.
Verified split REGISTER, combined LIST and ROOMS, preservation of an incomplete next command, and QUIT followed by connection closure.
Result: All four checks PASS.

### File framing — 2026-10-07
Ran tests/file_framing_test.py on CentOS.
Verified split binary payload forwarding without changes, separate processing of LIST immediately after file bytes, and an identical server copy.
An upload to a nonexistent room returned ERR 003, and the following LIST command was processed correctly.
Result: All four checks PASS.
