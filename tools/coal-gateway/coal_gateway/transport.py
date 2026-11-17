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


class HybridVsockTransport(SocketTransport):
    """A guest port behind a hybrid-vsock unix socket (Firecracker, vhost-device-vsock): connect, then "CONNECT port"."""

    def __init__(self, path, port):
        super().__init__(socket.AF_UNIX, path)
        self.port = port

    def connect(self, timeout=5.0):
        super().connect(timeout)
        self.sock.settimeout(timeout)
        try:
            self.sock.sendall(b"CONNECT %d\n" % self.port)
            reply = b""
            while not reply.endswith(b"\n"):          # one byte at a time: agentd's hello follows immediately
                c = self.sock.recv(1)
                if not c:
                    raise OSError("vsock connection refused")
                reply += c
            if not reply.startswith(b"OK"):
                raise OSError("vsock: " + reply.decode("ascii", "replace").strip())
        except OSError:
            self.close()
            raise
        self.sock.settimeout(None)


def parse_address(spec):
    """'unix:PATH', 'tcp:HOST:PORT', 'vsock:CID:PORT' or 'hvsock:PATH:PORT' -> a new, unconnected transport."""
    kind, _, rest = spec.partition(":")
    if kind == "unix" and rest:
        return SocketTransport(socket.AF_UNIX, rest)
    if kind == "hvsock":
        path, _, port = rest.rpartition(":")
        if path and port.isdigit():
            return HybridVsockTransport(path, int(port))
    if kind in ("tcp", "vsock"):
        host, _, port = rest.rpartition(":")
        if host and port.isdigit():
            if kind == "tcp":
                return SocketTransport(socket.AF_INET, (host, int(port)))
            if host.isdigit() and hasattr(socket, "AF_VSOCK"):
                return SocketTransport(socket.AF_VSOCK, (int(host), int(port)))
    raise ValueError("address must be unix:PATH, tcp:HOST:PORT, vsock:CID:PORT or hvsock:PATH:PORT, got %r" % spec)
