"""MkDocs hooks for inserting fail-closed generated API references."""

from __future__ import annotations

import hashlib
import ipaddress
import json
import logging
import os
from pathlib import Path
import re
import socket
import ssl
import subprocess
import tempfile
import threading
from typing import Any
from urllib.parse import urlsplit, urlunsplit
import zlib


GENERATED_REFERENCES = {
    "<!-- POSTGAMMA_GENERATED_C_API -->": "c-api.md",
    "<!-- POSTGAMMA_GENERATED_EXTENSION_API -->": "extension-api.md",
    "<!-- POSTGAMMA_GENERATED_PYTHON_API -->": "python-api.md",
}
VERSION_MARKER = "{{ POSTGAMMA_VERSION }}"
RELEASES_URL_MARKER = "{{ POSTGAMMA_RELEASES_URL }}"
RELEASE_STATE_MARKER = "{{ POSTGAMMA_RELEASE_STATE }}"


def module_dependencies(directory: Path, entries: list[str]) -> list[str]:
    """Find static relative imports in our emitted ES modules, without executing them.

    Keep dynamic imports
    (including native Wasm/data loading) on the existing verified startup path.
    """
    imports = re.compile(r"^\s*(?:import\s+(?:[^;]*?\s+from\s+)?|export\s+[^;]*?\s+from\s+)"
                         r"['\"]([^'\"]+)['\"]", re.MULTILINE)
    root, seen = directory.resolve(), set()

    def visit(path: Path) -> None:
        path = path.resolve()
        if not path.is_relative_to(root) or not path.is_file():
            raise ValueError("invalid documentation module dependency: " + str(path))
        name = path.relative_to(root).as_posix()
        if name in seen:
            return
        seen.add(name)
        for specifier in imports.findall(path.read_text(encoding="utf-8")):
            if specifier.startswith(("./", "../")):
                visit(path.parent / specifier)

    for entry in entries:
        visit(root / entry)
    return sorted(seen)


def on_page_context(context: dict, page: Any, config: Any, nav: Any) -> dict:
    """Prepare parallel imports of the homepage's small, main-thread modules."""
    if not page.is_homepage:
        return context
    root = Path(config.config_file_path).resolve().parent
    directory = Path(os.environ.get("POSTGAMMA_DOCS_WASM_DIR", root / "build/docs/browser"))
    package = directory / "assets/wasm"
    metadata = json.loads((package / "demo.json").read_text())
    modules = module_dependencies(root / "docs", ["assets/javascripts/psql-terminal.js"])
    if metadata.get("available"):
        # Worker entry points install message handlers and must only execute in
        # Workers. The public SDK entry and terminal only define their APIs.
        modules += ["assets/wasm/" + name for name in module_dependencies(package, ["src/index.js"])]
    context["live_modules"] = modules
    return context


class ReloadingTLSContext:
    """Use renewed certificate files for new connections without restarting MkDocs."""

    def __init__(self, cert: Path, key: Path):
        self.cert, self.key = cert, key
        self.lock = threading.Lock()
        self.stamp = self.file_stamp()
        self.context = self.load()
        self.failed_stamp = None
        self.last_error = None

    def file_stamp(self) -> tuple:
        return tuple((info.st_dev, info.st_ino, info.st_mtime_ns, info.st_size)
                     for info in (self.cert.stat(), self.key.stat()))

    def load(self) -> ssl.SSLContext:
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        context.minimum_version = ssl.TLSVersion.TLSv1_2
        context.load_cert_chain(self.cert, self.key)
        return context

    def current(self) -> ssl.SSLContext:
        with self.lock:
            stamp = None
            try:
                stamp = self.file_stamp()
                if stamp != self.stamp and stamp != self.failed_stamp:
                    context = self.load()
                    # A renewal may replace the certificate and key separately.
                    # Keep serving the old pair until both files are stable.
                    if self.file_stamp() == stamp:
                        self.context, self.stamp = context, stamp
                        self.failed_stamp = None
                        self.last_error = None
                        logging.getLogger("mkdocs.preview").info("Loaded renewed HTTPS certificate")
            except (OSError, ssl.SSLError) as error:
                failure = (stamp, str(error))
                if failure != self.last_error:
                    logging.getLogger("mkdocs.preview").warning(
                        "Cannot load renewed HTTPS certificate; retaining previous certificate: %s", error)
                    self.last_error = failure
                self.failed_stamp = stamp
            return self.context


def https_enabled(host: str) -> bool:
    mode = os.environ.get("POSTGAMMA_DOCS_HTTPS", "0").lower()
    if mode in {"1", "yes", "true"}:
        return True
    if mode in {"0", "no", "false"}:
        return False
    if mode != "auto":
        raise ValueError("DOCS_HTTPS must be auto, 1 or 0")
    if os.environ.get("POSTGAMMA_DOCS_TLS_CERT") or os.environ.get("POSTGAMMA_DOCS_TLS_KEY"):
        return True
    if host.lower() == "localhost":
        return False
    try:
        return not ipaddress.ip_address(host.strip("[]")).is_loopback
    except ValueError:
        return True


def certificate_names(host: str) -> list[str]:
    names = {"localhost", "127.0.0.1", "::1", socket.gethostname()}
    names.update(os.environ.get("POSTGAMMA_DOCS_TLS_HOSTS", "").replace(",", " ").split())
    names.add(host.strip("[]"))
    # Read local interface addresses; no external IP-discovery service is used.
    try:
        addresses = subprocess.run(["ip", "-j", "address", "show"], check=True,
                                   capture_output=True, text=True, timeout=5)
        for interface in json.loads(addresses.stdout):
            names.update(item["local"] for item in interface.get("addr_info", []))
    except (OSError, subprocess.SubprocessError, ValueError):
        pass
    result = set()
    for name in names:
        try:
            address = ipaddress.ip_address(name)
        except ValueError:
            if not re.fullmatch(r"[a-zA-Z0-9][a-zA-Z0-9.-]{0,252}", name):
                raise ValueError("Invalid DOCS_TLS_HOSTS name: " + name)
            result.add("DNS:" + name.lower())
        else:
            if not address.is_unspecified and not address.is_link_local:
                result.add("IP:" + str(address))
    return sorted(result)


def preview_certificate(root: Path, host: str) -> tuple[Path, Path, list[str]]:
    cert = os.environ.get("POSTGAMMA_DOCS_TLS_CERT", "")
    key = os.environ.get("POSTGAMMA_DOCS_TLS_KEY", "")
    if cert or key:
        if not cert or not key:
            raise ValueError("Set both DOCS_TLS_CERT and DOCS_TLS_KEY")
        return Path(cert).expanduser(), Path(key).expanduser(), []
    names = certificate_names(host)
    identifier = hashlib.sha256(",".join(names).encode()).hexdigest()[:16]
    directory = Path(os.environ.get("POSTGAMMA_DOCS_TLS_DIR", root / "build/docs/tls")) / identifier
    directory.mkdir(parents=True, exist_ok=True, mode=0o700)
    directory.chmod(0o700)
    cert_path, key_path = directory / "cert.pem", directory / "key.pem"
    try:
        if cert_path.is_file() and key_path.is_file():
            valid = subprocess.run(["openssl", "x509", "-in", str(cert_path),
                                    "-noout", "-checkend", "86400"], capture_output=True, timeout=10)
            if valid.returncode == 0:
                return cert_path, key_path, names
        with tempfile.TemporaryDirectory(dir=directory) as temporary:
            generated = Path(temporary)
            subprocess.run(["openssl", "req", "-x509", "-newkey", "rsa:2048", "-sha256",
                            "-nodes", "-days", "30", "-subj", "/CN=PostGamma docs preview",
                            "-addext", "subjectAltName=" + ",".join(names),
                            "-addext", "extendedKeyUsage=serverAuth",
                            "-keyout", str(generated / "key.pem"),
                            "-out", str(generated / "cert.pem")],
                           check=True, capture_output=True, timeout=30)
            (generated / "key.pem").chmod(0o600)
            (generated / "key.pem").replace(key_path)
            (generated / "cert.pem").replace(cert_path)
    except (OSError, subprocess.SubprocessError) as error:
        raise RuntimeError("Cannot generate the HTTPS preview certificate. Install openssl, "
                           "or set DOCS_TLS_CERT and DOCS_TLS_KEY.") from error
    return cert_path, key_path, names


def on_config(config: Any) -> Any:
    # MkDocs initially assigns an HTTP site_url even when the serving socket
    # will use TLS. Keep generated canonical URLs consistent on every rebuild.
    if https_enabled(config.dev_addr[0]) and config.site_url:
        config.site_url = urlunsplit(urlsplit(config.site_url)._replace(scheme="https"))
    return config


def on_files(files: Any, config: Any) -> Any:
    """Copy a verified browser package without checking generated binaries in."""
    from mkdocs.structure.files import File

    root = Path(config.config_file_path).resolve().parent
    directory = Path(os.environ.get("POSTGAMMA_DOCS_WASM_DIR", root / "build/docs/browser"))
    if not (directory / "assets/wasm/demo.json").is_file():
        raise RuntimeError("browser demo metadata is missing; run make docs-wasm")
    for path in sorted(directory.rglob("*")):
        if path.is_file() and path.suffix not in {".md", ".ts"}:
            files.append(File(path.relative_to(directory).as_posix(), str(directory), config.site_dir, False))
    return files


def on_serve(server: Any, **kwargs: Any) -> Any:
    """Local preview uses native isolation headers, retaining MkDocs live reload."""
    logger = logging.getLogger("mkdocs.preview")

    class PreviewRequestHandler(server.RequestHandlerClass):
        def log_request(self, code: Any = "-", size: Any = "-") -> None:
            if str(code) == "304":
                logger.debug('"%s" code %s', self.requestline, code)
            else:
                super().log_request(code, size)

    server.RequestHandlerClass = PreviewRequestHandler
    host, port = server.server_address[:2]
    use_https = https_enabled(host)
    if use_https:
        root = Path(kwargs["config"].config_file_path).resolve().parent
        cert, key, names = preview_certificate(root, host)
        contexts = ReloadingTLSContext(cert, key)

        class TLSRequestHandler(server.RequestHandlerClass):
            def handle(self) -> None:
                if isinstance(self.connection, ssl.SSLSocket):
                    super().handle()
                    return
                # A pasted address may arrive over HTTP on this same port.
                # Redirect before invoking the app, which requires HTTPS.
                self.raw_requestline = self.rfile.readline(65537)
                if len(self.raw_requestline) > 65536:
                    self.requestline = self.request_version = self.command = ""
                    self.send_error(414)
                    return
                if not self.raw_requestline or not self.parse_request():
                    return
                authorities = self.headers.get_all("Host", [])
                if len(authorities) != 1 or not re.fullmatch(
                        r"(?:\[[0-9a-fA-F:.]+\]|[A-Za-z0-9._-]+)(?::[0-9]{1,5})?", authorities[0]):
                    self.send_error(400, "A valid Host header is required")
                    return
                try:
                    urlsplit("https://" + authorities[0]).port
                    target = urlsplit(self.path)
                except ValueError:
                    self.send_error(400, "Invalid preview address")
                    return
                self.send_response(307)
                self.send_header("Location", urlunsplit(("https", authorities[0], target.path or "/", target.query, "")))
                self.send_header("Cache-Control", "no-store")
                self.send_header("Content-Length", "0")
                self.send_header("Connection", "close")
                self.end_headers()
                self.close_connection = True

        server.RequestHandlerClass = TLSRequestHandler
        finish_request = server.finish_request

        def accept_preview(request: Any, client_address: Any) -> None:
            # Detect the protocol and handshake in the request thread. An idle
            # socket must not block other connections used by browser Workers.
            previous_timeout = request.gettimeout()
            try:
                request.settimeout(10)
                first = request.recv(1, socket.MSG_PEEK)
                if first == b"\x16":
                    with contexts.current().wrap_socket(request, server_side=True) as secure:
                        secure.settimeout(previous_timeout)
                        finish_request(secure, client_address)
                        # Streaming gzip has no Content-Length, so clients
                        # need close_notify to distinguish a complete response
                        # from truncation. SSLSocket.close() alone omits it.
                        # Bound the wait for peers that never acknowledge it.
                        secure.settimeout(5)
                        secure.unwrap()
                elif first:
                    finish_request(request, client_address)
            except (ssl.SSLError, ConnectionError, TimeoutError) as error:
                logger.debug("Preview connection from %s closed: %s", client_address, error)

        server.finish_request = accept_preview
        server.url = urlunsplit(urlsplit(server.url)._replace(scheme="https"))
        if names:
            public_cert = cert.parent.parent / "preview.crt"
            public_cert.write_bytes(cert.read_bytes())
            logger.info("HTTPS development certificate: %s", cert)
            logger.info("Trust this certificate on the computer running the browser: %s", public_cert)
            for name in names:
                if name.startswith("IP:") and not ipaddress.ip_address(name[3:]).is_loopback:
                    address = name[3:]
                    if ":" in address:
                        address = "[" + address + "]"
                    logger.info("HTTPS preview: https://%s:%s/", address, port)
    app = server.get_app()

    def isolated(environ: Any, start_response: Any) -> Any:
        if use_https:
            environ["wsgi.url_scheme"] = "https"
        gzip_requested = False
        for encoding in environ.get("HTTP_ACCEPT_ENCODING", "").split(","):
            name, *parameters = encoding.strip().lower().split(";")
            if name.strip() == "gzip":
                try:
                    quality = next((float(p.split("=", 1)[1]) for p in parameters
                                    if p.strip().startswith("q=")), 1.0)
                    gzip_requested = quality > 0
                except ValueError:
                    pass
        compressed = False
        not_modified = False

        def respond(status: str, headers: list, exc_info: Any = None) -> Any:
            nonlocal compressed, not_modified
            headers = [(k, v) for k, v in headers if k.lower() not in {
                "cross-origin-opener-policy", "cross-origin-embedder-policy"}]
            headers.extend([("Cross-Origin-Opener-Policy", "same-origin"),
                            ("Cross-Origin-Embedder-Policy", "require-corp")])
            # The Wasm kernel is over 10 MiB uncompressed. Stream gzip for
            # static assets while preserving MkDocs' live-reload responses.
            path = environ.get("PATH_INFO", "")
            mount = getattr(server, "mount_path", "/")
            if (status.startswith("200 ") and environ.get("REQUEST_METHOD") in {"GET", "HEAD"}
                    and path.startswith(mount + "assets/wasm/")):
                # Validate each request, including after a live rebuild, while
                # allowing pthread Workers to reuse unchanged module bodies.
                asset_root = Path(server.root).resolve()
                asset = (asset_root / path[len(mount):]).resolve()
                if asset.is_relative_to(asset_root):
                    try:
                        info = asset.stat()
                    except OSError:
                        pass
                    else:
                        etag = f'W/"{info.st_mtime_ns:x}-{info.st_size:x}"'
                        headers.extend([("ETag", etag), ("Cache-Control", "no-cache")])
                        matches = {value.strip().removeprefix("W/") for value in
                                   environ.get("HTTP_IF_NONE_MATCH", "").split(",")}
                        if etag[2:] in matches or "*" in matches:
                            not_modified = True
                            status = "304 Not Modified"
                            headers = [(k, v) for k, v in headers if k.lower() != "content-length"]
                            headers.append(("Vary", "Accept-Encoding"))
            eligible = (status.startswith("200 ") and environ.get("REQUEST_METHOD") == "GET"
                        and not path.startswith("/livereload/")
                        and (path.endswith("/") or Path(path).suffix in {
                            ".html", ".css", ".js", ".mjs", ".json", ".svg", ".wasm", ".data"})
                        and not any(k.lower() == "content-encoding" for k, _ in headers))
            if eligible:
                headers.append(("Vary", "Accept-Encoding"))
            if eligible and gzip_requested:
                compressed = True
                headers = [(k, v) for k, v in headers if k.lower() != "content-length"]
                headers.append(("Content-Encoding", "gzip"))
            return start_response(status, headers, exc_info)
        response = app(environ, respond)
        if not_modified:
            if hasattr(response, "close"):
                response.close()
            return []
        if not compressed:
            return response

        def gzip_body() -> Any:
            compressor = zlib.compressobj(level=5, wbits=31)
            try:
                for chunk in response:
                    yield compressor.compress(chunk)
                yield compressor.flush()
            finally:
                if hasattr(response, "close"):
                    response.close()

        return gzip_body()

    server.set_app(isolated)
    return server


def on_page_markdown(
    markdown: str, page: Any, config: Any, files: Any
) -> str:
    """Replace one reference marker with its build-generated Markdown."""

    del page, files
    root = Path(config.config_file_path).resolve().parent
    version = (root / "VERSION").read_text(encoding="ascii").strip()
    baseline = json.loads(
        (root / "manifests/api/python-api-v1.json").read_text(encoding="utf-8")
    )
    release_state = baseline["release"]["state"]
    releases_url = os.environ.get("POSTGAMMA_RELEASES_URL", "#release-status")
    markdown = markdown.replace(VERSION_MARKER, version)
    markdown = markdown.replace(RELEASE_STATE_MARKER, release_state)
    markdown = markdown.replace(RELEASES_URL_MARKER, releases_url)
    configured = os.environ.get("POSTGAMMA_DOCS_GENERATED_DIR")
    generated = (
        Path(configured).resolve()
        if configured
        else root / "build" / "docs" / "generated"
    )
    for marker, filename in GENERATED_REFERENCES.items():
        count = markdown.count(marker)
        if count == 0:
            continue
        if count != 1:
            raise RuntimeError(f"documentation marker occurs {count} times: {marker}")
        path = generated / filename
        if not path.is_file():
            raise RuntimeError(
                f"generated API reference is missing: {path}; run make docs-api"
            )
        markdown = markdown.replace(marker, path.read_text(encoding="utf-8"))
    return markdown
