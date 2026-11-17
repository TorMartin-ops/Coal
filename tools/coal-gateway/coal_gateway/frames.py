"""Frames of the agent protocol: a u32 little-endian length, then that many bytes of JSON.

Pure functions and one incremental decoder; no I/O. See docs/agent-vm/protocol.md.
"""
import json
import struct

MAX_FRAME = 256 * 1024          # what agentd accepts; its hello event announces the real limit
_HEADER = struct.Struct("<I")


class FrameError(Exception):
    """The byte stream cannot be a sequence of frames (length beyond the limit)."""


def encode(msg, max_frame=MAX_FRAME):
    body = json.dumps(msg, separators=(",", ":")).encode("ascii")     # ASCII keeps lone surrogates harmless
    if len(body) > max_frame:
        raise FrameError("frame of %d bytes exceeds the limit of %d" % (len(body), max_frame))
    return _HEADER.pack(len(body)) + body


class Decoder:
    """Feed it bytes as they arrive; it returns the messages completed so far.

    A frame whose body is not a JSON object is dropped and counted in `dropped`: the length prefix
    already told where it ends, so the stream stays in step.
    """

    def __init__(self, max_frame=MAX_FRAME):
        self.max_frame = max_frame
        self.buf = bytearray()
        self.dropped = 0

    def feed(self, data):
        self.buf += data
        out = []
        while len(self.buf) >= 4:
            (length,) = _HEADER.unpack_from(self.buf)
            if length > self.max_frame:
                raise FrameError("frame of %d bytes exceeds the limit of %d" % (length, self.max_frame))
            if len(self.buf) < 4 + length:
                break
            body = bytes(self.buf[4:4 + length])
            del self.buf[:4 + length]
            if not length:
                continue                                    # an empty frame is a no-op
            try:
                msg = json.loads(body.decode("utf-8", "replace"))
            except ValueError:
                msg = None
            if isinstance(msg, dict):
                out.append(msg)
            else:
                self.dropped += 1
        return out
