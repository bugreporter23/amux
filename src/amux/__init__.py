from pathlib import Path

__version__ = Path(__file__).with_name("VERSION").read_text().strip()
PROTOCOL_VERSION = int(Path(__file__).with_name("PROTOCOL").read_text())


def envelope(message):
    return {"protocol": PROTOCOL_VERSION, "version": __version__, **message}
