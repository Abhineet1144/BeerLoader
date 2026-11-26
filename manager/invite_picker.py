#!/usr/bin/env python3
"""Friend picker for in-game invites (ISteamFriends::ActivateGameOverlayInviteDialog).

Steam's overlay is not available under Beer, so the loader opens this window when a game asks for the invite
dialog. It asks the Beer Steam provider for the signed-in account's friends and sends each chosen friend a lobby
invite through the provider (the friend's Steam shows it with a Join button).

    invite_picker.py --appid 480 --lobby 109775...
"""
import argparse
import itertools
import json
import os
import socket
import sys
import threading

import gi

gi.require_version("Gtk", "4.0")
gi.require_version("Adw", "1")
from gi.repository import Adw, GLib, Gtk  # noqa: E402

STATES = {0: "Offline", 1: "Online", 2: "Busy", 3: "Away", 4: "Snooze", 5: "Looking to trade",
          6: "Looking to play", 7: "Invisible"}


def socket_path():
    runtime = os.environ.get("XDG_RUNTIME_DIR")
    return os.path.join(runtime, "beer", "steam.sock") if runtime else "/tmp/beer-%d/steam.sock" % os.getuid()


class Provider:
    """One request at a time on the provider socket; replies are broadcast, so they are matched by id."""

    def __init__(self):
        self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.sock.settimeout(30)
        self.sock.connect(socket_path())
        self.buffer = b""
        self.ids = itertools.count(1)
        self.lock = threading.Lock()

    def request(self, op, **fields):
        with self.lock:
            rid = "invite%d-%d" % (os.getpid(), next(self.ids))
            self.sock.sendall((json.dumps({"op": op, "id": rid, **fields}) + "\n").encode())
            while True:
                while b"\n" in self.buffer:
                    line, self.buffer = self.buffer.split(b"\n", 1)
                    try:
                        reply = json.loads(line)
                    except ValueError:
                        continue
                    if reply.get("id") == rid:
                        return reply
                chunk = self.sock.recv(65536)
                if not chunk:
                    raise ConnectionError("the Beer Steam provider closed the connection")
                self.buffer += chunk


def describe(friend):
    state = friend.get("state", 0)
    if friend.get("appid") and state:
        return "In game (app %d)" % friend["appid"]
    return STATES.get(state, "Online")


class Picker(Adw.Application):
    def __init__(self, appid, lobby):
        super().__init__(application_id=None)
        self.appid, self.lobby = appid, lobby
        self.provider = None
        self.rows = []

    def do_activate(self):
        self.window = Adw.ApplicationWindow(application=self, title="Invite friends", default_width=420,
                                            default_height=560)
        self.toasts = Adw.ToastOverlay()
        box = Gtk.Box(orientation=Gtk.Orientation.VERTICAL)
        header = Adw.HeaderBar()
        self.invite_button = Gtk.Button(label="Invite", sensitive=False)
        self.invite_button.add_css_class("suggested-action")
        self.invite_button.connect("clicked", self.on_invite)
        header.pack_end(self.invite_button)
        box.append(header)
        self.search = Gtk.SearchEntry(placeholder_text="Search friends", margin_start=12, margin_end=12,
                                      margin_top=6, margin_bottom=6)
        self.search.connect("search-changed", lambda *_: self.listbox.invalidate_filter())
        box.append(self.search)
        self.status = Gtk.Label(label="Loading friends…", margin_top=24, css_classes=["dim-label"])
        box.append(self.status)
        self.listbox = Gtk.ListBox(selection_mode=Gtk.SelectionMode.NONE, css_classes=["boxed-list"],
                                   margin_start=12, margin_end=12, margin_bottom=12)
        self.listbox.set_filter_func(self.filter_row)
        scroller = Gtk.ScrolledWindow(vexpand=True, child=self.listbox)
        box.append(scroller)
        self.toasts.set_child(box)
        self.window.set_content(self.toasts)
        self.window.present()
        threading.Thread(target=self.load, daemon=True).start()

    def filter_row(self, row):
        text = self.search.get_text().strip().lower()
        return not text or text in row.friend_name.lower()

    def load(self, attempt=0):
        try:
            if not self.provider:
                self.provider = Provider()
            reply = self.provider.request("friends")
        except OSError as error:
            GLib.idle_add(self.show_error, "Cannot reach the Beer Steam provider: %s" % error)
            return
        if not reply.get("ok"):
            GLib.idle_add(self.show_error, reply.get("error") or "The Steam provider refused")
            return
        friends = reply.get("friends", [])
        # Names arrive shortly after sign-in; ask again a few times while some are still missing.
        if attempt < 5 and any(not f.get("name") for f in friends):
            GLib.idle_add(self.show_friends, friends)
            threading.Timer(1.0, self.load, args=(attempt + 1,)).start()
            return
        GLib.idle_add(self.show_friends, friends)

    def show_error(self, message):
        self.status.set_label(message)
        self.status.set_visible(True)

    def show_friends(self, friends):
        chosen = {row.steamid for row in self.rows if row.check.get_active()}
        self.listbox.remove_all()
        self.rows = []
        friends.sort(key=lambda f: (f.get("state", 0) == 0, (f.get("name") or "~").lower()))
        for friend in friends:
            name = friend.get("name") or friend["steamid"]
            row = Adw.ActionRow(title=GLib.markup_escape_text(name), subtitle=describe(friend))
            row.steamid, row.friend_name = friend["steamid"], name
            row.check = Gtk.CheckButton(active=friend["steamid"] in chosen, valign=Gtk.Align.CENTER)
            row.check.connect("toggled", self.update_button)
            row.add_prefix(row.check)
            row.set_activatable_widget(row.check)
            self.listbox.append(row)
            self.rows.append(row)
        if friends:
            self.status.set_visible(False)
        else:
            self.show_error("No friends found for this Steam account.")
        self.update_button()

    def update_button(self, *_):
        count = sum(row.check.get_active() for row in self.rows)
        self.invite_button.set_sensitive(count > 0)
        self.invite_button.set_label("Invite (%d)" % count if count else "Invite")

    def on_invite(self, _button):
        chosen = [(row.steamid, row.friend_name) for row in self.rows if row.check.get_active()]
        self.invite_button.set_sensitive(False)
        threading.Thread(target=self.send_invites, args=(chosen,), daemon=True).start()

    def send_invites(self, chosen):
        failed = []
        for steamid, name in chosen:
            try:
                reply = self.provider.request("invite_lobby", appid=str(self.appid), lobby=str(self.lobby),
                                              friend=steamid)
                if not reply.get("ok"):
                    failed.append("%s: %s" % (name, reply.get("error", "refused")))
            except OSError as error:
                failed.append("%s: %s" % (name, error))
        GLib.idle_add(self.invites_done, len(chosen), failed)

    def invites_done(self, count, failed):
        if failed:
            self.toasts.add_toast(Adw.Toast(title="Invite failed — " + "; ".join(failed), timeout=8))
            self.update_button()
            return
        self.toasts.add_toast(Adw.Toast(title="Invite sent" if count == 1 else "%d invites sent" % count))
        GLib.timeout_add(1200, self.quit)


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--appid", type=int, required=True)
    parser.add_argument("--lobby", type=int, required=True)
    args = parser.parse_args()
    return Picker(args.appid, args.lobby).run([sys.argv[0]])


if __name__ == "__main__":
    sys.exit(main())
