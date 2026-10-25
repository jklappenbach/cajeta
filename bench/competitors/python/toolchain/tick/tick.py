# toolchain/debugger-attach debuggee (bench/toolchain/Debugger.md): a running
# CPython process, started under `python -m debugpy --listen`, that calls
# tick() every millisecond for a debugpy client to attach to and break in.
import time


def tick(n):
    return n * 3 + 1  # BREAK HERE (attach_once.py finds this line)


def main():
    print("ready", flush=True)
    acc = 0
    i = 0
    while True:
        acc += tick(i)
        i += 1
        time.sleep(0.001)


if __name__ == "__main__":
    main()
