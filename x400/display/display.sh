#!/system/bin/sh
# linux-x400: shows a frame file on a layer above One UI. Usage: display.sh DIR FILE W H [Z]   (DIR holds the dex and the .so)
D=$1; shift
exec env CLASSPATH=$D/x400-display.dex /system/bin/app_process /system/bin X400Display $D/libx400display.so "$@"
