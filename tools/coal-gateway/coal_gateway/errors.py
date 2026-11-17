"""Errors shared by the gateway layers."""


class ToolError(Exception):
    """A failure to report to the client as a tool result with isError set."""


class AgentError(ToolError):
    """agentd answered a request with ok:false."""

    def __init__(self, error):
        if not isinstance(error, dict):
            error = {"message": str(error)}
        self.code = error.get("code", "error")
        self.errno = error.get("errno")
        message = error.get("message", "unknown error")
        super().__init__(message + (" (errno %d)" % self.errno if self.errno else ""))
