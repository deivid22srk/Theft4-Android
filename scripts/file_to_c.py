#!/usr/bin/env python3
"""Convert a binary file into a C array + header for embedding (BIN2C).

Usage:
    file_to_c.py <input_file> <array_name> <compression_type> <output.c> <output.h>

compression_type:
    zstd       compress the payload with Zstandard
    none|raw   embed the bytes verbatim

The generated pair follows the LibertyRecomp BIN2C() convention:
    output.h:
        extern decls for the byte array and the uncompressed-size constant,
        wrapped in extern "C" guards.
    output.c:
        extern + definition of the byte array, extern + definition of the
        unsigned long long <name>_uncompressed_size constant (original input
        size before compression).

Compression backends are tried in order: the Python 3.14+ stdlib
(compression.zstd), the 'zstandard' package, then the zstd command-line tool.
"""

import subprocess
import sys
import tempfile
from pathlib import Path


def compress_zstd(data: bytes, input_path: Path) -> bytes:
    # Python 3.14+ standard library.
    try:
        from compression import zstd as _zstd  # type: ignore

        return _zstd.compress(data)
    except ImportError:
        pass

    # 'zstandard' package (PyPI).
    try:
        import zstandard  # type: ignore

        return zstandard.ZstdCompressor(level=9).compress(data)
    except ImportError:
        pass

    # zstd command-line tool.
    try:
        with tempfile.TemporaryDirectory() as tmp:
            tmp_in = Path(tmp) / "payload.bin"
            tmp_out = Path(tmp) / "payload.zst"
            tmp_in.write_bytes(data)
            subprocess.run(
                ["zstd", "-9", "-f", "-q", "-o", str(tmp_out), str(tmp_in)],
                check=True,
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
            )
            return tmp_out.read_bytes()
    except (OSError, subprocess.CalledProcessError) as exc:
        raise RuntimeError(
            "file_to_c.py: no zstd backend available "
            "(stdlib compression.zstd, 'zstandard' package or zstd CLI)"
        ) from exc


def main(argv) -> int:
    if len(argv) != 6:
        print(
            "usage: file_to_c.py <input_file> <array_name> <compression_type> "
            "<output.c> <output.h>",
            file=sys.stderr,
        )
        return 1

    input_path = Path(argv[1])
    array_name = argv[2]
    compression = argv[3].lower()
    out_c = Path(argv[4])
    out_h = Path(argv[5])

    data = input_path.read_bytes()

    if compression == "zstd":
        payload = compress_zstd(data, input_path)
    elif compression in ("", "none", "raw"):
        payload = data
    else:
        print(f"file_to_c.py: unknown compression type '{compression}'",
              file=sys.stderr)
        return 1

    size = len(payload)
    decl = (
        f"extern unsigned char {array_name}[{size}];\n"
        f"extern unsigned long long {array_name}_uncompressed_size;\n"
    )

    out_h.write_text(
        "#ifdef __cplusplus\n"
        '  extern "C" {\n'
        "#endif\n"
        f"{decl}"
        "#ifdef __cplusplus\n"
        "  }\n"
        "#endif\n"
    )

    items = ", ".join(str(byte) for byte in payload)
    out_c.write_text(
        f"extern unsigned char {array_name}[{size}];\n"
        f"unsigned char {array_name}[{size}] = {{{items}}};\n"
        f"extern unsigned long long {array_name}_uncompressed_size;\n"
        f"unsigned long long {array_name}_uncompressed_size = {len(data)};\n"
    )

    print(
        f"file_to_c.py: {input_path.name} ({len(data)} bytes) -> "
        f"{out_c.name}/{out_h.name} ({size} bytes, {compression or 'raw'})"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
