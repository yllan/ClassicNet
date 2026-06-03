#!/usr/bin/env python3
"""
Minimal HTTP/2-over-TLS test server for ClassicNet's h2 client integration test.

Uses the sans-io `h2` library over a blocking TLS socket with ALPN advertising
"h2".  Answers every request with `200 OK`, a small text/plain body, and
END_STREAM.  Single-threaded, one connection at a time, loops forever until
killed.  Driven by scripts/test-h2.sh.

  usage: h2_test_server.py <port> <cert.pem> <key.pem>
"""
import socket
import ssl
import sys

import h2.config
import h2.connection
import h2.events

BODY = b"hello from h2\n"


def handle(conn_sock):
    config = h2.config.H2Configuration(client_side=False)
    conn = h2.connection.H2Connection(config=config)
    conn.initiate_connection()
    conn_sock.sendall(conn.data_to_send())

    while True:
        data = conn_sock.recv(65535)
        if not data:
            return
        events = conn.receive_data(data)
        for event in events:
            if isinstance(event, h2.events.RequestReceived):
                hdrs = dict(event.headers)
                method = hdrs.get(b":method", b"?").decode()
                path = hdrs.get(b":path", b"?").decode()
                auth = hdrs.get(b":authority", b"?").decode()
                sys.stderr.write(">> h2 request from client: %s %s (authority %s) -> 200\n"
                                 % (method, path, auth))
                sys.stderr.flush()
                sid = event.stream_id
                conn.send_headers(sid, [
                    (":status", "200"),
                    ("content-type", "text/plain"),
                    ("content-length", str(len(BODY))),
                ])
                conn.send_data(sid, BODY, end_stream=True)
        out = conn.data_to_send()
        if out:
            conn_sock.sendall(out)


def main():
    port = int(sys.argv[1])
    cert, key = sys.argv[2], sys.argv[3]

    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    ctx.load_cert_chain(certfile=cert, keyfile=key)
    ctx.set_alpn_protocols(["h2"])
    # TLS 1.3 is fine: the client handles the post-handshake NewSessionTicket
    # (cn_tls.c). Set CN_H2_FORCE_TLS12=1 to pin 1.2 for the fallback path.
    import os
    if os.environ.get("CN_H2_FORCE_TLS12"):
        ctx.maximum_version = ssl.TLSVersion.TLSv1_2

    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    sock.bind(("127.0.0.1", port))
    sock.listen(8)
    sys.stderr.write("h2 test server listening on 127.0.0.1:%d\n" % port)
    sys.stderr.flush()

    while True:
        raw, peer = sock.accept()
        sys.stderr.write(">> TCP connection from %s:%d\n" % peer)
        sys.stderr.flush()
        try:
            tls = ctx.wrap_socket(raw, server_side=True)
            sys.stderr.write(">> TLS handshake ok, ALPN=%s\n" % tls.selected_alpn_protocol())
            sys.stderr.flush()
        except ssl.SSLError as e:
            sys.stderr.write(">> TLS handshake FAILED: %s\n" % e)
            sys.stderr.flush()
            raw.close()
            continue
        try:
            handle(tls)
        except (ssl.SSLError, ConnectionError, OSError):
            pass
        finally:
            try:
                tls.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass
            tls.close()


if __name__ == "__main__":
    main()
