"""Who may call the HTTP endpoint.

Authenticator is the seam for OAuth 2.1. The shipped BearerToken compares one static token read from a
0600 file. For remote use, implement the same two methods for a resource server (validate the access
token's signature, audience and expiry; answer 401 with a WWW-Authenticate header that points at the
protected-resource metadata of RFC 9728) and pass it to HttpServer; nothing else changes.
"""
import hmac
import os
import secrets
import stat

from .errors import ToolError


class Principal:
    def __init__(self, subject, scopes=()):
        self.subject, self.scopes = subject, tuple(scopes)


class Authenticator:
    """Decides whether a request is allowed. Replace this to switch to OAuth 2.1."""

    def authenticate(self, headers):
        """Returns a Principal, or None when the request carries no valid credentials."""
        raise NotImplementedError

    def challenge(self):
        """The WWW-Authenticate value sent with a 401."""
        raise NotImplementedError

    def resource_metadata(self):
        """The RFC 9728 protected-resource metadata to serve at /.well-known/oauth-protected-resource, or None."""
        return None


class BearerToken(Authenticator):
    def __init__(self, token):
        self.token = token.encode()

    def authenticate(self, headers):
        scheme, _, value = (headers.get("Authorization") or "").partition(" ")
        if scheme.lower() == "bearer" and hmac.compare_digest(value.strip().encode(), self.token):
            return Principal("local")
        return None

    def challenge(self):
        return 'Bearer realm="coal-gateway"'


def load_or_create_token(path):
    """The token in the 0600 file at path; a new random one is written there on first use."""
    try:
        mode = os.stat(path).st_mode
    except FileNotFoundError:
        os.makedirs(os.path.dirname(path) or ".", mode=0o700, exist_ok=True)
        token = secrets.token_urlsafe(32)
        fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
        with os.fdopen(fd, "w") as f:
            f.write(token + "\n")
        return token
    if mode & (stat.S_IRWXG | stat.S_IRWXO):
        raise ToolError("%s is readable by other users (mode %o); run: chmod 600 %s" % (path, mode & 0o777, path))
    with open(path) as f:
        token = f.read().strip()
    if len(token) < 16:
        raise ToolError("%s holds no usable token (needs at least 16 characters); delete it to generate a new one" % path)
    return token
