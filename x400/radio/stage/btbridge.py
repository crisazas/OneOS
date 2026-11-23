#!/usr/bin/python3
"""Relay between the Bluetooth controller's H4 node and a pty that btattach turns into hciN.

  btbridge.py --dry-run                 only checks that the node exists (never opens it)
  btbridge.py --probe                   opens the node, sends HCI_Reset, prints the answer, closes (step B3)
  btbridge.py [--open-now]              creates the pty, writes its path to --pty-file, waits; SIGUSR1 opens the node, SIGUSR2 closes it
SIGTERM, SIGHUP, SIGINT, EOF on the node or EOF on the pty close everything and exit. The node is never opened before it is ordered.
"""
import argparse, os, select, signal, stat, sys, time, tty

RESET = bytes([0x01, 0x03, 0x0c, 0x00])
RESET_OK = bytes([0x04, 0x0e, 0x04, 0x01, 0x03, 0x0c, 0x00])

want_open = False
want_close = False
stop = False


def on_usr1(*_):
    global want_open
    want_open = True


def on_usr2(*_):
    global want_close
    want_close = True


def on_stop(*_):
    global stop
    stop = True


def log(msg):
    print("btbridge: " + msg, file=sys.stderr, flush=True)


def check_node(path):
    st = os.stat(path)
    if not stat.S_ISCHR(st.st_mode) and not stat.S_ISFIFO(st.st_mode):
        raise SystemExit("btbridge: %s is not a character device" % path)
    return st


def probe(path, timeout):
    """Open, send HCI_Reset, read H4 events until the Command Complete of Reset (opcode 0x0c03) or the timeout, close.
    Controllers announce themselves with a Command Status of opcode 0 first; that is skipped, not an error."""
    fd = os.open(path, os.O_RDWR | os.O_NOCTTY)
    try:
        os.write(fd, RESET)
        buf = b""
        end = time.monotonic() + timeout
        ok = False
        while time.monotonic() < end and not ok:
            r, _, _ = select.select([fd], [], [], max(0.0, end - time.monotonic()))
            if not r:
                break
            chunk = os.read(fd, 256)
            if not chunk:
                break
            buf += chunk
            while len(buf) >= 3 and buf[0] == 0x04 and len(buf) >= 3 + buf[2]:
                ev = buf[:3 + buf[2]]
                buf = buf[len(ev):]
                print("event: " + ev.hex())
                if ev[1] == 0x0e and ev[3:6] == bytes([0x01, 0x03, 0x0c]):
                    print("reset complete, status %d" % ev[6])
                    ok = ev[6] == 0
            if buf and buf[0] != 0x04:
                print("unexpected H4 type %02x: %s" % (buf[0], buf.hex()))
                break
        return ok
    finally:
        os.close(fd)


def relay(args):
    global want_open, want_close
    master, slave = os.openpty()
    tty.setraw(slave)
    tty.setraw(master)
    slave_path = os.ttyname(slave)
    if args.pty_file:
        tmp = args.pty_file + ".tmp"
        with open(tmp, "w") as f:
            f.write(slave_path + "\n")
        os.replace(tmp, args.pty_file)
    log("pty %s, device %s not opened yet" % (slave_path, args.device))
    trace = open(args.trace, "a") if args.trace else None
    dev = None
    if args.open_now:
        want_open = True
    try:
        while not stop:
            if want_open and dev is None:
                want_open = False
                dev = os.open(args.device, os.O_RDWR | os.O_NOCTTY)
                log("opened %s" % args.device)
            if want_close and dev is not None:
                want_close = False
                os.close(dev)
                dev = None
                log("closed %s" % args.device)
            fds = [master] + ([dev] if dev is not None else [])
            try:
                r, _, _ = select.select(fds, [], [], 0.5)
            except InterruptedError:
                continue
            for fd in r:
                try:
                    data = os.read(fd, 4096)
                except OSError:
                    data = b""
                if not data:
                    log("EOF on %s" % ("pty" if fd == master else "device"))
                    return 0
                dst = dev if fd == master else master
                if dst is None:
                    continue
                if trace:
                    trace.write("%s %s\n" % ("host>ctl" if fd == master else "ctl>host", data.hex()))
                    trace.flush()
                while data:
                    n = os.write(dst, data)
                    data = data[n:]
    finally:
        if dev is not None:
            os.close(dev)
            log("closed %s" % args.device)
        os.close(master)
        os.close(slave)
        if args.pty_file:
            try:
                os.unlink(args.pty_file)
            except OSError:
                pass
    return 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--device", default="/dev/scsc_h4_0")
    ap.add_argument("--pty-file", default="/run/x400-btbridge.pty")
    ap.add_argument("--trace")
    ap.add_argument("--dry-run", action="store_true")
    ap.add_argument("--probe", action="store_true")
    ap.add_argument("--probe-timeout", type=float, default=5.0)
    ap.add_argument("--open-now", action="store_true")
    args = ap.parse_args()
    st = check_node(args.device)
    if args.dry_run:
        print("%s exists, mode %o, rdev %d:%d; not opened" % (args.device, stat.S_IMODE(st.st_mode), os.major(st.st_rdev), os.minor(st.st_rdev)))
        return 0
    if args.probe:
        return 0 if probe(args.device, args.probe_timeout) else 1
    for s, h in ((signal.SIGUSR1, on_usr1), (signal.SIGUSR2, on_usr2), (signal.SIGTERM, on_stop), (signal.SIGHUP, on_stop), (signal.SIGINT, on_stop)):
        signal.signal(s, h)
    return relay(args)


if __name__ == "__main__":
    sys.exit(main())
