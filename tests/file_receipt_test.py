#!/usr/bin/env python3
"""End-to-end tests for the optional FILE_ACK extension (Python 3.6+).

Run after make, with no other server listening on port 10990:
    python3 tests/file_receipt_test.py
The suite starts/stops its own server and uses temporary working directories.
The timeout case intentionally waits for the normal 30-second receipt timeout.
No third-party Python packages are required.
"""

import argparse
import contextlib
import os
from pathlib import Path
import queue
import re
import socket
import subprocess
import tempfile
import threading
import time

PORT = 10990
TAG = " NID:5849"
PROJECT = Path(__file__).resolve().parent.parent
PAYLOAD = bytes(range(256)) * 8 + b"\nFILEACK 99 SAVED\n\x00LIST\n"


class Lines:
    """Continuously drain output, retaining unmatched asynchronous messages."""

    def __init__(self, stream):
        self.items = queue.Queue()
        self.pending = []
        self.history = []
        self.thread = threading.Thread(target=self._read, args=(stream,))
        self.thread.daemon = True
        self.thread.start()

    def _read(self, stream):
        for text in stream:
            self.items.put(text.rstrip("\r\n"))
        self.items.put(None)

    def wait(self, prefix, timeout=8):
        end = time.monotonic() + timeout
        while True:
            for i, text in enumerate(self.pending):
                if text.startswith(prefix):
                    return self.pending.pop(i)
            try:
                text = self.items.get(timeout=max(0, end - time.monotonic()))
            except queue.Empty:
                raise AssertionError("Missing {!r}; output: {!r}".format(prefix, self.history[-15:]))
            if text is None:
                raise AssertionError("Process ended while waiting for {!r}; output: {!r}".format(
                    prefix, self.history[-15:]))
            self.history.append(text)
            self.pending.append(text)


class CClient:
    def __init__(self, binary, cwd, name, blocked=False):
        self.cwd = Path(cwd)
        self.cwd.mkdir(parents=True)
        self.name = name
        if blocked:
            (self.cwd / "received").write_text("A file deliberately blocks the receive directory.\n")
        self.process = subprocess.Popen(
            [str(binary), "127.0.0.1"], cwd=str(self.cwd), stdin=subprocess.PIPE,
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, universal_newlines=True, bufsize=1)
        self.output = Lines(self.process.stdout)
        self.wait("OK CAPS FILE_ACK")
        self.send("REGISTER " + name)
        self.wait("OK REGISTERED " + name)

    def send(self, text):
        self.process.stdin.write(text + "\n")
        self.process.stdin.flush()

    def wait(self, prefix, timeout=8):
        return self.output.wait(prefix, timeout)

    def sendfile(self, target, filename, data):
        path = self.cwd / filename
        path.write_bytes(data)
        self.send("SENDFILE {} {}".format(target, path))
        return int(self.wait("OK FILE_TRANSFER ").split()[2])

    def saved(self, sender, filename, expected):
        path = self.cwd / "received" / self.name / sender / filename
        assert path.read_bytes() == expected, "Received file bytes differ: " + str(path)

    def close(self):
        if self.process.poll() is None:
            try:
                self.send("QUIT")
                self.process.wait(timeout=4)
            except (OSError, subprocess.TimeoutExpired):
                self.process.terminate()
                self.process.wait(timeout=4)
        self.process.stdin.close()
        self.output.thread.join(timeout=2)
        self.process.stdout.close()


class Peer:
    """A raw protocol client: deliberately does not generate receipts."""

    def __init__(self, name, capable=True):
        self.name = name
        self.sock = socket.create_connection(("127.0.0.1", PORT), timeout=8)
        self.buffer = bytearray()
        self.pending = []
        if capable:
            self.send("CAPS FILE_ACK")
            self.wait("OK CAPS FILE_ACK")
        self.send("REGISTER " + name)
        self.wait("OK REGISTERED " + name)

    def send(self, text):
        self.sock.sendall((text + "\n").encode())

    def exact(self, size, timeout=8):
        end = time.monotonic() + timeout
        while len(self.buffer) < size:
            self.sock.settimeout(max(0.001, end - time.monotonic()))
            data = self.sock.recv(max(4096, size - len(self.buffer)))
            assert data, "Connection closed inside a frame"
            self.buffer.extend(data)
        data = bytes(self.buffer[:size])
        del self.buffer[:size]
        return data

    def line(self, timeout=8):
        end = time.monotonic() + timeout
        while b"\n" not in self.buffer:
            self.sock.settimeout(max(0.001, end - time.monotonic()))
            data = self.sock.recv(4096)
            assert data, "Connection closed while reading a line"
            self.buffer.extend(data)
            assert len(self.buffer) <= 2 * 1024 * 1024, "Unexpectedly large response"
        at = self.buffer.index(b"\n")
        text = bytes(self.buffer[:at]).decode()
        del self.buffer[:at + 1]
        return text

    def wait(self, prefix, timeout=8):
        end = time.monotonic() + timeout
        while True:
            for i, text in enumerate(self.pending):
                if text.startswith(prefix):
                    return self.pending.pop(i)
            text = self.line(max(0.001, end - time.monotonic()))
            if text.startswith(prefix):
                return text
            # A FILE header must be handled explicitly, never skipped as text.
            assert not text.startswith("FILE "), "Unhandled file frame: " + text
            self.pending.append(text)

    def sendfile(self, target, filename, data, tracked=True):
        self.sock.sendall("SENDFILE {} {} {}\n".format(target, filename, len(data)).encode() + data)
        if tracked:
            return int(self.wait("OK FILE_TRANSFER ").split()[2])
        self.wait("OK FILE_RECEIVED " + filename)
        return None

    def file(self, sender, filename, expected, tracked=True):
        ident = int(self.wait("FILEID ").split()[1]) if tracked else None
        header = self.wait("FILE ")
        assert header == "FILE {} {} {}".format(sender, filename, len(expected)), header
        assert self.exact(len(expected)) == expected, "Forwarded payload differs"
        return ident

    def close(self):
        self.sock.close()


class Suite:
    def __init__(self, root, server, client):
        self.root = Path(root)
        self.server_binary, self.client_binary = server, client
        self.server_dir = self.root / "server"
        self.server_dir.mkdir()
        self.process = subprocess.Popen(
            [str(server)], cwd=str(self.server_dir), stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT, universal_newlines=True, bufsize=1)
        self.output = Lines(self.process.stdout)
        self.output.wait("Listening on ")
        self.stack = contextlib.ExitStack()
        self.checks = 0

    def raw(self, name, capable=True):
        peer = Peer(name, capable)
        self.stack.callback(peer.close)
        return peer

    def client(self, name, blocked=False):
        client = CClient(self.client_binary, self.root / name, name, blocked)
        self.stack.callback(client.close)
        return client

    def receipt(self, sender, ident, name, status):
        actual = sender.wait("MSG FILE_RECEIPT {} {} ".format(ident, name))
        assert actual == "MSG FILE_RECEIPT {} {} {}{}".format(ident, name, status, TAG), actual

    def complete(self, sender, ident, saved, failed, unknown):
        actual = sender.wait("MSG FILE_COMPLETE {} ".format(ident))
        wanted = "MSG FILE_COMPLETE {} saved={} failed={} unknown={}{}".format(
            ident, saved, failed, unknown, TAG)
        assert actual == wanted, (wanted, actual)

    def passed(self, text):
        self.checks += 1
        print("PASS {:02d}: {}".format(self.checks, text), flush=True)

    def close(self):
        self.stack.close()
        self.process.terminate()
        self.process.wait(timeout=5)
        self.output.thread.join(timeout=2)
        self.process.stdout.close()


def exercise(suite):
    tx, good, bad = suite.client("ack_alice"), suite.client("ack_bob"), suite.client("ack_fail", True)
    first = tx.sendfile(good.name, "receipt.bin", PAYLOAD)
    suite.receipt(tx, first, good.name, "SAVED")
    suite.complete(tx, first, 1, 0, 0)
    good.saved(tx.name, "receipt.bin", PAYLOAD)
    assert (suite.server_dir / "storage/IT23584990" / tx.name / "receipt.bin").read_bytes() == PAYLOAD
    suite.passed("real C clients: binary private transfer, saved receipt and exact file comparisons")

    second = tx.sendfile(good.name, "receipt.bin", b"")
    suite.receipt(tx, second, good.name, "SAVED")
    suite.complete(tx, second, 1, 0, 0)
    good.saved(tx.name, "receipt.bin", b"")
    assert first != second
    suite.passed("zero-byte file and repeated filename have distinct transfer IDs")

    ident = tx.sendfile(bad.name, "cannot_save.bin", PAYLOAD)
    suite.receipt(tx, ident, bad.name, "SAVE_FAILED")
    suite.complete(tx, ident, 0, 1, 0)
    bad.send("LIST")
    bad.wait("OK USERS ")
    assert (bad.cwd / "received").is_file()
    suite.passed("C client reports directory/save failure and drains bytes; next command works")

    # Rename can fail after all writes succeeded; this must never return SAVED.
    blocked_path = good.cwd / "received" / good.name / tx.name / "rename.bin"
    blocked_path.mkdir()
    ident = tx.sendfile(good.name, "rename.bin", PAYLOAD)
    suite.receipt(tx, ident, good.name, "SAVE_FAILED")
    suite.complete(tx, ident, 0, 1, 0)
    assert blocked_path.is_dir()
    assert not list(blocked_path.parent.glob(".download-*"))
    suite.passed("final rename failure returns SAVE_FAILED and removes temporary file")

    for peer in (tx, good, bad):
        peer.send("JOIN ackroom")
        peer.wait("OK JOINED ackroom")
    ident = tx.sendfile("#ackroom", "room.bin", PAYLOAD)
    suite.receipt(tx, ident, good.name, "SAVED")
    suite.receipt(tx, ident, bad.name, "SAVE_FAILED")
    suite.complete(tx, ident, 1, 1, 0)
    good.saved(tx.name, "room.bin", PAYLOAD)
    suite.passed("room delivery reports each recipient and a mixed saved/failed summary")

    tx.send("JOIN empty_ackroom")
    tx.wait("OK JOINED empty_ackroom")
    ident = tx.sendfile("#empty_ackroom", "emptyroom.bin", b"hello")
    suite.complete(tx, ident, 0, 0, 0)
    suite.passed("room with no other members completes with zero recipients")

    ident = tx.sendfile(tx.name, "self.bin", PAYLOAD)
    suite.receipt(tx, ident, tx.name, "SAVED")
    suite.complete(tx, ident, 1, 0, 0)
    tx.saved(tx.name, "self.bin", PAYLOAD)
    suite.passed("self-transfer keeps FILE metadata, payload and ACK framing intact")

    rawtx, rx, outsider = suite.raw("ack_rawtx"), suite.raw("ack_rawrx"), suite.raw("ack_outside")
    ident = rawtx.sendfile(rx.name, "validation.bin", PAYLOAD)
    assert rx.file(rawtx.name, "validation.bin", PAYLOAD) == ident
    outsider.send("FILEACK {} SAVED".format(ident))
    outsider.wait("ERR 005 INVALID_FILE_ACK")
    for malformed in ("FILEACK", "FILEACK 0 SAVED", "FILEACK -1 SAVED",
                      "FILEACK 18446744073709551616 SAVED",
                      "FILEACK {} WRONG".format(ident),
                      "FILEACK {} SAVED extra".format(ident)):
        rx.send(malformed)
        rx.wait("ERR 005 INVALID_FILE_ACK")
    # Fragmented ACK, then coalesced duplicate ACK + LIST exercise byte framing.
    rx.sock.sendall("FILEACK {} SA".format(ident).encode())
    rx.sock.sendall(b"VED\n")
    suite.receipt(rawtx, ident, rx.name, "SAVED")
    suite.complete(rawtx, ident, 1, 0, 0)
    rx.send("FILEACK {} SAVED\nLIST".format(ident))
    rx.wait("ERR 005 INVALID_FILE_ACK")
    rx.wait("OK USERS ")
    suite.passed("forged/malformed/duplicate ACKs rejected; fragmented and coalesced commands handled")

    oldrx, oldtx = suite.raw("ack_legacyrx", False), suite.raw("ack_legacytx", False)
    ident = rawtx.sendfile(oldrx.name, "legacy.bin", PAYLOAD)
    oldrx.file(rawtx.name, "legacy.bin", PAYLOAD, tracked=False)
    suite.receipt(rawtx, ident, oldrx.name, "UNSUPPORTED")
    suite.complete(rawtx, ident, 0, 0, 1)
    assert not any(text.startswith("FILEID ") for text in oldrx.pending)
    oldtx.sendfile(rx.name, "oldsender.bin", PAYLOAD, tracked=False)
    rx.file(oldtx.name, "oldsender.bin", PAYLOAD, tracked=False)
    assert not any(text.startswith("FILEID ") for text in rx.pending)
    suite.passed("legacy sender/receiver wire format retained; missing capability reported as unknown")

    ident = rawtx.sendfile(rx.name, "disconnect.bin", b"disconnect")
    assert rx.file(rawtx.name, "disconnect.bin", b"disconnect") == ident
    rx.close()
    suite.receipt(rawtx, ident, rx.name, "DISCONNECTED")
    suite.complete(rawtx, ident, 0, 0, 1)
    rx = suite.raw("ack_rawrx")
    rx.send("FILEACK {} SAVED".format(ident))
    rx.wait("ERR 005 INVALID_FILE_ACK")
    suite.passed("disconnect is unknown; reconnecting under the same name cannot acknowledge the old transfer")

    ident = rawtx.sendfile(rx.name, "sendergone.bin", b"sendergone")
    assert rx.file(rawtx.name, "sendergone.bin", b"sendergone") == ident
    rawtx.send("QUIT")
    rawtx.wait("OK BYE")
    # Waiting for LEFT establishes that disconnect cleanup has run before reconnect.
    rx.wait("MSG INFO ack_rawtx LEFT")
    rawtx.close()
    rawtx = suite.raw("ack_rawtx")
    rx.send("FILEACK {} SAVED".format(ident))
    rx.wait("ERR 005 INVALID_FILE_ACK")
    rawtx.send("LIST")
    rawtx.wait("OK USERS ")
    assert not any(text.startswith("MSG FILE_") for text in rawtx.pending)
    suite.passed("sender disconnect releases tracking; new sender session receives no stale receipt")

    # Fill the bounded table while every raw receiver withholds its ACK.
    ids = []
    for _ in range(64):
        ident = rawtx.sendfile(rx.name, "limit.bin", b"")
        assert rx.file(rawtx.name, "limit.bin", b"") == ident
        ids.append(ident)
    rawtx.send("SENDFILE {} rejected.bin 0".format(rx.name))
    rawtx.wait("ERR 006 RECEIPT_LIMIT_REACHED")
    assert not (suite.server_dir / "storage/IT23584990" / rawtx.name / "rejected.bin").exists()
    rx.send("FILEACK {} SAVED".format(ids[0]))
    suite.receipt(rawtx, ids[0], rx.name, "SAVED")
    suite.complete(rawtx, ids[0], 1, 0, 0)
    ident = rawtx.sendfile(rx.name, "released.bin", b"")
    assert rx.file(rawtx.name, "released.bin", b"") == ident
    for pending in ids[1:] + [ident]:
        rx.send("FILEACK {} SAVED".format(pending))
    for pending in ids[1:] + [ident]:
        suite.receipt(rawtx, pending, rx.name, "SAVED")
        suite.complete(rawtx, pending, 1, 0, 0)
    suite.passed("64 pending-transfer bound enforced; an ACK releases capacity without losing framing")

    # Both real clients upload maximum-size files at once while their receive
    # threads must keep draining incoming files and queueing receipts.
    bulk_a, bulk_b = bytes(range(256)) * 4096, bytes(reversed(range(256))) * 4096
    (tx.cwd / "duplex_a.bin").write_bytes(bulk_a)
    (good.cwd / "duplex_b.bin").write_bytes(bulk_b)
    tx.send("SENDFILE {} {}".format(good.name, tx.cwd / "duplex_a.bin"))
    good.send("SENDFILE {} {}".format(tx.name, good.cwd / "duplex_b.bin"))
    a = int(tx.wait("OK FILE_TRANSFER ").split()[2])
    b = int(good.wait("OK FILE_TRANSFER ").split()[2])
    suite.receipt(tx, a, good.name, "SAVED")
    suite.receipt(good, b, tx.name, "SAVED")
    suite.complete(tx, a, 1, 0, 0)
    suite.complete(good, b, 1, 0, 0)
    good.saved(tx.name, "duplex_a.bin", bulk_a)
    tx.saved(good.name, "duplex_b.bin", bulk_b)
    suite.passed("simultaneous 1 MiB C-client uploads finish with intact files and receipts in both directions")

    # No command is sent after the upload: the timer must work while idle.
    ident = rawtx.sendfile(rx.name, "timeout.bin", b"unconfirmed")
    assert rx.file(rawtx.name, "timeout.bin", b"unconfirmed") == ident
    print("Waiting for the normal 30-second receipt timeout...", flush=True)
    actual = rawtx.wait("MSG FILE_RECEIPT {} ".format(ident), timeout=38)
    assert actual == "MSG FILE_RECEIPT {} {} TIMEOUT{}".format(ident, rx.name, TAG), actual
    suite.complete(rawtx, ident, 0, 0, 1)
    rx.send("FILEACK {} SAVED".format(ident))
    rx.wait("ERR 005 INVALID_FILE_ACK")
    suite.passed("idle timeout reports unknown; late ACK rejected instead of claiming delivery")

    log = (suite.server_dir / "netmsg_IT23584990.log").read_text()
    for status in ("SAVED", "SAVE_FAILED", "UNSUPPORTED", "DISCONNECTED", "TIMEOUT"):
        assert re.search(r"event=FILE_RECEIPT .*status=" + status + r"\n", log), status
    assert "event=FILE_COMPLETE " in log
    assert "event=FILE_TRACKING_CANCELLED " in log
    suite.passed("server log records terminal receipts, completion summaries and cancelled tracking")


def incomplete_download(client, root):
    """A controlled server drops a forwarded file halfway through its payload."""
    cwd = Path(root) / "incomplete_client"
    cwd.mkdir()
    with socket.socket() as listener:
        listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        listener.bind(("127.0.0.1", PORT))
        listener.listen(1)
        listener.settimeout(8)
        process = subprocess.Popen(
            [str(client), "127.0.0.1"], cwd=str(cwd), stdin=subprocess.PIPE,
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, universal_newlines=True, bufsize=1)
        output = Lines(process.stdout)
        try:
            connection, _ = listener.accept()
            with connection:
                connection.settimeout(8)
                with connection.makefile("rb", buffering=0) as source:
                    assert source.readline() == b"CAPS FILE_ACK\n"
                    connection.sendall(b"OK CAPS FILE_ACK NID:5849\n")
                    process.stdin.write("REGISTER partialrx\n")
                    process.stdin.flush()
                    assert source.readline() == b"REGISTER partialrx\n"
                    connection.sendall(b"OK REGISTERED partialrx NID:5849\n"
                                       b"FILEID 401\nFILE stub partial.bin 20\nshort")
                    connection.shutdown(socket.SHUT_WR)
                    assert source.read() == b"", "Client sent an ACK for an incomplete file"
                assert process.wait(timeout=8) != 0, "Incomplete frame was treated as graceful closure"
            output.wait("File receive failed: partial.bin")
            assert not (cwd / "received/partialrx/stub/partial.bin").exists()
            assert not list(cwd.rglob(".download-*"))
        finally:
            if process.poll() is None:
                process.terminate()
                process.wait(timeout=4)
            process.stdin.close()
            output.thread.join(timeout=2)
            process.stdout.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--server", type=Path, default=PROJECT / "server_4990")
    parser.add_argument("--client", type=Path, default=PROJECT / "client_4990")
    args = parser.parse_args()
    server, client = args.server.resolve(), args.client.resolve()
    for binary in (server, client):
        if not binary.is_file() or not os.access(str(binary), os.X_OK):
            parser.error("Build the programs first; executable missing: " + str(binary))
    # Never stop or test against an unrelated listener.
    with socket.socket() as probe:
        probe.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        try:
            probe.bind(("0.0.0.0", PORT))
        except OSError:
            parser.error("Port 10990 is busy. Stop your existing server before running this suite.")
    with tempfile.TemporaryDirectory(prefix="netmsg-receipts-") as root:
        suite = Suite(root, server, client)
        try:
            exercise(suite)
        finally:
            suite.close()
        incomplete_download(client, root)
        suite.passed("incomplete forwarded file is removed; C client sends no receipt and exits with failure")
        print("ALL {} FILE RECEIPT TEST GROUPS PASSED".format(suite.checks), flush=True)


if __name__ == "__main__":
    main()
