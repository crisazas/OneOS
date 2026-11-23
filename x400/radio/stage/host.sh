#!/system/bin/sh
# Runs on the Android side as root, in the session's mount namespace, in the background (guest_start.sh), after /run of the guest is mounted.
# Turns the Wi-Fi networks that One UI saved into NetworkManager keyfiles under $OUT (the guest's /run/x400-radio, a tmpfs: nothing reaches flash and
# nothing is in git). guest.sh copies them into /etc/NetworkManager/system-connections. Never prints a key.
# X400_RADIO_OUT overrides the output directory and X400_RADIO_STORE the source file (tests).
STORE=${X400_RADIO_STORE:-/data/misc/apexdata/com.android.wifi/WifiConfigStore.xml}
OUT=${X400_RADIO_OUT:-/data/local/tmp/oneos/rootfs/run/x400-radio}
BB=""
for c in /data/adb/ksu/bin/busybox /data/adb/ap/bin/busybox /data/adb/magisk/busybox; do
    [ -x "$c" ] && { BB=$c; break; }
done
[ -n "$BB" ] || { echo "radio host.sh: no busybox"; exit 1; }
umask 077
rm -rf "$OUT"; mkdir -p "$OUT" && chmod 700 "$OUT" || { echo "radio host.sh: cannot create $OUT"; exit 1; }
n=0
SEP=$(printf '\001')
if [ -r "$STORE" ]; then
    # one record per <Network>: ssid, psk (quoted passphrase or 64 hex), the security types, most-recent flag
    $BB awk -v SEP="$SEP" '
    function unq(s) { gsub(/&quot;/, "\"", s); gsub(/&apos;/, "\047", s); gsub(/&lt;/, "<", s); gsub(/&gt;/, ">", s); gsub(/&amp;/, "\\&", s); return s }
    /<Network>/ { inn = 1; ssid = ""; psk = ""; types = ""; recent = 0; hidden = 0; next }
    inn && /name="SSID"/ { s = $0; sub(/^[^>]*>/, "", s); sub(/<.*/, "", s); ssid = unq(s) }
    inn && /name="PreSharedKey"/ { s = $0; sub(/^[^>]*>/, "", s); sub(/<.*/, "", s); psk = unq(s) }
    inn && /name="SecurityType"/ { s = $0; sub(/.*value="/, "", s); sub(/".*/, "", s); types = types " " s }
    inn && /name="IsMostRecentlyConnected" value="true"/ { recent = 1 }
    inn && /name="HiddenSSID" value="true"/ { hidden = 1 }
    /<\/Network>/ { inn = 0; print ssid SEP psk SEP types SEP recent SEP hidden }
    ' "$STORE" > "$OUT/.records" 2>/dev/null
    while IFS="$SEP" read -r ssid psk types recent hidden; do
        [ -n "$ssid" ] || continue
        case "$ssid" in \"*\") ssid=${ssid#\"}; ssid=${ssid%\"} ;; *) echo "radio host.sh: skipping a network with a hex SSID"; continue ;; esac
        case "$types" in
            *" 2"*) km=wpa-psk ;;
            *" 4"*) km=sae ;;
            *" 0"*) km=none ;;
            *) echo "radio host.sh: skipping a network of an unsupported type (types:$types)"; continue ;;
        esac
        if [ "$km" != none ]; then
            case "$psk" in
                \"*\") psk=${psk#\"}; psk=${psk%\"} ;;
                *) [ ${#psk} -eq 64 ] || { echo "radio host.sh: no usable key for one network"; continue; } ;;
            esac
            [ ${#psk} -ge 8 ] || { echo "radio host.sh: key too short for one network"; continue; }
        fi
        n=$((n + 1))
        id=$(printf '%s' "$ssid" | $BB md5sum | $BB cut -c1-32)
        uuid=$(printf '%s' "$id" | $BB sed 's/^\(........\)\(....\)\(....\)\(....\)\(............\).*/\1-\2-\3-\4-\5/')
        esc() { printf '%s' "$1" | $BB sed 's/\\/\\\\/g'; }
        f="$OUT/x400-android-$n.nmconnection"
        {
            echo "[connection]"
            printf '%s\n' "id=$(esc "$ssid")"
            echo "uuid=$uuid"
            echo "type=wifi"
            echo "interface-name=wlan0"
            echo "autoconnect=true"
            [ "$recent" = 1 ] && echo "autoconnect-priority=10"
            echo
            echo "[wifi]"
            echo "mode=infrastructure"
            printf '%s\n' "ssid=$(esc "$ssid")"
            [ "$hidden" = 1 ] && echo "hidden=true"
            if [ "$km" != none ]; then
                echo
                echo "[wifi-security]"
                echo "key-mgmt=$km"
                printf '%s\n' "psk=$(esc "$psk")"
            fi
            echo
            echo "[ipv4]"
            echo "method=auto"
            echo "route-table=1014"
            echo
            echo "[ipv6]"
            echo "method=auto"
            echo "route-table=1014"
        } > "$f"
        chmod 600 "$f"
        echo "radio host.sh: wrote $(basename "$f") (security $km)"
    done < "$OUT/.records"
    rm -f "$OUT/.records"
else
    echo "radio host.sh: $STORE not readable"
fi
echo "radio host.sh: $n connection(s) in $OUT"
: > "$OUT/ready"
