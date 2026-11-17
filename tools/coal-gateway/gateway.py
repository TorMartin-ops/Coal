#!/usr/bin/env python3
"""coal-gateway: MCP server for the Coal Agent VM, over stdio or Streamable HTTP.

    gateway.py stdio [--attach ADDR] [--build-dir DIR]
    gateway.py http  [--host 127.0.0.1] [--port 7077] [--token-file FILE]

See docs/agent-vm/gateway.md. Python 3.9+, standard library only.
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from coal_gateway.cli import main  # noqa: E402

if __name__ == "__main__":
    main()
