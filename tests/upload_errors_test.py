import socket
import uuid
from pathlib import Path

TAG = " NID:5849"
suffix = uuid.uuid4().hex[:8]

def line(sock):
    data = bytearray()
    while len(data) < 4096:
        part = sock.recv(1)
        assert part, "Unexpected connection closure"
        if part == b"\n":
            return data.decode()
        data.extend(part)
    raise AssertionError("Response too long")

def connect(name):
    sock = socket.create_connection(("127.0.0.1", 10990), timeout=5)
    sock.settimeout(5)
    sock.sendall(f"REGISTER {name}\n".encode())
    reply = line(sock)
    assert reply == "OK REGISTERED " + name + TAG, reply
    return sock

# Declare a file one byte larger than the 1 MiB limit.
with connect("large_" + suffix) as sock:
    sock.sendall(b"SENDFILE nobody large.bin 1048577\n")
    reply = line(sock)
    assert reply == "ERR 004 FILE_TOO_LARGE" + TAG, reply
    print(reply)
    assert sock.recv(1) == b"", "Oversized upload connection stayed open"
    print("PASS: oversized upload rejected and connection closed")

# Declare 100 bytes but send only three, then signal EOF.
name = "partial_" + suffix
with connect(name) as sock:
    sock.sendall(b"SENDFILE nobody partial.bin 100\nabc")
    sock.shutdown(socket.SHUT_WR)
    assert sock.recv(1) == b"", "Incomplete upload was not closed"

stored = Path("storage/IT23584990") / name / "partial.bin"
assert not stored.exists(), "Incomplete file was saved"
print("PASS: incomplete upload closed without saving a file")

# Verify the server still accepts and processes commands.
name = "health_" + suffix
with connect(name) as sock:
    sock.sendall(b"LIST\n")
    reply = line(sock)
    assert reply.startswith("OK USERS ") and reply.endswith(TAG), reply
    assert name in reply[len("OK USERS "):-len(TAG)].split(",")
    sock.sendall(b"QUIT\n")
    assert line(sock) == "OK BYE" + TAG
print("PASS: server remains responsive after upload errors")

print("ALL UPLOAD ERROR TESTS PASSED")
