import select
import socket
import uuid
from pathlib import Path

TAG = " NID:5849"
suffix = uuid.uuid4().hex[:8]
sender = "tx_" + suffix
receiver = "rx_" + suffix
filename = "framing.bin"
payload = bytes(range(256)) * 4 + b"\nLIST\n\x00QUIT\n"

def line(sock):
    data = bytearray()
    while len(data) < 4096:
        part = sock.recv(1)
        assert part, "Unexpected connection closure"
        if part == b"\n":
            text = data.decode()
            if text.startswith("MSG INFO "):
                data.clear()
                continue
            return text
        data.extend(part)
    raise AssertionError("Response too long")

def expect(sock, wanted):
    actual = line(sock)
    assert actual == wanted, (wanted, actual)

def connect(name):
    sock = socket.create_connection(("127.0.0.1", 10990), timeout=5)
    sock.settimeout(5)
    sock.sendall(f"REGISTER {name}\n".encode())
    expect(sock, "OK REGISTERED " + name + TAG)
    return sock

with connect(receiver) as rx, connect(sender) as tx:
    header = f"SENDFILE {receiver} {filename} {len(payload)}\n"
    tx.sendall(header.encode() + payload[:17])

    ready, _, _ = select.select([tx], [], [], 0.3)
    assert not ready, "Upload acknowledged before all bytes arrived"

    tx.sendall(payload[17:] + b"LIST\n")
    expect(rx, f"FILE {sender} {filename} {len(payload)}")

    received = bytearray()
    while len(received) < len(payload):
        part = rx.recv(len(payload) - len(received))
        assert part, "Incomplete forwarded file"
        received.extend(part)
    assert bytes(received) == payload, "Forwarded bytes changed"
    print("PASS: split binary payload forwarded unchanged")

    expect(tx, "OK FILE_RECEIVED " + filename + TAG)
    response = line(tx)
    assert response.startswith("OK USERS ") and response.endswith(TAG), response
    assert sender in response[len("OK USERS "):-len(TAG)].split(",")
    print("PASS: LIST after file bytes processed separately")

    stored = Path("storage/IT23584990") / sender / filename
    assert stored.read_bytes() == payload, "Stored bytes changed"
    print("PASS: server copy identical")

    # Rejected upload must still consume its declared payload.
    tx.sendall(b"SENDFILE #missing_" + suffix.encode()
               + b" rejected.bin 5\nabcdeLIST\n")
    expect(tx, "ERR 003 ROOM_NOT_FOUND" + TAG)
    response = line(tx)
    assert response.startswith("OK USERS ") and response.endswith(TAG), response
    print("PASS: rejected upload preserves next command")

    tx.sendall(b"QUIT\n")
    expect(tx, "OK BYE" + TAG)
    rx.sendall(b"QUIT\n")
    expect(rx, "OK BYE" + TAG)

print("ALL FILE FRAMING TESTS PASSED")
