#!/usr/bin/env python3
"""One debugger attach for toolchain/debugger-attach (bench/toolchain/Debugger.md).

    attach_once.py gdb <pid> <symbol>
    attach_once.py dlv <dlv> <pid> <symbol>
    attach_once.py jdb <port> <Class.method>
    attach_once.py debugpy <port> <file> <line>

Attaches the debugger to an already-running debuggee (started once by the
driver, outside the timed region), sets one breakpoint, waits until the
debugger reports it hit, and detaches leaving the debuggee running. Prints
`breakpoint reported <where>` and exits 0; anything else exits 1 with the
reason on stderr. cmdbench.py times this whole process per trial.
"""
import json
import socket
import subprocess
import sys
import threading
import time

TIMEOUT = 120.0


class Pipe:
    """A child with stdin/stdout pipes and a reader thread collecting output."""

    def __init__(self, argv):
        self.p = subprocess.Popen(argv, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                  stderr=subprocess.STDOUT)
        self.buf = b""
        self.lock = threading.Lock()
        threading.Thread(target=self._read, daemon=True).start()

    def _read(self):
        while True:
            b = self.p.stdout.read1(4096) if hasattr(self.p.stdout, "read1") else self.p.stdout.read(1)
            if not b:
                return
            with self.lock:
                self.buf += b

    def send(self, line):
        self.p.stdin.write(line.encode() + b"\n")
        self.p.stdin.flush()

    def wait_for(self, needle, timeout=TIMEOUT):
        end = time.monotonic() + timeout
        while time.monotonic() < end:
            with self.lock:
                if needle.encode() in self.buf:
                    return True
            if self.p.poll() is not None:
                with self.lock:
                    return needle.encode() in self.buf
            time.sleep(0.002)
        return False

    def tail(self):
        with self.lock:
            return self.buf.decode("utf-8", "replace")[-300:]

    def finish(self, timeout=30):
        try:
            self.p.stdin.close()
        except OSError:
            pass
        try:
            self.p.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            self.p.kill()
            self.p.wait()


def fail(msg):
    print("attach_once: " + msg, file=sys.stderr)
    sys.exit(1)


def gdb(pid, sym):
    r = subprocess.run(["gdb", "-nx", "-q", "-batch", "-p", pid, "-ex", "break " + sym,
                        "-ex", "continue", "-ex", "detach"],
                       capture_output=True, timeout=TIMEOUT)
    out = r.stdout.decode("utf-8", "replace")
    if "Breakpoint 1, " not in out or sym not in out:
        fail("gdb did not report the breakpoint: " + (out + r.stderr.decode())[-300:])
    print("breakpoint reported gdb %s" % sym)


def dlv(exe, pid, sym):
    d = Pipe([exe, "attach", pid, "--allow-non-terminal-interactive=true"])
    try:
        if not d.wait_for("Type 'help'"):
            fail("delve gave no prompt: " + d.tail())
        d.send("break " + sym)
        d.send("continue")
        if not d.wait_for("> [Breakpoint 1] %s()" % sym):
            fail("delve did not report the breakpoint: " + d.tail())
        d.send("clear 1")
        # quit -c needs an --accept-multiclient server; a plain attach asks
        # whether to kill the process, and "n" detaches leaving it running.
        d.send("quit")
        d.send("n")
    finally:
        d.finish()
    print("breakpoint reported delve %s" % sym)


def jdb(port, where):
    d = Pipe(["jdb", "-attach", "localhost:" + port])
    try:
        if not d.wait_for("Initializing jdb"):
            fail("jdb did not connect: " + d.tail())
        d.send("stop in " + where)
        if not d.wait_for("Breakpoint hit:"):
            fail("jdb did not report the breakpoint: " + d.tail())
        d.send("clear " + where)
        d.send("exit")
    finally:
        d.finish()
    print("breakpoint reported jdb %s" % where)


def debugpy(port, path, line):
    s = socket.create_connection(("127.0.0.1", int(port)), timeout=TIMEOUT)
    f = s.makefile("rb")
    seq = [0]

    def send(cmd, args):
        seq[0] += 1
        body = json.dumps({"seq": seq[0], "type": "request", "command": cmd,
                           "arguments": args}).encode()
        s.sendall(b"Content-Length: %d\r\n\r\n" % len(body) + body)
        return seq[0]

    def recv():
        n = 0
        while True:
            h = f.readline()
            if not h:
                raise RuntimeError("debugpy closed the connection")
            h = h.strip()
            if not h:
                break
            if h.lower().startswith(b"content-length:"):
                n = int(h.split(b":")[1])
        return json.loads(f.read(n))

    def until(pred):
        end = time.monotonic() + TIMEOUT
        while time.monotonic() < end:
            m = recv()
            if m.get("type") == "response" and not m.get("success", True):
                raise RuntimeError("%s failed: %s" % (m.get("command"), m.get("message")))
            if pred(m):
                return m
        raise TimeoutError("no matching message")

    try:
        i = send("initialize", {"adapterID": "debugpy", "clientID": "bench",
                                "linesStartAt1": True, "columnsStartAt1": True, "pathFormat": "path"})
        until(lambda m: m.get("type") == "response" and m.get("request_seq") == i)
        send("attach", {"justMyCode": False, "subProcess": False})
        until(lambda m: m.get("type") == "event" and m.get("event") == "initialized")
        b = send("setBreakpoints", {"source": {"path": path}, "breakpoints": [{"line": int(line)}]})
        until(lambda m: m.get("type") == "response" and m.get("request_seq") == b)
        send("configurationDone", {})
        ev = until(lambda m: m.get("type") == "event" and m.get("event") == "stopped")
        if ev.get("body", {}).get("reason") != "breakpoint":
            raise RuntimeError("stopped for %r" % ev.get("body", {}).get("reason"))
        b = send("setBreakpoints", {"source": {"path": path}, "breakpoints": []})
        until(lambda m: m.get("type") == "response" and m.get("request_seq") == b)
        d = send("disconnect", {"terminateDebuggee": False, "restart": False})
        until(lambda m: m.get("type") == "response" and m.get("request_seq") == d)
    except Exception as e:  # noqa: BLE001
        fail("debugpy: %s" % e)
    finally:
        s.close()
    print("breakpoint reported debugpy %s:%s" % (path.rsplit("/", 1)[-1], line))


def main():
    a = sys.argv[1:]
    if not a:
        sys.exit(__doc__)
    if a[0] == "gdb":
        gdb(a[1], a[2])
    elif a[0] == "dlv":
        dlv(a[1], a[2], a[3])
    elif a[0] == "jdb":
        jdb(a[1], a[2])
    elif a[0] == "debugpy":
        debugpy(a[1], a[2], a[3])
    else:
        sys.exit(__doc__)


if __name__ == "__main__":
    main()
