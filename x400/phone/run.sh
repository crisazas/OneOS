# Siempre dentro de un espacio de montajes propio: lo montado desaparece al salir. Uso: sh chrwrap.sh [VAR=valor ...]
exec /data/adb/ksu/bin/busybox unshare -m sh /data/local/tmp/chr2.sh
