---
title: viewport.*
layout: default
parent: Tools reference
---

# viewport.* namespace

Full tool reference and per-arg details: see the [plugin README in the repo](https://github.com/bluesteelll/ue5-mcp-bridge/blob/main/README.md#viewport).

Probe the live editor for current handlers in this namespace:

```python
import socket, json
s = socket.create_connection(("127.0.0.1", 30020), timeout=3)
s.sendall(b'{"id":"p","kind":"call_function","method":"tools.list","args":{}}\n')
buf = b""
while b"\n" not in buf:
    buf += s.recv(64*1024)
import re
all_handlers = json.loads(buf[:buf.index(b"\n")].decode())["result"]["cpp_handlers"]
print([h for h in all_handlers if h.startswith("viewport.")])
```
