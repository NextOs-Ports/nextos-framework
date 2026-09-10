#!/usr/bin/env python3
"""Extract the 1.2.7d shop catalog and build its offline IAB response."""

from __future__ import annotations

import argparse
import hashlib
import json
import mmap
import os
from pathlib import Path
import tempfile


MAIN_OBB_SHA256 = "276c413051b3349e7738afb23521f972d085a186cb22ab18db230906aab46981"
CATALOG_NAME = "IapLocalData_Google.json"
CATALOG_SIZE = 9577
CATALOG_SHA256 = "93aecb98da97508352af354f04d42e8b5b4b3b50d993dc9b300002941c77721f"
CATALOG_PREFIX = b'{\n\t"items":[\n\t{\n\t\t"billing_methods":['
OFFLINE_NAME = "IapStoreItems_Offline.json"
OFFLINE_SIZE = 4717
OFFLINE_SHA256 = "c857e6f3b2685bf514dc53af345db43059b2e4c44fe3d4add28059e973c029b9"


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        while chunk := stream.read(8 * 1024 * 1024):
            digest.update(chunk)
    return digest.hexdigest()


def atomic_write(path: Path, payload: bytes) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.NamedTemporaryFile(dir=path.parent, delete=False) as stream:
        temporary = Path(stream.name)
        stream.write(payload)
        stream.flush()
        os.fsync(stream.fileno())
    temporary.chmod(0o644)
    os.replace(temporary, path)


def extract_catalog(main_obb: Path) -> bytes:
    if sha256_file(main_obb) != MAIN_OBB_SHA256:
        raise SystemExit("main OBB SHA-256 does not match Android 1.2.7d")

    with main_obb.open("rb") as stream, mmap.mmap(
        stream.fileno(), 0, access=mmap.ACCESS_READ
    ) as mapped:
        offset = mapped.find(CATALOG_PREFIX)
        if offset < 0 or mapped.find(CATALOG_PREFIX, offset + 1) >= 0:
            raise SystemExit("the exact 1.2.7d shop catalog was not found uniquely")
        payload = mapped[offset : offset + CATALOG_SIZE]

    if hashlib.sha256(payload).hexdigest() != CATALOG_SHA256:
        raise SystemExit("embedded shop catalog failed its SHA-256 check")
    return payload


def build_offline_response(catalog: bytes) -> bytes:
    source = json.loads(catalog)
    items = source.get("items")
    if not isinstance(items, list) or len(items) != 18:
        raise SystemExit("shop catalog does not contain the expected 18 IAB items")

    response: list[dict[str, object]] = []
    for item in items:
        methods = item.get("billing_methods")
        if not isinstance(methods, list) or len(methods) != 1:
            raise SystemExit("shop item does not have exactly one billing method")
        method = methods[0]
        if method.get("name") != "googleplay":
            raise SystemExit("shop item is not the expected Google Play entry")
        response.append(
            {
                "title": item["name"],
                "price": method["display_price"],
                "type": "inapp",
                "price_amount_micros": round(method["price"] * 1_000_000),
                "description": item["description"],
                "productId": item["entry_id"],
                "price_currency_code": method["currency"],
            }
        )

    payload = (json.dumps({"store_items": response}, indent=2) + "\n").encode()
    if len(payload) != OFFLINE_SIZE:
        raise SystemExit("generated offline IAB response has an unexpected size")
    if hashlib.sha256(payload).hexdigest() != OFFLINE_SHA256:
        raise SystemExit("generated offline IAB response failed its SHA-256 check")
    return payload


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("main_obb", type=Path)
    parser.add_argument("output_directory", type=Path)
    args = parser.parse_args()

    catalog = extract_catalog(args.main_obb)
    offline = build_offline_response(catalog)
    atomic_write(args.output_directory / CATALOG_NAME, catalog)
    atomic_write(args.output_directory / OFFLINE_NAME, offline)
    print(f"OK: {CATALOG_NAME} sha256={CATALOG_SHA256}")
    print(f"OK: {OFFLINE_NAME} sha256={OFFLINE_SHA256}")


if __name__ == "__main__":
    main()
