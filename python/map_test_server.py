"""
Servidor HTTPS de prueba para la Capa B (descarga de tiles por zona).

Sirve, desde el directorio tiles/, el `base_url` que el firmware espera:
  GET /catalog.json
  GET /zones/<id>.json
  GET /tiles/<file>.bin

Uso:
  # 1) generar cert self-signed (una vez):
  #    python map_test_server.py --gen-cert
  # 2) (re)generar el catalogo a partir de los tiles:
  #    python make_catalog.py
  # 3) levantar el server:
  #    python map_test_server.py --host 0.0.0.0 --port 8443

Flags de testing:
  --print-cert     imprime el PEM del cert (para embeberlo en el firmware: MAP_SYNC_TEST_CERT)
  --corrupt NAME   sirve el tile <NAME> con bytes alterados (para probar el rechazo por sha256)
  --bump-version   re-genera el catalogo (sube la version de las zonas que cambiaron)

Prueba rapida:
  curl --cacert server.crt https://localhost:8443/catalog.json
"""
import os
import ssl
import json
import argparse
import subprocess
from http.server import ThreadingHTTPServer, SimpleHTTPRequestHandler

HERE = os.path.dirname(os.path.abspath(__file__))
TILES_DIR = os.path.join(HERE, "tiles")
CERT = os.path.join(HERE, "server.crt")
KEY = os.path.join(HERE, "server.key")

CORRUPT = set()  # nombres de tile a corromper (sin path)


def gen_cert(host):
    """Genera un cert self-signed valido para localhost + el host dado."""
    san = "subjectAltName=DNS:localhost,IP:127.0.0.1"
    if host and host not in ("0.0.0.0", "localhost", "127.0.0.1"):
        san += f",IP:{host}"
    subprocess.run([
        "openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes",
        "-keyout", KEY, "-out", CERT, "-days", "825",
        "-subj", "/CN=mykeego-map-test",
        "-addext", san,
    ], check=True)
    print(f"Cert generado: {CERT} (key: {KEY}, SAN: {san})")


class Handler(SimpleHTTPRequestHandler):
    def __init__(self, *args, **kwargs):
        super().__init__(*args, directory=TILES_DIR, **kwargs)

    def log_message(self, fmt, *args):
        print("  " + (fmt % args))

    def do_GET(self):
        # Inyectar corrupcion para tiles marcados
        name = os.path.basename(self.path.split("?")[0])
        if name in CORRUPT and name.endswith(".bin"):
            path = os.path.join(TILES_DIR, name)
            try:
                with open(path, "rb") as f:
                    data = bytearray(f.read())
            except FileNotFoundError:
                self.send_error(404)
                return
            if data:
                data[0] ^= 0xFF  # corromper 1 byte -> sha256 falla
            self.send_response(200)
            self.send_header("Content-Type", "application/octet-stream")
            self.send_header("Content-Length", str(len(data)))
            self.end_headers()
            self.wfile.write(bytes(data))
            print(f"  [CORRUPT] served {name}")
            return
        super().do_GET()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="0.0.0.0")
    ap.add_argument("--port", type=int, default=8443)
    ap.add_argument("--gen-cert", action="store_true")
    ap.add_argument("--print-cert", action="store_true")
    ap.add_argument("--corrupt", action="append", default=[],
                    help="nombre de tile a corromper, ej: tile_-34455_-58509.bin")
    ap.add_argument("--bump-version", action="store_true",
                    help="regenera el catalogo antes de servir")
    args = ap.parse_args()

    if args.gen_cert:
        gen_cert(args.host)
        return
    if args.print_cert:
        with open(CERT) as f:
            print(f.read())
        return
    if args.bump_version:
        from make_catalog import emit_catalog
        emit_catalog()

    if not (os.path.exists(CERT) and os.path.exists(KEY)):
        print("Falta el cert. Corre primero: python map_test_server.py --gen-cert")
        return

    CORRUPT.update(args.corrupt)
    if CORRUPT:
        print("Tiles a corromper:", ", ".join(sorted(CORRUPT)))

    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    ctx.load_cert_chain(CERT, KEY)

    httpd = ThreadingHTTPServer((args.host, args.port), Handler)
    httpd.socket = ctx.wrap_socket(httpd.socket, server_side=True)
    print(f"HTTPS test server en https://{args.host}:{args.port}  (sirviendo {TILES_DIR})")
    print(f"Prueba: curl --cacert {CERT} https://localhost:{args.port}/catalog.json")
    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        print("\nbye")


if __name__ == "__main__":
    main()
