#!/system/bin/sh
# linux-x400: reads `getevent -lq` lines on stdin and creates $1/return.request when Volume Up and Volume Down have both been
# held for 3 s (OneOS's way back). Only reads; never grabs a key device. Usage: getevent -lq DEV | x400_keywatch.sh SESSION_DIR
S=$1
rm -f "$S/.vu" "$S/.vd"
while read -r typ code val rest; do
  [ "$typ" = EV_KEY ] || continue
  case "$code" in
    KEY_VOLUMEUP)   f="$S/.vu" ;;
    KEY_VOLUMEDOWN) f="$S/.vd" ;;
    *) continue ;;
  esac
  if [ "$val" = DOWN ]; then
    : > "$f"
    if [ -e "$S/.vu" ] && [ -e "$S/.vd" ]; then
      ( sleep 3; [ -e "$S/.vu" ] && [ -e "$S/.vd" ] && : > "$S/return.request" ) &
    fi
  elif [ "$val" = UP ]; then
    rm -f "$f"
  fi
done
