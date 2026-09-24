#!/bin/sh
#
# xrdp: a Wayland session's desktop, run by the compositor startwayland.sh
# starts, with WAYLAND_DISPLAY set to its socket.
#
# Reports the socket name to sesexec, sizes the output to the client, and
# runs the desktop: ~/.config/xrdp/waylandsession if it is executable, else
# xfce4-session, else a terminal. When it exits, the session ends.

OUTPUT=${XRDP_WAYLAND_OUTPUT:-HEADLESS-1}

# Start on one output at the client's size; the backend applies the
# client's monitor layout when it connects. The other outputs wait, off.
set -- --output "$OUTPUT"
W=${XRDP_WAYLAND_SIZE%x*}
H=${XRDP_WAYLAND_SIZE#*x}
# MS-RDPEDISP's range: labwc aborts on a tiny output
case "$W$H" in
    ''|*[!0-9]*) ;;
    *) if [ "$W" -ge 200 ] && [ "$H" -ge 200 ] && [ "$W" -le 8192 ] &&
              [ "$H" -le 8192 ]; then
           set -- "$@" --custom-mode "${XRDP_WAYLAND_SIZE}@60Hz"
       fi ;;
esac
for o in $(wlr-randr 2>/dev/null | awk '/^[^ ]/ {print $1}'); do
    [ "$o" != "$OUTPUT" ] && set -- "$@" --output "$o" --off
done
wlr-randr "$@"

# sesexec waits for this: the compositor's socket, as an absolute path so
# the session's other processes need no XDG_RUNTIME_DIR to reach it. Its
# name is the display the session is known by.
case "$WAYLAND_DISPLAY" in
    /*) SOCKET=$WAYLAND_DISPLAY ;;
    *)  SOCKET=$XDG_RUNTIME_DIR/$WAYLAND_DISPLAY ;;
esac
# RemoteApp (sway): a second line, sway's IPC socket, for chansrv.
if [ "$XRDP_WAYLAND_REMOTEAPP" = 1 ] && [ -n "$SWAYSOCK" ]; then
    IPC_LINE=$SWAYSOCK
else
    IPC_LINE=
fi
if [ -n "$XRDP_WAYLAND_NAME_FILE" ]; then
    printf '%s\n%s' "$SOCKET" "${IPC_LINE:+$IPC_LINE
}" > "$XRDP_WAYLAND_NAME_FILE.tmp" &&
        mv "$XRDP_WAYLAND_NAME_FILE.tmp" "$XRDP_WAYLAND_NAME_FILE"
fi

# sesexec sets these from the display name, which it only learns from us:
# chansrv's audio sockets, for the PulseAudio/PipeWire xrdp modules
DISPLAY_NAME=${SOCKET##*/}
export XRDP_PULSE_SINK_SOCKET=xrdp_chansrv_audio_out_socket_$DISPLAY_NAME
export XRDP_PULSE_SOURCE_SOCKET=xrdp_chansrv_audio_in_socket_$DISPLAY_NAME
unset XRDP_WAYLAND_NAME_FILE XRDP_WAYLAND_SIZE

# On the user's D-Bus (startwayland.sh): services it starts on demand
# (notifications, portals, the keyring's prompts) must find this session's
# displays. Only these variables: the rest (GDK_BACKEND, QT_QPA_PLATFORM)
# would outlive the session in the user's activation environment and
# follow the user into a later X11 session.
activation_env() {
    if [ "$XRDP_WAYLAND_USER_BUS" = 1 ] &&
            command -v dbus-update-activation-environment >/dev/null 2>&1; then
        WAYLAND_DISPLAY=$SOCKET dbus-update-activation-environment \
            --systemd WAYLAND_DISPLAY ${DISPLAY:+DISPLAY} \
            ${XDG_CURRENT_DESKTOP:+XDG_CURRENT_DESKTOP} >/dev/null 2>&1
    fi
}

# RemoteApp: no desktop. chansrv starts the client's applications through
# sway (which outlives this script). Only the desktop-independent xrdp
# pieces a desktop's autostart would run: the audio modules.
if [ "$XRDP_WAYLAND_REMOTEAPP" = 1 ]; then
    [ -n "$XRDP_SWAY_CONF" ] && rm -f "$XRDP_SWAY_CONF"
    for f in /etc/xdg/autostart/*xrdp*.desktop; do
        [ -f "$f" ] || continue
        cmd=$(sed -n 's/^Exec=//p' "$f" | head -1)
        [ -n "$cmd" ] && (setsid -f sh -c "$cmd" >/dev/null 2>&1)
    done
    exit 0
fi

USER_SESSION=${XDG_CONFIG_HOME:-$HOME/.config}/xrdp/waylandsession
if [ -x "$USER_SESSION" ]; then
    activation_env
    exec "$USER_SESSION"
fi

if command -v xfce4-session >/dev/null 2>&1; then
    export XDG_CURRENT_DESKTOP=XFCE
    activation_env

    # xfdesktop 4.20 sizes the desktop once, at startup, and ignores later
    # output changes on Wayland (the panel copes). Restart it when the
    # layout changes: an output's size, place, or on/off.
    (
        layout() {
            wlr-randr 2>/dev/null | awk '
                /^[^ ]/ { name = $1 }
                /^  Enabled:/ { print name, $2 }
                /\(current\)/ { print name, $1 }
                /^  Position:/ { print name, $2 }'
        }
        last=$(layout)
        while sleep 1; do
            now=$(layout)
            [ -z "$now" ] && exit 0      # compositor gone
            if [ "$now" != "$last" ]; then
                last=$now
                sleep 0.5
                xfdesktop --quit >/dev/null 2>&1
                sleep 0.5
                setsid -f xfdesktop >/dev/null 2>&1
            fi
        done
    ) &

    exec xfce4-session
fi

for term in foot xfce4-terminal alacritty kitty; do
    if command -v "$term" >/dev/null 2>&1; then
        exec "$term"
    fi
done
echo "waylandsession.sh: no desktop session found" >&2
exit 1
