#!/bin/bash
# Install the encoder as a root-owned systemd unit that the desktop user may
# start without a password, so ipadcast can bring the encoder up when a viewer
# connects. The unit is NOT enabled: nothing loads at boot.
#
#   sudo t8103/service/install-service.sh install    (after t8103/build-modules.sh)
#   sudo t8103/service/install-service.sh remove
#
# Rerun `install` after a kernel update (modules are per kernel release).
set -euo pipefail
[ "$(id -u)" = 0 ] || { echo "run with sudo"; exit 1; }
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../.." && pwd)
U=${SUDO_USER:?run through sudo, not as root directly}
REL=$(uname -r)
LIB=/usr/local/lib/apple-ave
RULE=/etc/polkit-1/rules.d/49-apple-ave.rules
case "${1:-}" in
install)
    for m in "$REPO/driver/apple-ave.ko" "$REPO/test/ave-overlay.ko"; do
        [ -f "$m" ] || { echo "ERROR: $m missing, run t8103/build-modules.sh"; exit 1; }
        modinfo -F vermagic "$m" | grep -q "^$REL " || { echo "ERROR: $m is built for another kernel"; exit 1; }
    done
    install -Dm644 "$REPO/driver/apple-ave.ko" "$LIB/$REL/apple-ave.ko"
    install -Dm644 "$REPO/test/ave-overlay.ko" "$LIB/$REL/ave-overlay.ko"
    install -Dm755 "$HERE/apple-ave-load" "$LIB/apple-ave-load"
    install -Dm644 "$HERE/apple-ave.service" /etc/systemd/system/apple-ave.service
    fw=$REPO/data/blobs/macos-13.5-j313/ave_h13g.bin
    [ -f /lib/firmware/apple/ave_h13g.bin ] || install -Dm644 "$fw" /lib/firmware/apple/ave_h13g.bin
    cat > "$RULE" <<R
// Let $U start (not stop, not edit) the AVE encoder unit without a password.
// What it runs is root-owned: /usr/local/lib/apple-ave/.
polkit.addRule(function(action, subject) {
    if (action.id == "org.freedesktop.systemd1.manage-units" &&
        action.lookup("unit") == "apple-ave.service" &&
        action.lookup("verb") == "start" &&
        subject.user == "$U") {
        return polkit.Result.YES;
    }
});
R
    chmod 644 "$RULE"
    systemctl daemon-reload
    echo "installed for $REL. Start: systemctl start apple-ave.service (no sudo needed for $U)"
    ;;
remove)
    systemctl disable apple-ave.service 2>/dev/null || true
    rm -f /etc/systemd/system/apple-ave.service "$RULE"
    rm -rf "$LIB" /var/lib/apple-ave
    systemctl daemon-reload
    echo "removed (loaded modules stay until the next boot)"
    ;;
*) echo "usage: $0 install|remove"; exit 2 ;;
esac
