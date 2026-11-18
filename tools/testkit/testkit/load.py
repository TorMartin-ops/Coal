"""Host load measurement, used only to scale the last-resort wall-clock watchdog."""
import os


def scale():
    """>= 1: how much slower than an idle host this one is likely to run a guest.

    One-minute load average per core, so a host that runs several other guests gets a proportionally longer watchdog.
    Guest-time budgets are unaffected: they are counted in guest time, which load does not change."""
    try:
        load = os.getloadavg()[0]
    except OSError:
        return 1.0
    return min(8.0, max(1.0, 1.0 + load / (os.cpu_count() or 1)))
