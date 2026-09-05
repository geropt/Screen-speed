#!/usr/bin/env python3
"""Reconstruye el stream de bytes a partir de un log de terminal del Ruptela.

Los logs de campo no son capturas crudas: el programa de terminal les agrega un
encabezado, prefija cada línea con `HH:MM:SS.mmm, ` y **escapa los bytes NUL como el
texto `<0>`**. Este script deshace las tres cosas para poder alimentar el framing
compartido con los mismos bytes que vio el dispositivo.

    python3 tools/capture_to_stream.py campo.log campo.bin

LIMITACIÓN, y es importante: el escapado es **ambiguo**. Un record binario que contenga
la secuencia 0x3C 0x30 0x3E (`<0>` en ASCII) es indistinguible de un NUL escapado, así
que la reconstrucción es de mejor esfuerzo, no exacta. Con las capturas disponibles el
resultado se valida solo —los checksums NMEA cierran— pero un log distinto podría
contener el caso ambiguo.

La otra limitación: el terminal corta línea donde el stream traía `\\n`, así que se
reinserta uno por línea. Eso es correcto para NMEA y para records que contengan 0x0A,
pero no hay forma de distinguir un `\\n` del stream de uno agregado por el terminal en
un límite de lectura.
"""

import argparse
import re
import sys

TS = re.compile(rb"^\d\d:\d\d:\d\d\.\d\d\d, ")
HEADER_LINES = 3          # "Terminal log file", "Date: ...", separador


def reconstruct(raw: bytes) -> bytes:
    lines = raw.split(b"\n")
    out = bytearray()
    for line in lines[HEADER_LINES:]:
        m = TS.match(line)
        out += line[m.end():] if m else line
        out += b"\n"
    return bytes(out).replace(b"<0>", b"\x00")


def nmea_selfcheck(data: bytes):
    """Valida los checksums NMEA: si cierran, la reconstrucción es creíble."""
    ok = bad = 0
    for m in re.finditer(rb"\$([^*\r\n$]{1,90})\*([0-9A-Fa-f]{2})", data):
        calc = 0
        for b in m.group(1):
            calc ^= b
        if calc == int(m.group(2), 16):
            ok += 1
        else:
            bad += 1
    return ok, bad


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("log")
    ap.add_argument("output")
    args = ap.parse_args()

    raw = open(args.log, "rb").read()
    data = reconstruct(raw)
    ok, bad = nmea_selfcheck(data)

    open(args.output, "wb").write(data)
    print(f"{args.output}: {len(data)} bytes")
    print(f"  NUL desescapados: {data.count(0)}")
    print(f"  checksums NMEA: {ok} válidos, {bad} inválidos")
    if ok == 0:
        print("  ERROR: ningún checksum cierra; el formato del log no es el esperado",
              file=sys.stderr)
        return 1
    if bad > ok * 0.05:
        print("  AVISO: más del 5 % de checksums fallan; revisar la reconstrucción",
              file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
