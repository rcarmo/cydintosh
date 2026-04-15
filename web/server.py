#!/usr/bin/env python3
from http.server import ThreadingHTTPServer, SimpleHTTPRequestHandler
from pathlib import Path
import os

ROOT = Path(__file__).resolve().parent
HOST = os.environ.get("CYDINTOSH_FLASHER_HOST", "127.0.0.1")
PORT = int(os.environ.get("CYDINTOSH_FLASHER_PORT", "8765"))


class FlasherHandler(SimpleHTTPRequestHandler):
    def __init__(self, *args, **kwargs):
        super().__init__(*args, directory=str(ROOT), **kwargs)


def main() -> None:
    os.chdir(ROOT)
    server = ThreadingHTTPServer((HOST, PORT), FlasherHandler)
    print(f"Serving Cydintosh flasher from {ROOT} at http://{HOST}:{PORT}/")
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()


if __name__ == "__main__":
    main()
