import select
import socket

TAG = " NID:5849"

def read_line(sock):
    data = bytearray()
    while len(data) < 4096:
        part = sock.recv(1)
        if not part:
            raise AssertionError("Server closed the connection")
        if part == b"\n":
            return data.decode()
        data.extend(part)
    raise AssertionError("Response line too long")

def expect(sock, expected):
    actual = read_line(sock)
    assert actual == expected, f"Expected {expected!r}, got {actual!r}"

with socket.create_connection(("127.0.0.1", 10990), timeout=5) as sock:
    sock.settimeout(5)

    # Send only part of REGISTER, without a newline.
    sock.sendall(b"REG")
    ready, _, _ = select.select([sock], [], [], 0.3)
    assert not ready, "Server responded before the command was complete"

    sock.sendall(b"ISTER framinguser\n")
    expect(sock, "OK REGISTERED framinguser" + TAG)
    print("PASS: split REGISTER command")

    # Two complete commands in one write.
    sock.sendall(b"LIST\nROOMS\n")
    users = read_line(sock)
    assert users.startswith("OK USERS "), users
    assert users.endswith(TAG), users
    names = users[len("OK USERS "):-len(TAG)].split(",")
    assert "framinguser" in names, users

    rooms = read_line(sock)
    assert rooms.startswith("OK ROOMS "), rooms
    assert rooms.endswith(TAG), rooms
    print("PASS: combined LIST and ROOMS commands")

    # A complete command followed by an incomplete command.
    sock.sendall(b"LIST\nRO")
    users = read_line(sock)
    assert users.startswith("OK USERS ") and users.endswith(TAG), users

    ready, _, _ = select.select([sock], [], [], 0.3)
    assert not ready, "Server processed the incomplete RO command"

    sock.sendall(b"OMS\nQUIT\n")
    rooms = read_line(sock)
    assert rooms.startswith("OK ROOMS ") and rooms.endswith(TAG), rooms
    expect(sock, "OK BYE" + TAG)
    assert sock.recv(1) == b"", "Connection remained open after QUIT"
    print("PASS: partial next command preserved")
    print("PASS: QUIT response and connection closure")

print("ALL TCP COMMAND FRAMING TESTS PASSED")
