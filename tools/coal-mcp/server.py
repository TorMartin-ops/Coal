#!/usr/bin/env python3
"""coal-mcp: MCP server over stdio that lets an agent drive Coal OS.

A thin entry point: the tools, the protocol and the VM management live in tools/coal-gateway.
Options: --attach ADDR, --build-dir DIR, --memory MIB, --log-dir DIR. Python 3.9+, standard library only.
"""
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "coal-gateway"))

from coal_gateway.cli import main_stdio  # noqa: E402

if __name__ == "__main__":
    main_stdio()
