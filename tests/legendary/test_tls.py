"""Which settings actually make Legendary's HTTP stack use the bundled CA file.

Legendary talks to Epic only through ``requests`` sessions (api/egs.py, api/lgd.py,
core.py).  The functional tests start a local HTTPS server (127.0.0.1 only) whose
certificate is signed by a throw-away CA created with the ``openssl`` CLI, and check
whether a request through Legendary's own session succeeds with that CA bundle configured
and fails with the default certifi bundle.
"""
from __future__ import annotations

import http.server
import os
import shutil
import ssl
import subprocess
import threading

import certifi
import pytest
import requests


@pytest.fixture
def tls_server(tmp_path):
    if not shutil.which("openssl"):
        pytest.skip("openssl CLI not available")
    d = tmp_path / "pki"
    d.mkdir()

    def sh(*args):
        subprocess.run(args, cwd=d, check=True, capture_output=True)

    sh("openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-keyout", "ca.key",
       "-out", "ca.pem", "-days", "2", "-subj", "/CN=Test CA (throw-away)",
       "-addext", "basicConstraints=critical,CA:TRUE",
       "-addext", "keyUsage=critical,keyCertSign,cRLSign")
    sh("openssl", "req", "-newkey", "rsa:2048", "-nodes", "-keyout", "srv.key", "-out", "srv.csr",
       "-subj", "/CN=localhost")
    (d / "ext.cnf").write_text("subjectAltName=DNS:localhost,IP:127.0.0.1\n"
                               "keyUsage=critical,digitalSignature,keyEncipherment\n"
                               "extendedKeyUsage=serverAuth\n")
    sh("openssl", "x509", "-req", "-in", "srv.csr", "-CA", "ca.pem", "-CAkey", "ca.key",
       "-CAcreateserial", "-out", "srv.pem", "-days", "2", "-extfile", "ext.cnf")

    class H(http.server.BaseHTTPRequestHandler):
        def do_GET(self):
            self.send_response(200)
            self.send_header("Content-Length", "2")
            self.end_headers()
            self.wfile.write(b"ok")

        def log_message(self, *a):
            pass

    srv = http.server.HTTPServer(("127.0.0.1", 0), H)
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    ctx.load_cert_chain(d / "srv.pem", d / "srv.key")
    srv.socket = ctx.wrap_socket(srv.socket, server_side=True)
    t = threading.Thread(target=srv.serve_forever, daemon=True)
    t.start()
    yield f"https://localhost:{srv.server_address[1]}/", str(d / "ca.pem")
    srv.shutdown()
    srv.server_close()


def test_configure_sets_verify_everywhere(bridge, cfg, tmp_path):
    ca = tmp_path / "bundle.pem"
    ca.write_text("placeholder")                 # only the path is inspected here
    bridge.configure(str(cfg), ca_bundle=str(ca))
    core = bridge._core()

    assert os.environ["REQUESTS_CA_BUNDLE"] == str(ca)
    assert os.environ["SSL_CERT_FILE"] == str(ca)
    url = "https://example.invalid/"
    for s in (core.egs.session, core.egs.unauth_session, core.lgdapi.session,
              requests.Session()):               # last: sessions Legendary creates ad hoc (core.py:128)
        eff = s.merge_environment_settings(url, {}, None, None, None)["verify"]
        assert eff == str(ca)
    for s in (core.egs.session, core.egs.unauth_session, core.lgdapi.session):
        assert s.verify == str(ca)


def test_default_bundle_is_certifi(bridge, cfg):
    bridge.configure(str(cfg))
    assert os.environ["REQUESTS_CA_BUNDLE"] == certifi.where()
    assert os.path.isfile(certifi.where())
    assert bridge._core().egs.session.verify == certifi.where()


def test_missing_bundle_is_an_error(bridge, cfg, tmp_path):
    with pytest.raises(bridge.NotConfiguredError):
        bridge.configure(str(cfg), ca_bundle=str(tmp_path / "nope.pem"))


def test_configured_bundle_is_really_used(bridge, cfg, tls_server, monkeypatch):
    url, test_ca = tls_server

    # default (certifi, which does not contain the throw-away CA): handshake must fail
    bridge.configure(str(cfg))
    with pytest.raises(requests.exceptions.SSLError):
        bridge._core().egs.unauth_session.get(url, timeout=5)

    # the throw-away CA as the bundle: handshake succeeds through Legendary's own sessions
    bridge.configure(str(cfg), ca_bundle=test_ca)
    core = bridge._core()
    assert core.egs.unauth_session.get(url, timeout=5).text == "ok"
    assert core.egs.session.get(url, timeout=5).text == "ok"
    assert core.lgdapi.session.get(url, timeout=5).text == "ok"
    assert requests.Session().get(url, timeout=5).text == "ok"   # env var reaches new sessions


def test_ssl_cert_file_alone_does_not_affect_requests(tls_server, monkeypatch):
    """Evidence for docs/notes/auth.md: SSL_CERT_FILE is not what makes requests trust a CA."""
    url, test_ca = tls_server
    monkeypatch.delenv("REQUESTS_CA_BUNDLE", raising=False)
    monkeypatch.delenv("CURL_CA_BUNDLE", raising=False)
    monkeypatch.setenv("SSL_CERT_FILE", test_ca)
    with pytest.raises(requests.exceptions.SSLError):
        requests.Session().get(url, timeout=5)
