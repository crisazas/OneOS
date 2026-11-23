#!/bin/sh
# Asks, and if the answer is yes asks OneOS to bring One UI back. It writes the request file OneOS's own guest app writes for "Back to One UI"
# (oneos_return, read by the session watcher in oneos_switch.sh: any content but "tile" is the app request, the same as the volume keys);
# guest_start.sh binds the folder that holds it at /run/x400-return. Run from the desktop launcher: sh /tmp/x400/shortcuts/return-oneui.sh
REQ=/run/x400-return
if [ ! -d "$REQ" ]; then
  kdialog --title "Volver a One UI" --error "No se puede pedir el regreso desde aquí: falta $REQ.\nUsa Vol+ y Vol- durante 3 s."
  exit 1
fi
kdialog --title "Volver a One UI" --warningyesno "Se cerrará el escritorio Linux y volverá One UI (tarda unos 2 minutos, con la pantalla en negro).\n\nLo que tengas sin guardar se perderá. Los paquetes y ajustes ya guardados se conservan.\n\n¿Volver a One UI ahora?" || exit 0
echo app > "$REQ/oneos_return"
