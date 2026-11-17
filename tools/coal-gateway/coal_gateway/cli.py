"""Command line: build a Gateway around a Machine and serve it on stdio or HTTP."""
import argparse
import os
import signal
import sys

from . import auth
from .errors import ToolError
from .machine import SRC_DIR, Machine
from .mcp import Gateway
from .runlog import RunLog
from .stdio import StdioServer
from .streamable_http import HttpServer
from .transport import parse_address


def log(msg):
    print("coal-gateway: " + msg, file=sys.stderr, flush=True)


def repo_version():
    try:
        with open(os.path.join(SRC_DIR, "VERSION")) as f:
            return f.read().strip() or "0"
    except OSError:
        return "0"


def default_token_file():
    base = os.environ.get("XDG_CONFIG_HOME") or os.path.join(os.path.expanduser("~"), ".config")
    return os.path.join(base, "coal-gateway", "token")


def add_common(ap):
    ap.add_argument("--attach", metavar="unix:PATH|tcp:HOST:PORT|vsock:CID:PORT",
                    help="use an already running agentd at this address instead of managing QEMU")
    ap.add_argument("--build-dir", help="CMake build directory (default: <repo>/build/coal-mcp)")
    ap.add_argument("--memory", type=int, metavar="MIB",
                    help="default guest RAM for coal_boot (default: $COAL_MEMORY_MB or 512)")
    ap.add_argument("--log-dir", help="where per-run JSONL logs go (default: <build-dir>/runs)")


def parser(prog="coal-gateway", with_http=True):
    ap = argparse.ArgumentParser(prog=prog, description="MCP gateway that drives Coal OS in QEMU")
    add_common(ap)
    if with_http:
        sub = ap.add_subparsers(dest="mode", required=True)
        sub.add_parser("stdio", help="serve one client on stdin/stdout")
        h = sub.add_parser("http", help="serve MCP over Streamable HTTP (stateless, bearer token)")
        h.add_argument("--host", default="127.0.0.1", help="address to bind (default 127.0.0.1)")
        h.add_argument("--port", type=int, default=7077)
        h.add_argument("--token-file", default=default_token_file(),
                       help="bearer token file, mode 0600, created on first run (default: %(default)s)")
        h.add_argument("--allow-origin", action="append", default=[], metavar="ORIGIN",
                       help="accept browser requests from this Origin (localhost origins are always accepted)")
        h.add_argument("--allow-non-loopback", action="store_true",
                       help="permit binding a non-loopback address (put TLS and OAuth 2.1 in front)")
    return ap


def build_gateway(args, name):
    if args.memory:
        os.environ["COAL_MEMORY_MB"] = str(args.memory)
    if args.build_dir:
        os.environ["COAL_BUILD_DIR"] = args.build_dir
    try:
        if args.attach:
            parse_address(args.attach)
    except ValueError as e:
        raise SystemExit("--attach: %s" % e)
    machine = Machine(args.attach)
    runlog = RunLog(args.log_dir or os.path.join(machine.build_dir, "runs"), lambda: machine.workspace_id)
    signal.signal(signal.SIGTERM, lambda *_: (machine.shutdown(), os._exit(0)))
    return machine, Gateway(machine, runlog, {"name": name, "version": repo_version()}, log)


def run_stdio(args, name):
    machine, gateway = build_gateway(args, name)
    try:
        StdioServer(gateway).serve()
    except KeyboardInterrupt:
        pass
    machine.shutdown()


def run_http(args, name):
    machine, gateway = build_gateway(args, name)
    try:
        token = auth.load_or_create_token(args.token_file)
        server = HttpServer(gateway, auth.BearerToken(token), args.host, args.port, args.allow_origin,
                            args.allow_non_loopback)
    except (ToolError, ValueError, OSError) as e:
        raise SystemExit("coal-gateway: %s" % e)
    log("listening on http://%s:%d/mcp; bearer token in %s" % (server.address + (args.token_file,)))
    log("run log: %s (run %s)" % (gateway.runlog.directory, gateway.default_run))
    try:
        server.serve()
    except KeyboardInterrupt:
        pass
    machine.shutdown()


def main(argv=None):
    args = parser().parse_args(argv)
    (run_stdio if args.mode == "stdio" else run_http)(args, "coal-gateway")


def main_stdio(argv=None, name="coal-mcp"):
    """Entry point of the stdio-only server: the same options, no mode word."""
    run_stdio(parser("coal-mcp", with_http=False).parse_args(argv), name)
