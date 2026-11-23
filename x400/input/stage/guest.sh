#!/bin/sh
# Runs in the guest after Plasma is up (G0's session-inner.sh: `sh /tmp/x400/input/guest.sh &`). inputd itself is started by session-inner.sh.
G=/tmp/x400/input
exec /tmp/keys > /tmp/keys.log 2>&1
