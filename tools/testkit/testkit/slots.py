"""Host-wide admission control: a flock-based semaphore every QEMU start must pass.

The slots are the files /tmp/coal-testkit/slots/0 .. N-1 (N = max(1, nproc - 1), or $COAL_TESTKIT_SLOTS). A guest
takes an exclusive flock on a free slot file for the whole life of its QEMU: a non-blocking try over all slots, then a
blocking wait on slot 0. `flock /tmp/coal-testkit/slots/0 CMD` is therefore a valid way to take part from a shell.

A process tree holds one slot. coaltest sets COAL_TESTKIT_SLOT_HELD for the test it started, so a tool that is
itself admitted (the gateway) does not queue for a second slot and deadlock on a one-slot host. This module imports
nothing from the rest of the kit so that other tools can use it.
"""
import contextlib
import errno
import fcntl
import os
import time

HELD_ENV = "COAL_TESTKIT_SLOT_HELD"


def slot_dir():
    return os.environ.get("COAL_TESTKIT_SLOT_DIR", "/tmp/coal-testkit/slots")


def slot_count():
    try:
        return max(1, int(os.environ["COAL_TESTKIT_SLOTS"]))
    except (KeyError, ValueError):
        return max(1, (os.cpu_count() or 2) - 1)


def _open_slot(n):
    path = os.path.join(slot_dir(), str(n))
    os.makedirs(slot_dir(), exist_ok=True)
    for d in (os.path.dirname(slot_dir()), slot_dir()):
        with contextlib.suppress(OSError):
            os.chmod(d, 0o1777)                  # shared by every user of the host
    fd = os.open(path, os.O_RDWR | os.O_CREAT, 0o666)
    with contextlib.suppress(OSError):
        os.fchmod(fd, 0o666)
    return fd


class Slot:
    """An admission ticket. Release it (or use it as a context manager) when QEMU is gone."""

    def __init__(self, fd, index, waited):
        self.fd, self.index, self.waited = fd, index, waited

    def release(self):
        if self.fd is not None:
            os.close(self.fd)                    # closing drops the flock
            self.fd = None

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.release()


class _Inherited(Slot):
    """The parent process already holds a slot for this tree; nothing to take and nothing to release."""

    def __init__(self):
        super().__init__(None, int(os.environ[HELD_ENV] or 0), 0.0)


def acquire(label=""):
    """Blocks until a slot is free and returns it. `label` is only for diagnostics."""
    if HELD_ENV in os.environ:
        return _Inherited()
    start = time.monotonic()
    n = slot_count()
    fds = []
    try:
        for i in range(n):
            fd = _open_slot(i)
            try:
                fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
                return Slot(fd, i, time.monotonic() - start)
            except OSError as e:
                if e.errno not in (errno.EWOULDBLOCK, errno.EAGAIN):
                    raise
                fds.append(fd)
        fcntl.flock(fds[0], fcntl.LOCK_EX)       # every slot is busy: queue on slot 0
        slot = Slot(fds.pop(0), 0, time.monotonic() - start)
        return slot
    finally:
        for fd in fds:
            os.close(fd)


def held_env(slot):
    """Environment entries that tell a child process the tree already holds `slot`."""
    return {HELD_ENV: str(slot.index)}


def status():
    """[(slot index, busy)] for every slot, by trying to lock it."""
    out = []
    for i in range(slot_count()):
        fd = _open_slot(i)
        try:
            fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
            out.append((i, False))
        except OSError:
            out.append((i, True))
        finally:
            os.close(fd)
    return out
