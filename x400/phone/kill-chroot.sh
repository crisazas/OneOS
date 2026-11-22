#!/bin/sh
# Kills only the processes whose root is the given chroot directory (the chroot shares the PID namespace with the host).
# Usage: kill-chroot.sh /abs/path/of/chroot [signal]
R=$(readlink -f "$1"); S=${2:-TERM}
for p in /proc/[0-9]*; do
  r=$(readlink "$p/root" 2>/dev/null) || continue
  [ "$r" = "$R" ] && kill -"$S" "${p#/proc/}" 2>/dev/null
done
