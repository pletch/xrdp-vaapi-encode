#!/bin/sh
#
# xrdp: start a Wayland session's compositor (SCP_SESSION_TYPE_WAYLAND).
#
# Run by sesexec as the user. Starts a headless wlroots compositor (labwc)
# whose session command, waylandsession.sh, reports the compositor's socket
# name back to sesexec and runs the desktop. The session lasts as long as
# the compositor, and the compositor as long as the desktop.
#
# From sesexec:
#   XRDP_WAYLAND_NAME_FILE  where waylandsession.sh writes WAYLAND_DISPLAY
#   XRDP_WAYLAND_SIZE       the client's size, WxH
#   XRDP_WAYLAND_REMOTEAPP  1 for a RemoteApp session: sway, no desktop
# Optional:
#   XRDP_WAYLAND_RENDER_NODE  DRM render node (default: XRDP_VAAPI_DEVICE,
#                             then /dev/dri/renderD128)
#   XRDP_WAYLAND_MONITORS     most client monitors the session can show
#                             (default 4): the compositor's outputs, all
#                             but the first off until a client uses them
#   XRDP_WAYLAND_PRIVATE_BUS  1: always give the desktop its own D-Bus
#                             session bus (see below)
# Read by the backend (wlxrdp), from [SessionVariables] like these:
#   XRDP_WAYLAND_SCALE        each monitor's scale: unset or "auto", the
#                             client's (mstsc sends its display's, e.g.
#                             200%), else 200% over 2000 pixels wide;
#                             "client", the client's, else 100%; or a
#                             number for every monitor (150 or 1.5)
#   XRDP_WAYLAND_MAX_PIXELS   all monitors together, in pixels (default
#                             40000000); a larger layout is refused

XRDP_CFG_DIR=$(dirname "$(readlink -f "$0")")

export XDG_RUNTIME_DIR=${XDG_RUNTIME_DIR:-/run/user/$(id -u)}
export XDG_SESSION_TYPE=wayland

# wlroots: no real outputs or input devices; a virtual output per monitor
# a client may bring (wlxrdp enables and places them)
export WLR_BACKENDS=headless
export WLR_HEADLESS_OUTPUTS=${XRDP_WAYLAND_MONITORS:-4}
export WLR_LIBINPUT_NO_DEVICES=1
export WLR_RENDERER=${WLR_RENDERER:-gles2}
export WLR_RENDER_DRM_DEVICE=${XRDP_WAYLAND_RENDER_NODE:-${XRDP_VAAPI_DEVICE:-/dev/dri/renderD128}}
export LABWC_FALLBACK_OUTPUT=NOOP-fallback

# Toolkits on their Wayland backends
export GDK_BACKEND=wayland,x11
export QT_QPA_PLATFORM=wayland
export MOZ_ENABLE_WAYLAND=1

# Never take the default socket name. With WAYLAND_DISPLAY unset,
# libwayland connects to wayland-0, and GTK tries Wayland first, so apps in
# the user's X11 sessions would land on this compositor. Holding
# wayland-0's lock makes the compositor skip it. If someone else holds it
# (another of these sessions, or a real Wayland login), it is skipped
# anyway.
exec 9>"$XDG_RUNTIME_DIR/wayland-0.lock"
flock -n 9 || true

unset DISPLAY WAYLAND_DISPLAY

# XFCE's labwc configuration, where XFCE is installed
LABWC_ARGS=
if [ -f /usr/share/xfce4/labwc/labwc-rc.xml ] && \
        command -v xfce4-session >/dev/null 2>&1; then
    LABWC_ARGS="--config /usr/share/xfce4/labwc/labwc-rc.xml"
fi

# RemoteApp: sway, whose IPC lets chansrv follow and place the windows
# (labwc offers no way to). The session lasts until sesexec ends it.
if [ "$XRDP_WAYLAND_REMOTEAPP" = 1 ]; then
    if ! command -v sway >/dev/null 2>&1; then
        echo "startwayland.sh: RemoteApp sessions need sway" >&2
        exit 1
    fi
    CONF=$XDG_RUNTIME_DIR/xrdp-sway-remoteapp.$$.conf
    {
        echo "include $XRDP_CFG_DIR/sway-remoteapp.conf"
        echo "exec $XRDP_CFG_DIR/waylandsession.sh"
    } > "$CONF" || exit 1
    export XRDP_SWAY_CONF=$CONF
    exec sway -c "$CONF"
fi

# The desktop's D-Bus session bus: the user's, as for X11 sessions, where
# the keyring (org.freedesktop.secrets) and the user's services are. A
# private bus has no keyring: apps asking for secrets (VS Code, browsers)
# block until the activation times out, and a Wayland client that blocks
# is disconnected by the compositor. But a second desktop session on the
# user's bus would clash with the first (session manager and settings
# daemons claim fixed bus names), so with one already there, or with
# XRDP_WAYLAND_PRIVATE_BUS=1, the desktop gets its own bus, as before.
user_bus_busy() {
    for name in org.xfce.SessionManager org.gnome.SessionManager \
            org.kde.ksmserver; do
        dbus-send --session --print-reply --dest=org.freedesktop.DBus \
            /org/freedesktop/DBus org.freedesktop.DBus.NameHasOwner \
            "string:$name" 2>/dev/null | grep -q 'boolean true' && return 0
    done
    return 1
}
SESSION_CMD="dbus-run-session -- $XRDP_CFG_DIR/waylandsession.sh"
if [ "$XRDP_WAYLAND_PRIVATE_BUS" != 1 ] && \
        [ -S "$XDG_RUNTIME_DIR/bus" ] && command -v dbus-send >/dev/null 2>&1; then
    export DBUS_SESSION_BUS_ADDRESS=unix:path=$XDG_RUNTIME_DIR/bus
    if user_bus_busy; then
        echo "startwayland.sh: a desktop session is already on the user's" \
            "D-Bus; this one gets its own bus (no keyring)" >&2
    else
        export XRDP_WAYLAND_USER_BUS=1
        SESSION_CMD=$XRDP_CFG_DIR/waylandsession.sh
    fi
fi

# The compositor is this process (sesexec's session leader): a SIGTERM
# here ends it, and with it the session's clients.
exec labwc $LABWC_ARGS -S "$SESSION_CMD"
