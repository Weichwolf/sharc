#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Verify or retrieve pinned local manufacturer references; never build input."""

import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
from urllib.parse import urlparse
from urllib.request import Request, urlopen


ROOT = Path(__file__).resolve().parents[1]
LOCAL = ROOT / "doc/local"


def load_manifest():
    """Reject unsafe filenames, unpinned files and ambiguous document IDs."""
    path = ROOT / "doc/reference-manifest.json"
    manifest = json.loads(path.read_text())
    documents = manifest["documents"]
    if not documents:
        raise ValueError("Empty reference inventory")
    names = set()
    for row in documents:
        name = row["file"]
        if Path(name).name != name or not name.endswith(".pdf") or name in names:
            raise ValueError(f"Invalid or duplicate reference filename: {name}")
        names.add(name)
        digest = row["sha256"]
        if len(digest) != 64 or any(c not in "0123456789abcdef" for c in digest):
            raise ValueError(f"Invalid SHA-256: {name}")
        if row["bytes"] <= 0 or row["pages"] <= 0:
            raise ValueError(f"Invalid file dimensions: {name}")
        for key in ("official_url", "source_url"):
            if urlparse(row[key]).scheme != "https":
                raise ValueError(f"Non-HTTPS reference URL: {name}")
    return documents


def verify_bytes(row, data):
    """A changed publisher file needs a reviewed manifest update, never a bypass."""
    if not data.startswith(b"%PDF-"):
        raise ValueError(f"Not a PDF: {row['file']}")
    if len(data) != row["bytes"] or hashlib.sha256(data).hexdigest() != row["sha256"]:
        raise ValueError(f"Reference identity mismatch: {row['file']}")


def verify_document(row):
    path = LOCAL / row["file"]
    verify_bytes(row, path.read_bytes())
    print(f"Verified {row['file']} ({row['bytes']} bytes)")


def fetch_document(row):
    target = LOCAL / row["file"]
    if target.exists():
        verify_document(row)
        return
    LOCAL.mkdir(parents=True, exist_ok=True)
    request = Request(row["source_url"], headers={"User-Agent": "reference-fetch/1.0"})
    with urlopen(request, timeout=30) as response:
        data = response.read(row["bytes"] + 1)
    verify_bytes(row, data)
    # Atomic installation; a failed or changed download never replaces a PDF.
    with tempfile.NamedTemporaryFile(dir=LOCAL, suffix=".download", delete=False) as stream:
        temporary = Path(stream.name)
        stream.write(data)
    try:
        temporary.replace(target)
    finally:
        temporary.unlink(missing_ok=True)
    verify_document(row)


def extract_text(row):
    verify_document(row)
    executable = shutil.which("pdftotext")
    if not executable:
        raise RuntimeError("pdftotext is required for text extraction")
    source = LOCAL / row["file"]
    output = source.with_suffix(".txt")
    subprocess.run([executable, "-layout", str(source), str(output)], check=True)
    print(f"Extracted {output.relative_to(ROOT)}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("command", choices=("verify", "fetch", "text"))
    arguments = parser.parse_args()
    actions = {"verify": verify_document, "fetch": fetch_document, "text": extract_text}
    for row in load_manifest():
        actions[arguments.command](row)
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, ValueError, KeyError, RuntimeError, subprocess.CalledProcessError) as error:
        print(f"Reference error: {error}", file=sys.stderr)
        sys.exit(1)
