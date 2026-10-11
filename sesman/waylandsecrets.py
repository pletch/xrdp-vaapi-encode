#!/usr/bin/python3
#
# xrdp: the user's keyring on a Wayland session's private D-Bus.
#
# A Wayland desktop started while another desktop holds the user's session
# bus gets a bus of its own (startwayland.sh). The keyring daemon is on the
# user's bus, and the private bus cannot start another: asking it for
# org.freedesktop.secrets blocks until activation times out (two minutes),
# and browsers wait for it before they load a page.
#
# This takes org.freedesktop.secrets on the private bus and passes each
# method call to the keyring on the user's bus, and its replies and signals
# back. The keyring sees one client, this one: Secret Service sessions and
# prompts belong to it, which is fine while it lives as long as the desktop.
#
# Run by waylandsession.sh on the private bus, with the user's bus address
# as its argument. Exits when either bus goes away.

import sys

import gi
gi.require_version("Gio", "2.0")
from gi.repository import Gio, GLib

NAME = "org.freedesktop.secrets"
TIMEOUT_MS = 120000  # the keyring's prompts wait for the user


def log(msg):
    print("waylandsecrets: " + msg, file=sys.stderr)


def main():
    if len(sys.argv) != 2:
        log("usage: waylandsecrets.py USER_BUS_ADDRESS")
        return 2

    flags = (Gio.DBusConnectionFlags.AUTHENTICATION_CLIENT |
             Gio.DBusConnectionFlags.MESSAGE_BUS_CONNECTION)
    try:
        user = Gio.DBusConnection.new_for_address_sync(
            sys.argv[1], flags, None, None)
        private = Gio.bus_get_sync(Gio.BusType.SESSION, None)
    except GLib.Error as e:
        log(e.message)
        return 1

    # Nothing to pass on to: leave the name to the private bus
    owner = user.call_sync(
        "org.freedesktop.DBus", "/org/freedesktop/DBus",
        "org.freedesktop.DBus", "NameHasOwner", GLib.Variant("(s)", (NAME,)),
        GLib.VariantType("(b)"), Gio.DBusCallFlags.NONE, -1, None)
    if not owner.unpack()[0]:
        log("no keyring on the user's bus")
        return 1

    loop = GLib.MainLoop()

    def relay_reply(conn, res, call):
        try:
            reply = conn.send_message_with_reply_finish(res)
        except GLib.Error as e:
            out = Gio.DBusMessage.new_method_error_literal(
                call, "org.freedesktop.DBus.Error.Failed", e.message)
        else:
            if reply.get_message_type() == Gio.DBusMessageType.ERROR:
                out = Gio.DBusMessage.new_method_error_literal(
                    call, reply.get_error_name(),
                    reply.get_arg0() or reply.get_error_name())
            else:
                out = call.new_method_reply()
                if reply.get_body() is not None:
                    out.set_body(reply.get_body())
                out.set_unix_fd_list(reply.get_unix_fd_list())
        try:
            private.send_message(out, Gio.DBusSendMessageFlags.NONE)
        except GLib.Error as e:
            log("reply: " + e.message)

    def relay_call(call):
        out = Gio.DBusMessage.new_method_call(
            NAME, call.get_path(), call.get_interface(), call.get_member())
        if call.get_body() is not None:
            out.set_body(call.get_body())
        # The Secret portal's RetrieveSecret passes a pipe for the secret
        out.set_unix_fd_list(call.get_unix_fd_list())
        out.set_flags(call.get_flags())
        if call.get_flags() & Gio.DBusMessageFlags.NO_REPLY_EXPECTED:
            user.send_message(out, Gio.DBusSendMessageFlags.NONE)
        else:
            user.send_message_with_reply(
                out, Gio.DBusSendMessageFlags.NONE, TIMEOUT_MS, None,
                relay_reply, call)
        return GLib.SOURCE_REMOVE

    # Every method call to us goes to the keyring. Filters run on GDBus's
    # worker thread: hand the call to the main loop.
    def on_message(conn, msg, incoming):
        if incoming and \
                msg.get_message_type() == Gio.DBusMessageType.METHOD_CALL and \
                msg.get_path() is not None:
            GLib.idle_add(relay_call, msg)
            return None
        return msg

    private.add_filter(on_message)

    def relay_signal(conn, sender, path, iface, member, params):
        try:
            private.emit_signal(None, path, iface, member, params)
        except GLib.Error as e:
            log("signal: " + e.message)

    user.signal_subscribe(NAME, None, None, None, None,
                          Gio.DBusSignalFlags.NONE, relay_signal)

    def lost(*args):
        if loop.is_running():
            log("lost a bus or the name; exiting")
            loop.quit()

    user.connect("closed", lambda *a: lost())
    private.connect("closed", lambda *a: lost())
    private.set_exit_on_close(False)
    user.set_exit_on_close(False)
    # The keyring going away (or restarting) ends this too: a new one would
    # need its signals subscribed afresh
    user.signal_subscribe(
        "org.freedesktop.DBus", "org.freedesktop.DBus", "NameOwnerChanged",
        "/org/freedesktop/DBus", NAME, Gio.DBusSignalFlags.NONE,
        lambda *a: lost())

    Gio.bus_own_name_on_connection(
        private, NAME, Gio.BusNameOwnerFlags.DO_NOT_QUEUE,
        lambda *a: log("serving the user's keyring"), lost)

    loop.run()
    return 0


if __name__ == "__main__":
    sys.exit(main())
