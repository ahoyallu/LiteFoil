#!/usr/bin/env python3
"""
Encrypt a LiteFoil remote JSON as a text-safe JSON envelope.

Example:
  tools/encrypt_remote_json.py \
    --public-key tools/index_public.pem \
    --input catalog.json \
    --output catalog.encrypted.json

The output can be pasted into a raw text host for builds that provide an
out-of-tree decryptor. The stock project keeps only the public key.

Dependencies:
  pip install cryptography
"""

from __future__ import annotations

import argparse
import base64
import json
import os
import struct
import sys
import zlib
from pathlib import Path

try:
    from cryptography.hazmat.primitives import hashes, serialization
    from cryptography.hazmat.primitives.asymmetric import padding
    from cryptography.hazmat.primitives.ciphers.aead import AESGCM
except ImportError:
    sys.exit("Missing dependency: run  pip install cryptography")


ENVELOPE_VERSION = 1
ALGORITHM = "RSA-OAEP-SHA256+A256GCM"
BASE64_ZLIB_PREFIX = "EZ-A256GCM-v1:"
BASE64_ZLIB_MAGIC = b"LFZLv1\0\0"


def b64(data: bytes) -> str:
    return base64.b64encode(data).decode("ascii")


def read_json_text(path: str | None) -> str:
    if path is None or path == "-":
        return sys.stdin.read()
    return Path(path).read_text(encoding="utf-8")


def write_text(path: str | None, text: str) -> None:
    if path is None or path == "-":
        sys.stdout.write(text)
        sys.stdout.write("\n")
        return
    Path(path).write_text(text + "\n", encoding="utf-8")


def canonical_json(raw: str) -> bytes:
    try:
        parsed = json.loads(raw)
    except json.JSONDecodeError as exc:
        sys.exit(f"Input is not valid JSON: {exc}")
    return json.dumps(parsed, ensure_ascii=False, separators=(",", ":")).encode("utf-8")


def load_public_key(path: str):
    pem = Path(path).read_bytes()
    key = serialization.load_pem_public_key(pem)
    if not hasattr(key, "encrypt"):
        sys.exit(f"Public key does not support encryption: {path}")
    return key


def encrypt_json_envelope(public_key_path: str, raw_json: str) -> dict:
    public_key = load_public_key(public_key_path)
    plaintext = canonical_json(raw_json)

    aes_key = AESGCM.generate_key(bit_length=256)
    nonce = os.urandom(12)
    encrypted = AESGCM(aes_key).encrypt(nonce, plaintext, None)
    ciphertext = encrypted[:-16]
    tag = encrypted[-16:]

    wrapped_key = public_key.encrypt(
        aes_key,
        padding.OAEP(
            mgf=padding.MGF1(algorithm=hashes.SHA256()),
            algorithm=hashes.SHA256(),
            label=None,
        ),
    )

    return {
        "encrypted_json": ENVELOPE_VERSION,
        "alg": ALGORITHM,
        "key": b64(wrapped_key),
        "nonce": b64(nonce),
        "tag": b64(tag),
        "ciphertext": b64(ciphertext),
    }


def encrypt_base64_zlib(public_key_path: str, raw_json: str) -> str:
    public_key = load_public_key(public_key_path)
    plaintext = canonical_json(raw_json)
    compressed = zlib.compress(plaintext)

    aes_key = AESGCM.generate_key(bit_length=256)
    nonce = os.urandom(12)
    encrypted = AESGCM(aes_key).encrypt(nonce, compressed, None)
    ciphertext = encrypted[:-16]
    tag = encrypted[-16:]

    wrapped_key = public_key.encrypt(
        aes_key,
        padding.OAEP(
            mgf=padding.MGF1(algorithm=hashes.SHA256()),
            algorithm=hashes.SHA256(),
            label=None,
        ),
    )

    header = struct.pack(
        ">8sBHBBI",
        BASE64_ZLIB_MAGIC,
        0,
        len(wrapped_key),
        len(nonce),
        len(tag),
        len(ciphertext),
    )
    blob = header + wrapped_key + nonce + tag + ciphertext
    return BASE64_ZLIB_PREFIX + b64(blob)


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Encrypt a remote LiteFoil JSON using an RSA public key."
    )
    parser.add_argument(
        "--public-key",
        required=True,
        help="Path to the RSA public key PEM, for example tools/index_public.pem.",
    )
    parser.add_argument(
        "--input",
        "-i",
        help="Input JSON file. Omit or use '-' to read from stdin.",
    )
    parser.add_argument(
        "--output",
        "-o",
        help="Output encrypted JSON file. Omit or use '-' to write to stdout.",
    )
    parser.add_argument(
        "--format",
        choices=("json-envelope", "base64-zlib"),
        default="json-envelope",
        help="Output format. Defaults to the legacy JSON envelope.",
    )
    parser.add_argument(
        "--pretty",
        action="store_true",
        help="Pretty-print the JSON envelope. Ignored for base64-zlib.",
    )
    args = parser.parse_args()

    raw_json = read_json_text(args.input)
    if args.format == "base64-zlib":
        output = encrypt_base64_zlib(args.public_key, raw_json)
    else:
        envelope = encrypt_json_envelope(args.public_key, raw_json)
        if args.pretty:
            output = json.dumps(envelope, ensure_ascii=True, indent=2)
        else:
            output = json.dumps(envelope, ensure_ascii=True, separators=(",", ":"))
    write_text(args.output, output)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
