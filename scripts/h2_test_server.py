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


def respond(conn, sid, method, path, auth, body):
    # Echo any request body back (so the POST integration test can assert the
    # body round-tripped); GET and other body-less requests get the default BODY.
    out_body = body if body else BODY
    sys.stderr.write(">> h2 request from client: %s %s (authority %s) "
                     "[%d body bytes] -> 200 [%d echoed]\n"
                     % (method, path, auth, len(body), len(out_body)))
    sys.stderr.flush()
    conn.send_headers(sid, [
        (":status", "200"),
        ("content-type", "application/octet-stream"),
        ("content-length", str(len(out_body))),
    ])
    conn.send_data(sid, out_body, end_stream=True)


def handle(conn_sock):
    config = h2.config.H2Configuration(client_side=False)
    conn = h2.connection.H2Connection(config=config)
    conn.initiate_connection()
    conn_sock.sendall(conn.data_to_send())

    streams = {}   # stream_id -> {"hdrs": dict, "body": bytearray}

    while True:
        data = conn_sock.recv(65535)
        if not data:
            return
        events = conn.receive_data(data)
        for event in events:
            if isinstance(event, h2.events.RequestReceived):
                hdrs = dict(event.headers)
                streams[event.stream_id] = {"hdrs": hdrs, "body": bytearray()}
            elif isinstance(event, h2.events.DataReceived):
                st = streams.get(event.stream_id)
                if st is not None:
                    st["body"].extend(event.data)
                conn.acknowledge_received_data(
                    event.flow_controlled_length, event.stream_id)
            elif isinstance(event, h2.events.StreamEnded):
                st = streams.pop(event.stream_id, None)
                if st is None:
                    continue
                hdrs = st["hdrs"]
                respond(conn,
                        event.stream_id,
                        hdrs.get(b":method", b"?").decode(),
                        hdrs.get(b":path", b"?").decode(),
                        hdrs.get(b":authority", b"?").decode(),
                        bytes(st["body"]))
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
            sys.stderr.write(">> TLS handshake ok, version=%s ALPN=%s\n"
                             % (tls.version(), tls.selected_alpn_protocol()))
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
