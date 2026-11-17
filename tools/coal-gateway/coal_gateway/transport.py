"""Byte-stream transports to agentd.

The agent protocol needs an ordered, reliable stream and nothing else, so a transport is four calls.
QEMU's COM2 chardev arrives as a unix or tcp socket; virtio-vsock is a socket of another family. The
framing, request ids and everything above are the same for all of them.
"""
import socket


class Transport:
    """Interface: connect() once, then send() whole buffers and recv() chunks; b"" from recv() is end of stream."""

    def connect(self, timeout=5.0):
        raise NotImplementedError

    def send(self, data):
        raise NotImplementedError

    def recv(self):
        raise NotImplementedError

    def close(self):
        raise NotImplementedError


class SocketTransport(Transport):
    def __init__(self, family, address):
        self.family, self.address = family, address
        self.sock = None

    def connect(self, timeout=5.0):
        s = socket.socket(self.family, socket.SOCK_STREAM)
        s.settimeout(timeout)
        try:
            s.connect(self.address)
        except OSError:
            s.close()
            raise
        s.settimeout(None)
        self.sock = s

    def send(self, data):
        self.sock.sendall(data)

    def recv(self):
        return self.sock.recv(65536)

    def close(self):
        s, self.sock = self.sock, None
        if s:
            try:
                s.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass
            s.close()


def parse_address(spec):
    """'unix:PATH', 'tcp:HOST:PORT' or 'vsock:CID:PORT' -> a new, unconnected transport."""
    kind, _, rest = spec.partition(":")
    if kind == "unix" and rest:
        return SocketTransport(socket.AF_UNIX, rest)
    if kind in ("tcp", "vsock"):
        host, _, port = rest.rpartition(":")
        if host and port.isdigit():
            if kind == "tcp":
                return SocketTransport(socket.AF_INET, (host, int(port)))
            if host.isdigit() and hasattr(socket, "AF_VSOCK"):
                return SocketTransport(socket.AF_VSOCK, (int(host), int(port)))
    raise ValueError("address must be unix:PATH, tcp:HOST:PORT or vsock:CID:PORT, got %r" % spec)
