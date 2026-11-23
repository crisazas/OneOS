#!/usr/bin/python3
"""Dry test of stage/btbridge.py with a pty standing in for /dev/scsc_h4_0 (no radio is touched).
Run on the PC or in the guest: python3 x400/radio/test-btbridge.py"""
import os, select, signal, subprocess, sys, tempfile, time, tty

HERE = os.path.dirname(os.path.abspath(__file__))
BRIDGE = os.path.join(HERE, "stage", "btbridge.py")
RESET = bytes([0x01, 0x03, 0x0c, 0x00])
RESET_OK = bytes([0x04, 0x0e, 0x04, 0x01, 0x03, 0x0c, 0x00])


def read_until(fd, n, timeout=3.0):
    got = b""
    end = time.monotonic() + timeout
    while len(got) < n and time.monotonic() < end:
        r, _, _ = select.select([fd], [], [], 0.2)
        if r:
            got += os.read(fd, 256)
    return got


def holds(pid, path):
    for f in os.listdir("/proc/%d/fd" % pid):
        try:
            if os.readlink("/proc/%d/fd/%s" % (pid, f)) == path:
                return True
        except OSError:
            pass
    return False


def check(name, ok, extra=""):
    print(("PASS " if ok else "FAIL ") + name + (" " + extra if extra else ""))
    if not ok:
        global failed
        failed = True


failed = False
cm, cs = os.openpty()
tty.setraw(cs)
tty.setraw(cm)
fake = os.ttyname(cs)
tmp = tempfile.mkdtemp()
ptyfile = os.path.join(tmp, "pty")

r = subprocess.run([sys.executable, BRIDGE, "--dry-run", "--device", fake], capture_output=True, text=True)
check("dry-run reports the node", r.returncode == 0 and "not opened" in r.stdout, r.stdout.strip())
r = subprocess.run([sys.executable, BRIDGE, "--dry-run", "--device", "/dev/does-not-exist"], capture_output=True, text=True)
check("dry-run fails on a missing node", r.returncode != 0)

p = subprocess.Popen([sys.executable, BRIDGE, "--device", fake, "--pty-file", ptyfile, "--trace", os.path.join(tmp, "trace")], stderr=subprocess.PIPE)
for _ in range(50):
    if os.path.exists(ptyfile):
        break
    time.sleep(0.1)
host_path = open(ptyfile).read().strip()
check("pty file written", host_path.startswith("/dev/pts/"), host_path)
check("node NOT opened before the order", not holds(p.pid, fake))
host = os.open(host_path, os.O_RDWR | os.O_NOCTTY)
tty.setraw(host)
p.send_signal(signal.SIGUSR1)
time.sleep(0.6)
check("node opened after SIGUSR1", holds(p.pid, fake))
os.write(host, RESET)
at_ctl = read_until(cm, len(RESET))
check("HCI Reset reaches the controller side", at_ctl == RESET, at_ctl.hex())
os.write(cm, RESET_OK)
at_host = read_until(host, len(RESET_OK))
check("answer comes back to the host side", at_host == RESET_OK, at_host.hex())
trace = open(os.path.join(tmp, "trace")).read().split("\n")
check("trace has both directions", "host>ctl 01030c00" in trace and "ctl>host 040e0401030c00" in trace)
big = bytes(range(256)) * 40
os.write(host, big)
got = read_until(cm, len(big), 5)
check("10 KB host->controller intact", got == big, "%d bytes" % len(got))
p.send_signal(signal.SIGUSR2)
time.sleep(0.6)
check("node closed after SIGUSR2", not holds(p.pid, fake))
p.send_signal(signal.SIGUSR1)
time.sleep(0.6)
check("node reopened on demand", holds(p.pid, fake))
p.send_signal(signal.SIGTERM)
try:
    rc = p.wait(5)
except subprocess.TimeoutExpired:
    p.kill()
    rc = None
check("SIGTERM exits cleanly (0)", rc == 0, str(rc))
check("pty file removed", not os.path.exists(ptyfile))

p = subprocess.Popen([sys.executable, BRIDGE, "--device", fake, "--pty-file", ptyfile], stderr=subprocess.DEVNULL)
time.sleep(0.8)
p.send_signal(signal.SIGHUP)
check("SIGHUP exits cleanly", p.wait(5) == 0)

p = subprocess.Popen([sys.executable, BRIDGE, "--device", fake, "--pty-file", ptyfile, "--open-now"], stderr=subprocess.DEVNULL)
time.sleep(0.8)
os.close(cm)
check("EOF on the device ends the bridge", p.wait(5) == 0)

cm, cs = os.openpty()
tty.setraw(cs)
tty.setraw(cm)
fake = os.ttyname(cs)
import threading
def controller():
    if read_until(cm, 4, 4) == RESET:
        os.write(cm, RESET_OK)
t = threading.Thread(target=controller)
t.start()
r = subprocess.run([sys.executable, BRIDGE, "--probe", "--device", fake], capture_output=True, text=True)
t.join()
check("--probe sends Reset and sees the Command Complete", r.returncode == 0 and RESET_OK.hex() in r.stdout, r.stdout.strip())
sys.exit(1 if failed else 0)
