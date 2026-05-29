#!/usr/bin/env python3
"""
git-lfs standalone custom transfer agent backed by a local folder.

This keeps large binaries (the video-bundling research PDFs, etc.) out of
GitHub: the LFS pointer is committed to the repo as usual, but the actual
object bytes are copied to / from a local directory on this machine instead
of being uploaded to a remote LFS server.

It implements the git-lfs custom transfer protocol (newline-delimited JSON on
stdin, newline-delimited JSON responses on stdout). Objects are stored, using
git-lfs' own sharding, at:

    <STORE>/<oid[0:2]>/<oid[2:4]>/<oid>

Wired up via (see .gitattributes + repo-local git config):

    git config lfs.standalonetransferagent     lfs-local
    git config lfs.customtransfer.lfs-local.path <abs path to this script>
    git config lfs.customtransfer.lfs-local.args <abs path to local store dir>

Reference: https://github.com/git-lfs/git-lfs/blob/main/docs/custom-transfers.md
"""

import json
import os
import shutil
import sys
import tempfile


def respond(obj):
    """Write one protocol message and flush so git-lfs sees it immediately."""
    sys.stdout.write(json.dumps(obj) + "\n")
    sys.stdout.flush()


def object_path(store, oid):
    return os.path.join(store, oid[:2], oid[2:4], oid)


def handle_upload(store, msg):
    oid = msg["oid"]
    src = msg["path"]
    dst = object_path(store, oid)
    try:
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        # Copy to a temp file in the same dir, then atomically rename, so a
        # killed transfer never leaves a half-written object in the store.
        tmp = dst + ".tmp"
        shutil.copyfile(src, tmp)
        os.replace(tmp, dst)
        respond({"event": "complete", "oid": oid})
    except Exception as exc:  # noqa: BLE001 - report any failure to git-lfs
        respond({"event": "complete", "oid": oid,
                 "error": {"code": 1, "message": "upload failed: %s" % exc}})


def handle_download(store, msg):
    oid = msg["oid"]
    src = object_path(store, oid)
    try:
        if not os.path.isfile(src):
            raise FileNotFoundError("object %s not found in local store" % oid)
        fd, tmp = tempfile.mkstemp(prefix="lfs-local-")
        os.close(fd)
        shutil.copyfile(src, tmp)
        # git-lfs moves this temp file into .git/lfs/objects itself.
        respond({"event": "complete", "oid": oid, "path": tmp})
    except Exception as exc:  # noqa: BLE001
        respond({"event": "complete", "oid": oid,
                 "error": {"code": 2, "message": "download failed: %s" % exc}})


def main():
    if len(sys.argv) < 2:
        sys.stderr.write("lfs-localstore: missing <store-dir> argument\n")
        return 1
    store = os.path.abspath(sys.argv[1])

    for line in sys.stdin:
        line = line.strip()
        if not line:
            continue
        try:
            msg = json.loads(line)
        except json.JSONDecodeError:
            continue

        event = msg.get("event")
        if event == "init":
            os.makedirs(store, exist_ok=True)
            respond({})              # empty object == init succeeded
        elif event == "upload":
            handle_upload(store, msg)
        elif event == "download":
            handle_download(store, msg)
        elif event == "terminate":
            break
        # Any other event is ignored per protocol.
    return 0


if __name__ == "__main__":
    sys.exit(main())
