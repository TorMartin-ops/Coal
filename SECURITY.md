# Security

## Trust model

Coal OS is a hobby kernel. It is not a security boundary: there are no users or permissions (every
process is root), no memory-protection hardening beyond ring 0/3 and paging, and it has not been
audited. Do not run untrusted code in it and do not expect isolation between its processes.

It has no network stack, so nothing can reach it except through its console, its disk image and the
second serial port.

## coal-mcp

`tools/coal-mcp` gives an MCP client full control of the Coal VM: it can run any command as root
inside the VM (`mcpd`, the in-VM agent, runs commands as root by design; the VM is the sandbox) and
read or write any file in the VM. `coal_read_file` (`save_to`) and `coal_write_file` (`host_path`) also
read and write host files with the privileges of the user running the server. Only connect MCP clients
you trust.

- The QEMU monitor and COM2 sockets live in a temporary directory with mode 0700, so other local
  users cannot attach to the VM; the build directory is created with mode 0700 as well.
- QEMU and CMake are started with argument lists, never through a shell.
- `mcpd` validates JSON fields, base64 input and lengths and caps output at 64 KiB per stream.
- The VM disk can always be put back with `coal_snapshot` / `coal_restore` / `coal_reset`.

## Reporting a problem

Please open a GitHub issue, or for something you would rather not make public, contact the maintainer
(Tor Martin Kohle) through the address on his GitHub profile.
