#!/usr/bin/env python3
"""toolchain/debugger-attach (bench/toolchain/Debugger.md), the Cajeta side.

One debugger session against `cajeta dap`: start the server, launch the
program, set one source breakpoint, wait for the `stopped` event with reason
`breakpoint`, then disconnect. Prints `breakpoint reported <file>:<line>` on
success; exits non-zero otherwise.

Usage: dap_breakpoint.py <cajeta> <source-root> <entry pkg.Class.method>
                         <file.cajeta> <line> <cache-dir> [timeout-s]
"""
import json, os, subprocess, sys, threading, queue, time

def main():
    cajeta, root, entry, bpfile, line, cache = sys.argv[1:7]
    timeout = float(sys.argv[7]) if len(sys.argv) > 7 else 600.0
    p = subprocess.Popen([cajeta, "dap"], stdin=subprocess.PIPE,
                         stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
    q = queue.Queue()

    def reader():
        f = p.stdout
        while True:
            hdr = b""
            while not hdr.endswith(b"\r\n\r\n"):
                c = f.read(1)
                if not c:
                    q.put(None); return
                hdr += c
            n = 0
            for h in hdr.decode().split("\r\n"):
                if h.lower().startswith("content-length:"):
                    n = int(h.split(":")[1])
            q.put(json.loads(f.read(n)))
    threading.Thread(target=reader, daemon=True).start()
    seq = [0]

    def send(cmd, args):
        seq[0] += 1
        body = json.dumps({"seq": seq[0], "type": "request", "command": cmd,
                           "arguments": args}).encode()
        p.stdin.write(b"Content-Length: %d\r\n\r\n" % len(body) + body)
        p.stdin.flush()
        return seq[0]

    def wait(pred):
        end = time.monotonic() + timeout
        while True:
            left = end - time.monotonic()
            if left <= 0:
                raise TimeoutError("no matching DAP message")
            m = q.get(timeout=left)
            if m is None:
                raise RuntimeError("dap server closed its output")
            if m.get("type") == "response" and not m.get("success", True):
                raise RuntimeError("request %s failed: %s" % (m.get("command"), m.get("message")))
            if m.get("type") == "event" and m.get("event") == "terminated":
                if pred(m):
                    return m
                raise RuntimeError("program terminated before the breakpoint")
            if pred(m):
                return m

    rc = 1
    try:
        s = send("initialize", {"adapterID": "cajeta", "clientID": "bench"})
        wait(lambda m: m.get("type") == "response" and m.get("request_seq") == s)
        s = send("launch", {"sourceRoot": root, "entry-method": entry, "cacheDir": cache,
                            "stopOnEntry": False})
        wait(lambda m: m.get("type") == "response" and m.get("request_seq") == s)
        s = send("setBreakpoints", {"source": {"path": bpfile},
                                    "breakpoints": [{"line": int(line)}]})
        wait(lambda m: m.get("type") == "response" and m.get("request_seq") == s)
        send("configurationDone", {})
        ev = wait(lambda m: m.get("type") == "event" and m.get("event") == "stopped")
        reason = ev.get("body", {}).get("reason")
        if reason != "breakpoint":
            raise RuntimeError("stopped for %r, not a breakpoint" % reason)
        print("breakpoint reported %s:%s" % (os.path.basename(bpfile), line), flush=True)
        rc = 0
    except Exception as e:
        print("dap_breakpoint: %s" % e, file=sys.stderr)
    finally:
        try:
            send("disconnect", {"terminateDebuggee": True})
        except Exception:
            pass
        try:
            p.wait(timeout=20)
        except subprocess.TimeoutExpired:
            p.kill(); p.wait()
    sys.exit(rc)

if __name__ == "__main__":
    main()
