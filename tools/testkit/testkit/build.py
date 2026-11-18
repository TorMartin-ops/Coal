"""Builds Coal OS once per coaltest invocation and fingerprints what the tests run."""
import contextlib
import os
import shutil
import subprocess
from dataclasses import dataclass

from . import qemu, slots

SRC = "src/CoalOS"


class BuildError(Exception):
    pass


@dataclass
class Build:
    dir: str
    kernel: str
    disk: str                      # pristine: guest tests only read it (QEMU snapshot)
    work_disk: str                 # the one coal-mcp and the gateway write to; reset before each host-driven test
    kernel_sha: str
    disk_sha: str

    def reset_disk(self):
        subprocess.run(["cp", "--sparse=always", self.disk, self.work_disk], check=True)


def build(repo, build_dir, log):
    """cmake + build into build_dir (incremental), holding an admission slot while the compiler runs."""
    os.makedirs(build_dir, exist_ok=True)
    jobs = str(os.cpu_count() or 2)
    steps = [["cmake", "-S", os.path.join(repo, SRC), "-B", build_dir],
             ["cmake", "--build", build_dir, "-j", jobs]]
    with contextlib.suppress(FileNotFoundError):
        os.unlink(os.path.join(build_dir, "disk.img"))     # a disk a host-driven test wrote to is never reused
    with slots.acquire("build"), open(log, "w") as out:
        for cmd in steps:
            if subprocess.run(cmd, stdout=out, stderr=subprocess.STDOUT).returncode:
                raise BuildError("%s failed; see %s" % (" ".join(cmd[:2]), log))
    kernel, disk = (os.path.join(build_dir, n) for n in ("kernel.bin", "disk.img"))
    pristine = os.path.join(build_dir, "disk.pristine.img")
    shutil.copyfile(disk, pristine)
    return Build(build_dir, kernel, pristine, disk, qemu.sha256_file(kernel), qemu.sha256_file(pristine))
