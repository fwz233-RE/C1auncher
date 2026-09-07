#!/usr/bin/env python3
"""Test-only curl URL rewrite: all requests are confined to loopback fixtures."""
import json
import os
import sys
from urllib.parse import urlsplit

args = sys.argv[1:]
url = args[-1]
parts = urlsplit(url)
# Assert production transport policy before rewriting to local fixtures.
assert args[0] == "--disable", "curl must not load a user configuration"
assert args[args.index("--proto-redir") + 1] == ("=http" if parts.scheme == "http" else "=http,https")
timeout = int(args[args.index("--max-time") + 1])
if parts.path.endswith(("/index.v1", "/index.v1.sig")):
    assert 0 < timeout <= 45
else:
    assert timeout == 600
assert int(args[args.index("--speed-time") + 1]) >= 180
origin = f"{parts.scheme}://{parts.netloc}"
role = None
if origin in ("http://www.fwz233.com", "http://www.fwz233.com:80"):
    role = "primary"
elif origin == "http://123.56.214.77":
    role = "fallback"
log = os.environ.get("C1PKG_TEST_CURL_LOG")
if log:
    with open(log, "a", encoding="utf-8") as stream:
        stream.write(json.dumps({"role": role, "url": url, "args": args}) + "\n")
if role:
    target = os.environ.get(f"C1PKG_TEST_{role.upper()}", "dns-error")
    if target == "dns-error":
        sys.exit(6)
    if target == "connect-error":
        sys.exit(7)
    if target == "timeout-error":
        sys.exit(28)
    replacement = urlsplit(target)
    if replacement.scheme != "http" or replacement.hostname != "127.0.0.1":
        sys.exit("unsafe test fixture")
    args[-1] = f"http://{replacement.netloc}{parts.path}"
elif parts.scheme != "http" or parts.hostname != "127.0.0.1":
    sys.exit(6)  # Never contact a production, custom, or spoofed hostname.
os.execv("/usr/bin/curl", ["/usr/bin/curl", "--disable", "--noproxy", "*"] + args)
