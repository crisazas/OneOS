#!/bin/sh
# Runs in the guest as root (session-inner.sh, `grp shortcuts`). Puts three shortcuts where the user finds them, idempotently (the root is persistent,
# so what is here from an earlier session stays and is replaced by the current text):
#   Desktop/One UI           opens One UI's shared storage (/mnt/oneui, guest_start.sh) in Dolphin
#   Desktop/Volver a One UI  asks for confirmation (return-oneui.sh) and brings One UI back
#   a "One UI" entry in Dolphin's Places panel (~/.local/share/user-places.xbel)
G=${X400_SHORTCUTS_G:-/tmp/x400/shortcuts}
D=/root/Desktop
mkdir -p "$D"
cat > "$D/oneui.desktop" <<EOT
[Desktop Entry]
Type=Application
Name=One UI
Comment=Archivos de One UI (DCIM, Download, Documents...), lectura y escritura
Icon=smartphone
Exec=dolphin /mnt/oneui
Terminal=false
Categories=Utility;
EOT
cat > "$D/volver-a-oneui.desktop" <<EOT
[Desktop Entry]
Type=Application
Name=Volver a One UI
Comment=Cierra el escritorio Linux y vuelve a One UI (pide confirmacion)
Icon=system-log-out
Exec=sh $G/return-oneui.sh
Terminal=false
Categories=System;
EOT
chmod 755 "$D/oneui.desktop" "$D/volver-a-oneui.desktop"
P=/root/.local/share/user-places.xbel
if [ -f "$P" ] && ! grep -q 'file:///mnt/oneui' "$P"; then
  python3 - "$P" <<'EOT'
import sys
p = sys.argv[1]
s = open(p).read()
entry = ''' <bookmark href="file:///mnt/oneui">
  <title>One UI</title>
  <info>
   <metadata owner="http://freedesktop.org">
    <bookmark:icon name="smartphone"/>
   </metadata>
   <metadata owner="http://www.kde.org">
    <ID>x400-oneui/0</ID>
    <isSystemItem>false</isSystemItem>
   </metadata>
  </info>
 </bookmark>
'''
i = s.rindex('</xbel>')
open(p, 'w').write(s[:i] + entry + s[i:])
EOT
fi
