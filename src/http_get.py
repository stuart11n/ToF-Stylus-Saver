import socket


def get(url, timeout=10):
    """HTTP GET via raw sockets. Returns (status_code, body_text)."""
    if not url.startswith("http://"):
        raise ValueError("only http:// URLs supported")

    rest = url[7:]
    path = "/"
    if "/" in rest:
        host, path = rest.split("/", 1)
        path = "/" + path
    else:
        host = rest

    port = 80
    if ":" in host:
        host, port_s = host.split(":", 1)
        port = int(port_s)

    ai = socket.getaddrinfo(host, port)[0][-1]
    s = socket.socket()
    s.settimeout(timeout)
    try:
        s.connect(ai)
        req = (
            "GET %s HTTP/1.0\r\nHost: %s\r\nConnection: close\r\n\r\n" % (path, host)
        )
        s.send(req.encode())
        data = b""
        while True:
            chunk = s.recv(512)
            if not chunk:
                break
            data += chunk
            if len(data) > 8192:
                break
    finally:
        s.close()

    header, _, body = data.partition(b"\r\n\r\n")
    status = 0
    try:
        status = int(header.split(b" ", 2)[1])
    except (IndexError, ValueError):
        pass
    try:
        text = body.decode()
    except UnicodeError:
        text = str(body)
    return status, text
