"""Steam page: sign in to a Steam account through the beer-steam session provider.

The provider (steam/provider.c) owns the Steam connection and keeps the session in the system keyring.
This page talks to it over $XDG_RUNTIME_DIR/beer/steam.sock (one JSON object per line) and starts it
when it is not running. Passwords and Steam Guard codes are passed straight to the provider and never
stored or logged here.
"""
import json
import os
import socket
import subprocess
import threading
import time

import gi

gi.require_version("Gtk", "4.0")
gi.require_version("Adw", "1")
from gi.repository import Adw, GLib, Gtk  # noqa: E402

import storage  # noqa: E402
from qr import QrCode  # noqa: E402

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_PROVIDER = os.path.join(REPO, "beer-steam")


def socket_path():
    runtime = os.environ.get("XDG_RUNTIME_DIR")
    folder = os.path.join(runtime, "beer") if runtime else "/tmp/beer-%d" % os.getuid()
    return os.path.join(folder, "steam.sock")


class ProviderClient:
    """One connection to the provider. Events arrive on the GTK main loop through on_event."""

    def __init__(self, on_event):
        self.on_event = on_event
        self.sock = None
        self.lock = threading.Lock()
        self.next_id = 1

    def connect(self):
        sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        try:
            sock.connect(socket_path())
        except OSError:
            sock.close()
            return False
        self.sock = sock
        threading.Thread(target=self._read, args=(sock,), daemon=True).start()
        return True

    def connected(self):
        return self.sock is not None

    def send(self, op, **fields):
        with self.lock:
            request_id = str(self.next_id)
            self.next_id += 1
        message = dict(fields, op=op, id=request_id)
        try:
            self.sock.sendall((json.dumps(message) + "\n").encode())
        except (OSError, AttributeError):
            self.close()
            GLib.idle_add(self.on_event, {"event": "disconnected"})
        return request_id

    def close(self):
        sock, self.sock = self.sock, None
        if sock:
            try:
                sock.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass
            sock.close()

    def _read(self, sock):
        buffer = b""
        while True:
            try:
                chunk = sock.recv(65536)
            except OSError:
                chunk = b""
            if not chunk:
                break
            buffer += chunk
            while b"\n" in buffer:
                line, buffer = buffer.split(b"\n", 1)
                try:
                    event = json.loads(line)
                except ValueError:
                    continue
                GLib.idle_add(self.on_event, event)
        if self.sock is sock:
            self.sock = None
            GLib.idle_add(self.on_event, {"event": "disconnected"})


JOB_TEXT = {"queued": "Waiting to download", "preparing": "Preparing…", "committing": "Finishing…",
            "paused": "Paused"}


def fmt_bytes(value):
    value = float(value or 0)
    for unit in ("B", "KB", "MB", "GB"):
        if value < 1000 or unit == "GB":
            return ("%.0f %s" if unit in ("B", "KB") else "%.1f %s") % (value, unit)
        value /= 1000


def fmt_duration(seconds):
    seconds = int(seconds)
    if seconds < 60:
        return "%ds" % seconds
    if seconds < 3600:
        return "%dm" % (seconds // 60)
    return "%dh %02dm" % (seconds // 3600, seconds % 3600 // 60)


# Library entries shown by default; tools, DLC, configs and the rest sit behind the "show all" toggle.
PRIMARY_TYPES = {"game", "demo", "application"}
TYPE_NAMES = {"demo": "Demo", "application": "Application", "tool": "Tool", "dlc": "DLC", "config": "Configuration",
              "beta": "Beta", "music": "Soundtrack", "video": "Video", "series": "Series", "hardware": "Hardware"}

GUARD_TEXT = {
    "device_code": "Enter the code from the Steam Guard authenticator in your Steam mobile app.",
    "email_code": "Enter the code Steam sent to your email{hint}.",
    "device_confirmation": "Approve the sign-in in your Steam mobile app.",
    "email_confirmation": "Approve the sign-in from the email Steam sent you{hint}.",
}


class SteamPage(Gtk.Box):
    def __init__(self, app):
        super().__init__(orientation=Gtk.Orientation.VERTICAL)
        self.app = app
        self.client = ProviderClient(self._on_event)
        self.state = {}
        self.login_kind = None        # "qr" or "password" while a sign-in is running
        self.started_provider = False

        self.views = Gtk.Stack(transition_type=Gtk.StackTransitionType.CROSSFADE, vexpand=True)
        self.append(self.views)
        self._build_unavailable()
        self._build_connecting()
        self._build_signed_out()
        self._build_guard()
        self._build_signed_in()
        self.views.set_visible_child_name("connecting")
        GLib.idle_add(self._connect)

    # ---------------------------------------------------------------- layout helpers
    @staticmethod
    def _scrolled(child, width=560):
        clamp = Adw.Clamp(maximum_size=width, margin_top=24, margin_bottom=24, margin_start=12, margin_end=12)
        clamp.set_child(child)
        scroll = Gtk.ScrolledWindow(vexpand=True, hscrollbar_policy=Gtk.PolicyType.NEVER)
        scroll.set_child(clamp)
        return scroll

    @staticmethod
    def _pill(label, style=None):
        button = Gtk.Button(label=label, halign=Gtk.Align.CENTER)
        button.add_css_class("pill")
        if style:
            button.add_css_class(style)
        return button

    def _build_unavailable(self):
        self.unavailable = Adw.StatusPage(icon_name="network-offline-symbolic", title="Steam provider not running")
        start = self._pill("Start provider", "suggested-action")
        start.connect("clicked", lambda _b: self._connect(force_start=True))
        self.unavailable.set_child(start)
        self.views.add_named(self.unavailable, "unavailable")

    def _build_connecting(self):
        self.connecting = Adw.StatusPage(title="Connecting to Steam…")
        self.connecting.set_child(Adw.Spinner(width_request=32, height_request=32))
        self.views.add_named(self.connecting, "connecting")

    def _build_signed_out(self):
        body = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=18)
        heading = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=6)
        heading.append(Gtk.Image(icon_name="avatar-default-symbolic", pixel_size=64, margin_bottom=6))
        title = Gtk.Label(label="Sign in to Steam")
        title.add_css_class("title-2")
        heading.append(title)
        subtitle = Gtk.Label(label="Download and play the games you own, with your real Steam account.", wrap=True,
                             justify=Gtk.Justification.CENTER)
        subtitle.add_css_class("dim-label")
        heading.append(subtitle)
        body.append(heading)

        self.method = Adw.ToggleGroup(halign=Gtk.Align.CENTER)
        self.method.add(Adw.Toggle(name="qr", label="QR code"))
        self.method.add(Adw.Toggle(name="password", label="Password"))
        self.method.connect("notify::active-name", lambda *_a: self._show_method())
        body.append(self.method)

        self.methods = Gtk.Stack(transition_type=Gtk.StackTransitionType.CROSSFADE, vhomogeneous=False)
        body.append(self.methods)

        # QR code sign-in
        qr_box = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=12)
        frame = Gtk.Box(halign=Gtk.Align.CENTER, width_request=272, height_request=272)
        frame.add_css_class("card")
        self.qr_stack = Gtk.Stack(hexpand=True, vexpand=True)
        self.qr_code = QrCode(size=248)
        start_qr = self._pill("Show QR code", "suggested-action")
        start_qr.set_valign(Gtk.Align.CENTER)
        start_qr.connect("clicked", lambda _b: self._begin_qr())
        self.qr_stack.add_named(start_qr, "idle")
        self.qr_stack.add_named(Adw.Spinner(width_request=32, height_request=32), "waiting")
        self.qr_stack.add_named(self.qr_code, "code")
        frame.append(self.qr_stack)
        qr_box.append(frame)
        qr_help = Gtk.Label(wrap=True, justify=Gtk.Justification.CENTER,
                            label="In the Steam mobile app, open the Steam Guard tab and tap the QR code button, "
                                  "then point the camera at this code. The code refreshes on its own.")
        qr_help.add_css_class("dim-label")
        qr_box.append(qr_help)
        self.qr_cancel = self._pill("Cancel")
        self.qr_cancel.connect("clicked", lambda _b: self._cancel_login())
        self.qr_cancel.set_visible(False)
        qr_box.append(self.qr_cancel)
        self.methods.add_named(qr_box, "qr")

        # Account name + password sign-in
        password_box = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=18)
        fields = Adw.PreferencesGroup()
        self.account_row = Adw.EntryRow(title="Account name")
        self.password_row = Adw.PasswordEntryRow(title="Password")
        self.account_row.connect("entry-activated", lambda _r: self.password_row.grab_focus())
        self.password_row.connect("entry-activated", lambda _r: self._begin_password())
        fields.add(self.account_row)
        fields.add(self.password_row)
        password_box.append(fields)
        self.sign_in = self._pill("Sign in", "suggested-action")
        self.sign_in.connect("clicked", lambda _b: self._begin_password())
        password_box.append(self.sign_in)
        note = Gtk.Label(wrap=True, justify=Gtk.Justification.CENTER,
                         label="Your password is sent only to Steam. Beer keeps the signed-in session in your "
                               "system keyring and never stores the password.")
        note.add_css_class("dim-label")
        note.add_css_class("caption")
        password_box.append(note)
        self.methods.add_named(password_box, "password")

        self.signin_error = Gtk.Label(wrap=True, justify=Gtk.Justification.CENTER, visible=False)
        self.signin_error.add_css_class("error")
        body.append(self.signin_error)
        self.views.add_named(self._scrolled(body), "signed_out")
        self.method.set_active_name("qr")
        self._show_method()

    def _build_guard(self):
        body = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=18)
        self.guard_status = Adw.StatusPage(icon_name="security-high-symbolic", title="Steam Guard")
        self.guard_status.add_css_class("compact")
        body.append(self.guard_status)
        self.guard_fields = Adw.PreferencesGroup()
        self.code_row = Adw.EntryRow(title="Code")
        self.code_row.connect("entry-activated", lambda _r: self._submit_code())
        submit = Gtk.Button(icon_name="go-next-symbolic", valign=Gtk.Align.CENTER, tooltip_text="Submit")
        submit.add_css_class("flat")
        submit.connect("clicked", lambda _b: self._submit_code())
        self.code_row.add_suffix(submit)
        self.guard_fields.add(self.code_row)
        body.append(self.guard_fields)
        self.guard_waiting = Gtk.Box(halign=Gtk.Align.CENTER, spacing=8)
        self.guard_waiting.append(Adw.Spinner())
        self.guard_waiting.append(Gtk.Label(label="Waiting for approval…"))
        body.append(self.guard_waiting)
        self.guard_error = Gtk.Label(wrap=True, justify=Gtk.Justification.CENTER, visible=False)
        self.guard_error.add_css_class("error")
        body.append(self.guard_error)
        cancel = self._pill("Cancel")
        cancel.connect("clicked", lambda _b: self._cancel_login())
        body.append(cancel)
        self.views.add_named(self._scrolled(body), "guard")

    def _build_signed_in(self):
        body = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=18)

        # Account: compact header, details on demand.
        account = Adw.PreferencesGroup()
        self.profile_row = Adw.ExpanderRow()
        self.avatar = Adw.Avatar(size=40, show_initials=True)
        self.profile_row.add_prefix(self.avatar)
        sign_out = Gtk.Button(label="Sign out", valign=Gtk.Align.CENTER)
        sign_out.add_css_class("flat")
        sign_out.connect("clicked", lambda _b: self._confirm_sign_out())
        self.profile_row.add_suffix(sign_out)
        self.steamid_row = Adw.ActionRow(title="SteamID", subtitle_selectable=True)
        self.steamid_row.add_css_class("property")
        self.storage_row = Adw.ActionRow(title="Session storage")
        self.storage_row.add_css_class("property")
        self.profile_row.add_row(self.steamid_row)
        self.profile_row.add_row(self.storage_row)
        account.add(self.profile_row)
        body.append(account)

        # Library: owned apps from the provider, games first.
        header = Gtk.Box(spacing=12)
        title_box = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, hexpand=True, valign=Gtk.Align.CENTER)
        title = Gtk.Label(label="Library", xalign=0)
        title.add_css_class("heading")
        self.library_count = Gtk.Label(xalign=0)
        self.library_count.add_css_class("dim-label")
        self.library_count.add_css_class("caption")
        title_box.append(title)
        title_box.append(self.library_count)
        header.append(title_box)
        self.library_spinner = Adw.Spinner(visible=False, tooltip_text="Refreshing from Steam")
        header.append(self.library_spinner)
        self.show_all = Gtk.ToggleButton(label="Show all", valign=Gtk.Align.CENTER,
                                         tooltip_text="Also show tools, DLC and other software")
        self.show_all.add_css_class("flat")
        self.show_all.connect("toggled", lambda _b: self._refilter())
        header.append(self.show_all)
        body.append(header)

        self.section = Adw.ToggleGroup(halign=Gtk.Align.START)
        for name, label in (("installed", "Installed"), ("downloading", "Downloading"), ("library", "Library")):
            self.section.add(Adw.Toggle(name=name, label=label))
        self.section.set_active_name("installed")
        self.section.connect("notify::active-name", lambda *_a: self._refilter())
        body.append(self.section)

        self.search = Gtk.SearchEntry(placeholder_text="Search your games")
        self.search.connect("search-changed", lambda _e: self._refilter())
        body.append(self.search)

        self.library_stack = Gtk.Stack(vhomogeneous=False)
        loading = Gtk.Box(halign=Gtk.Align.CENTER, spacing=8, margin_top=24)
        loading.append(Adw.Spinner())
        loading.append(Gtk.Label(label="Reading your library from Steam…"))
        self.library_stack.add_named(loading, "loading")
        self.library_message = Adw.StatusPage(icon_name="applications-games-symbolic")
        self.library_message.add_css_class("compact")
        self.library_stack.add_named(self.library_message, "message")
        self.library_list = Gtk.ListBox(selection_mode=Gtk.SelectionMode.NONE)
        self.library_list.add_css_class("boxed-list")
        self.library_list.set_filter_func(self._filter_row)
        self.library_stack.add_named(self.library_list, "list")
        body.append(self.library_stack)
        self.apps = []
        self.rows = {}
        self.jobs = {}
        self.installs = {}
        self.updates = set()
        self.checked_updates = False
        self.waiters = {}
        self.views.add_named(self._scrolled(body, width=720), "signed_in")

    def _show_library(self, event):
        state = event.get("state")
        self.library_spinner.set_visible(state == "refreshing")
        self.library_spinner.set_tooltip_text("Refreshing from Steam" if state == "refreshing" else None)
        if state == "loading" and not event.get("apps"):
            self.library_stack.set_visible_child_name("loading")
            self.library_count.set_label("")
            return
        if state == "error" and not event.get("apps"):
            self.library_message.set_title("Could not read your library")
            self.library_message.set_description(GLib.markup_escape_text(event.get("error", "")))
            self.library_stack.set_visible_child_name("message")
            return
        self.apps = event.get("apps", [])
        self.rows = {}
        child = self.library_list.get_first_child()
        while child:
            following = child.get_next_sibling()
            self.library_list.remove(child)
            child = following
        for app in self.apps:
            self.library_list.append(self._app_row(app))
        self._refilter()

    @staticmethod
    def _primary(app):
        return app.get("type") in PRIMARY_TYPES

    def _app_row(self, app):
        appid = app["appid"]
        name = app.get("name") or "App %d" % appid
        row = Adw.ActionRow(title=GLib.markup_escape_text(name))
        row.app = app
        row.search_key = name.casefold()
        row.details = ["App %d" % appid]
        kind = app.get("type", "")
        if kind and kind != "game":
            row.details.append(TYPE_NAMES.get(kind, kind.capitalize()))
        if not app.get("windows"):
            row.details.append("No Windows version")
        if app.get("shared"):
            row.details.append("Family shared")

        # Right-hand side: one view per install state.
        row.side = Gtk.Stack(valign=Gtk.Align.CENTER, hhomogeneous=False, interpolate_size=True,
                             transition_type=Gtk.StackTransitionType.CROSSFADE)
        install = Gtk.Button(label="Install", valign=Gtk.Align.CENTER)
        install.add_css_class("suggested-action")
        install.set_sensitive(bool(app.get("windows")))
        install.connect("clicked", lambda _b, a=app: self._ask_install(a))
        row.side.add_named(install, "install")

        progress = Gtk.Box(spacing=6, valign=Gtk.Align.CENTER)
        row.bar = Gtk.ProgressBar(valign=Gtk.Align.CENTER, width_request=140)
        progress.append(row.bar)
        row.pause = Gtk.Button(icon_name="media-playback-pause-symbolic", tooltip_text="Pause", valign=Gtk.Align.CENTER)
        row.pause.add_css_class("flat")
        row.pause.connect("clicked", lambda _b, i=appid: self._toggle_pause(i))
        progress.append(row.pause)
        cancel = Gtk.Button(icon_name="process-stop-symbolic", tooltip_text="Cancel", valign=Gtk.Align.CENTER)
        cancel.add_css_class("flat")
        cancel.connect("clicked", lambda _b, i=appid: self.client.send("cancel", appid=str(i)))
        progress.append(cancel)
        row.side.add_named(progress, "progress")

        installed = Gtk.Box(spacing=6, valign=Gtk.Align.CENTER)
        row.update = Gtk.Button(label="Update", valign=Gtk.Align.CENTER, visible=False)
        row.update.add_css_class("suggested-action")
        row.update.connect("clicked", lambda _b, i=appid: self._job("update", i))
        installed.append(row.update)
        row.installed_label = Gtk.Label(label="Installed")
        row.installed_label.add_css_class("dim-label")
        row.installed_label.add_css_class("caption")
        installed.append(row.installed_label)
        menu = Gtk.MenuButton(icon_name="view-more-symbolic", valign=Gtk.Align.CENTER, tooltip_text="More")
        menu.add_css_class("flat")
        menu.set_popover(self._installed_menu(appid))
        installed.append(menu)
        row.side.add_named(installed, "installed")

        failed = Gtk.Box(spacing=6, valign=Gtk.Align.CENTER)
        retry = Gtk.Button(label="Retry", valign=Gtk.Align.CENTER)
        retry.connect("clicked", lambda _b, a=app: self._retry(a))
        failed.append(retry)
        dismiss = Gtk.Button(icon_name="window-close-symbolic", tooltip_text="Dismiss", valign=Gtk.Align.CENTER)
        dismiss.add_css_class("flat")
        dismiss.connect("clicked", lambda _b, i=appid: (self.jobs.pop(i, None), self._update_row(i)))
        failed.append(dismiss)
        row.side.add_named(failed, "error")
        row.add_suffix(row.side)
        self.rows[appid] = row
        self._update_row(appid)
        return row

    def _installed_menu(self, appid):
        popover = Gtk.Popover()
        box = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, margin_top=6, margin_bottom=6, margin_start=6, margin_end=6)
        for label, action in (("Open folder", "open"), ("Verify files", "verify"), ("Change version…", "version"),
                              ("Move to another folder…", "move"), ("Uninstall…", "uninstall")):
            button = Gtk.Button(label=label)
            button.add_css_class("flat")
            button.get_child().set_xalign(0)
            if action == "uninstall":
                button.add_css_class("error")
            button.connect("clicked", lambda _b, a=action, i=appid: (popover.popdown(), self._installed_action(a, i)))
            box.append(button)
        popover.set_child(box)
        return popover

    # ---------------------------------------------------------------- install state per app
    def _update_row(self, appid):
        row = self.rows.get(appid)
        if not row:
            return
        job = self.jobs.get(appid)
        record = self.installs.get(appid)
        details = list(row.details)
        if job and job["state"] not in ("installed", "cancelled"):
            state = job["state"]
            if state == "error":
                row.side.set_visible_child_name("error")
                details.append("Failed: " + (job.get("error") or "unknown error"))
            else:
                row.side.set_visible_child_name("progress")
                done, total = job.get("done", 0), job.get("total", 0)
                paused = state == "paused"
                row.pause.set_icon_name("media-playback-start-symbolic" if paused else "media-playback-pause-symbolic")
                row.pause.set_tooltip_text("Resume" if paused else "Pause")
                if state in ("downloading", "verifying") and total:
                    row.bar.set_fraction(min(1.0, done / total))
                    text = "%d%% · %s of %s" % (int(100 * done / total), fmt_bytes(done), fmt_bytes(total))
                    rate = job.get("rate", 0)
                    if state == "downloading" and rate > 1:
                        text += " · %s/s · %s left" % (fmt_bytes(rate), fmt_duration((total - done) / rate))
                    details.append(("Verifying · " if state == "verifying" else "") + text)
                else:
                    row.bar.pulse() if state in ("preparing", "committing") else row.bar.set_fraction(
                        done / total if total else 0)
                    details.append("Moving…" if job.get("action") == "move" and state == "downloading"
                                   else JOB_TEXT.get(state, state.capitalize()))
        elif record:
            row.side.set_visible_child_name("installed")
            update = appid in self.updates
            row.update.set_visible(update)
            row.installed_label.set_visible(not update)
            details.append("Installed in %s · %s" % (record["folder"], fmt_bytes(record.get("size", 0))))
            if record.get("branch") and record["branch"] != "public":
                details.append("Version: %s" % record["branch"])
            if record.get("state") != "installed":
                row.side.set_visible_child_name("install")
                details[-1] = "Download incomplete"
        else:
            row.side.set_visible_child_name("install")
        row.set_subtitle(GLib.markup_escape_text(" · ".join(details)))
        where = self._membership(appid)
        if getattr(row, "where", None) != where:
            row.where = where
            if not getattr(self, "_refilter_pending", False):
                self._refilter_pending = True
                GLib.idle_add(self._deferred_refilter)

    def _deferred_refilter(self):
        self._refilter_pending = False
        self._refilter()
        return False

    def _folders(self):
        return [f for f in (self.app.lib.settings.get("steam_libraries") or []) if f]

    def _ask_install(self, app):
        folders = self._folders()
        if not folders:
            self.app.toast("Add a Steam downloads folder in Loader settings first")
            return
        dialog = Adw.AlertDialog(heading="Install %s?" % (app.get("name") or "App %d" % app["appid"]),
                                 body="Steam downloads the game into the folder you choose. Its saves are kept in "
                                      "its own prefix, separate from any copy you imported yourself.")
        group = Adw.PreferencesGroup()
        folder_row = Adw.ComboRow(title="Folder", model=Gtk.StringList.new(folders))
        group.add(folder_row)
        space = Adw.ActionRow(title="Free space")
        space.add_css_class("property")

        def show_space(*_args):
            folder = folders[folder_row.get_selected()] if folders else ""
            probe = folder
            while probe and not os.path.exists(probe):
                probe = os.path.dirname(probe)
            try:
                stats = os.statvfs(probe or "/")
                space.set_subtitle(fmt_bytes(stats.f_bavail * stats.f_frsize))
            except OSError:
                space.set_subtitle("Unknown")
        folder_row.connect("notify::selected", show_space)
        show_space()
        group.add(space)
        version = self._version_rows(app["appid"], group)
        dialog.set_extra_child(group)
        dialog.add_response("cancel", "Cancel")
        dialog.add_response("install", "Install")
        dialog.set_response_appearance("install", Adw.ResponseAppearance.SUGGESTED)
        dialog.set_default_response("install")

        def answered(_d, response):
            if response == "install":
                self._job("install", app["appid"], folder=folders[folder_row.get_selected()],
                          name=app.get("name") or "", branch=version())
        dialog.connect("response", answered)
        dialog.present(self.app.window)

    def _job(self, op, appid, folder=None, name=None, branch=None, **extra):
        record = self.installs.get(appid)
        folder = folder or (record or {}).get("folder")
        if not folder:
            return
        self.jobs[appid] = {"appid": appid, "state": "queued", "done": 0, "total": 0, "action": op}
        self.updates.discard(appid)
        self._update_row(appid)
        branch = branch or (record or {}).get("branch") or "public"
        self.client.send(op, appid=str(appid), folder=folder, name=name or (record or {}).get("name", ""),
                         language=self.app.lib.settings.get("steam_language", "english"), branch=branch, **extra)

    def _version_rows(self, appid, group, current="public"):
        """Adds a Version row (and an unlock row for password betas) to group; returns a getter for the choice."""
        names = ["public"]
        info = {"public": {"name": "public", "unlocked": True, "password": False, "description": ""}}
        row = Adw.ComboRow(title="Version", subtitle="Loading branches…", model=Gtk.StringList.new(["Public"]))
        unlock = Adw.PasswordEntryRow(title="Beta password", visible=False)
        unlock_button = Gtk.Button(label="Unlock", valign=Gtk.Align.CENTER)
        unlock_button.add_css_class("flat")
        unlock.add_suffix(unlock_button)
        group.add(row)
        group.add(unlock)

        def label(branch):
            entry = info[branch]
            text = "Public" if branch == "public" else branch
            if entry.get("password") and not entry.get("unlocked"):
                text += " (password)"
            return text

        def refresh(*_args):
            selected = names[row.get_selected()] if row.get_selected() < len(names) else "public"
            entry = info.get(selected, {})
            unlock.set_visible(bool(entry.get("password")) and not entry.get("unlocked"))
            row.set_subtitle(GLib.markup_escape_text(entry.get("description") or
                                                     ("The released game" if selected == "public" else "Beta branch")))

        def got_branches(event):
            names[:] = ["public"] + sorted(b["name"] for b in event.get("branches", []) if b["name"] != "public")
            for b in event.get("branches", []):
                info[b["name"]] = b
            row.set_model(Gtk.StringList.new([label(n) for n in names]))
            row.set_selected(names.index(current) if current in names else 0)
            refresh()

        def got_password(event):
            if not event.get("ok"):
                unlock.add_css_class("error")
                self.app.toast(event.get("error", "Wrong beta password"))
                return
            for branch in event.get("branches", []):
                info.setdefault(branch, {"name": branch})["unlocked"] = True
                if branch not in names:
                    names.append(branch)
            selected = names[row.get_selected()]
            row.set_model(Gtk.StringList.new([label(n) for n in names]))
            row.set_selected(names.index(selected))
            unlock.set_text("")
            unlock.remove_css_class("error")
            refresh()

        def try_unlock(*_args):
            password = unlock.get_text()
            if password:
                self.waiters[("beta_password", appid)] = got_password
                self.client.send("beta_password", appid=str(appid), password=password)

        unlock_button.connect("clicked", try_unlock)
        unlock.connect("entry-activated", try_unlock)
        row.connect("notify::selected", refresh)
        self.waiters[("app_branches", appid)] = got_branches
        self.client.send("app_branches", appid=str(appid))

        def chosen():
            branch = names[row.get_selected()] if row.get_selected() < len(names) else "public"
            entry = info.get(branch, {})
            return branch if entry.get("unlocked", True) or not entry.get("password") else "public"
        return chosen

    def _retry(self, app):
        appid = app["appid"]
        self.jobs.pop(appid, None)
        record = self.installs.get(appid)
        if record:
            self._job("update" if record.get("state") == "installed" else "install", appid)
        else:
            self._ask_install(app)

    def _toggle_pause(self, appid):
        job = self.jobs.get(appid)
        if job:
            self.client.send("resume" if job["state"] == "paused" else "pause", appid=str(appid))

    def _installed_action(self, action, appid):
        record = self.installs.get(appid)
        if not record:
            return
        if action == "open":
            path = os.path.join(record["folder"], "common", record["installdir"])
            subprocess.Popen(["xdg-open", path], stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                             stderr=subprocess.DEVNULL, start_new_session=True)
        elif action == "verify":
            self._job("verify", appid)
        elif action == "version":
            dialog = Adw.AlertDialog(heading="Version of %s" % record.get("name", ""),
                                     body="Switching downloads only what differs between the versions.")
            group = Adw.PreferencesGroup()
            version = self._version_rows(appid, group, current=record.get("branch") or "public")
            dialog.set_extra_child(group)
            dialog.add_response("cancel", "Cancel")
            dialog.add_response("switch", "Switch")
            dialog.set_response_appearance("switch", Adw.ResponseAppearance.SUGGESTED)
            dialog.connect("response", lambda _d, r: r == "switch" and self._job("update", appid, branch=version()))
            dialog.present(self.app.window)
        elif action == "move":
            targets = [f for f in self._folders() if f != record["folder"]]
            if not targets:
                self.app.toast("Add another Steam downloads folder in Loader settings first")
                return
            dialog = Adw.AlertDialog(heading="Move %s?" % record.get("name", ""),
                                     body="The game's folder moves as a whole, including saves or mods you added to it.")
            group = Adw.PreferencesGroup()
            target_row = Adw.ComboRow(title="To", model=Gtk.StringList.new(targets))
            group.add(target_row)
            dialog.set_extra_child(group)
            dialog.add_response("cancel", "Cancel")
            dialog.add_response("move", "Move")
            dialog.set_response_appearance("move", Adw.ResponseAppearance.SUGGESTED)
            dialog.connect("response", lambda _d, r: r == "move" and self._job(
                "move", appid, to=targets[target_row.get_selected()]))
            dialog.present(self.app.window)
        elif action == "uninstall":
            dialog = Adw.AlertDialog(heading="Uninstall %s?" % record.get("name", ""),
                                     body="The game's files are deleted. Files you added to its folder, such as "
                                          "saves or mods, are kept.")
            dialog.add_response("cancel", "Cancel")
            dialog.add_response("uninstall", "Uninstall")
            dialog.set_response_appearance("uninstall", Adw.ResponseAppearance.DESTRUCTIVE)
            dialog.connect("response", lambda _d, r: r == "uninstall" and self.client.send(
                "uninstall", appid=str(appid), folder=record["folder"]))
            dialog.present(self.app.window)

    def _refresh_installs(self):
        folders = self._folders()
        if self.client.connected():
            self.client.send("installs", folders="\n".join(folders))
            self.client.send("downloads")

    def _show_installs(self, event):
        self.installs = {a["appid"]: a for a in event.get("apps", [])}
        for appid in list(self.rows):
            self._update_row(appid)
        self.app.sync_steam_installs(list(self.installs.values()))
        if self.installs and not self.checked_updates:
            self.checked_updates = True
            self.client.send("check_updates", folders="\n".join(self._folders()))

    def _on_download(self, event):
        appid = event.get("appid")
        state = event.get("state")
        self.jobs[appid] = event
        self._update_row(appid)
        if state == "installed":
            self.jobs.pop(appid, None)
            if event.get("action") == "verify":
                self.app.toast("%s: files verified" % event.get("name", ""))
            elif event.get("action") == "move":
                self.app.toast("%s moved" % event.get("name", ""))
            else:
                self.app.toast("%s is ready to play" % event.get("name", ""))
            self._refresh_installs()
        elif state == "cancelled":
            self.jobs.pop(appid, None)
            self._refresh_installs()
        elif state == "error":
            self.app.toast("%s: %s" % (event.get("name", ""), event.get("error", "")))

    def _membership(self, appid):
        job = self.jobs.get(appid)
        if job and job.get("state") not in ("installed", "cancelled"):
            return "downloading"
        record = self.installs.get(appid)
        return "installed" if record and record.get("state") == "installed" else "library"

    def _in_section(self, app, section):
        where = self._membership(app["appid"])
        if section == "library":
            return self.show_all.get_active() or self._primary(app) or where != "library"
        return where == section

    def _filter_row(self, row):
        app = getattr(row, "app", None)
        if app is None:
            return True
        if not self._in_section(app, self.section.get_active_name() or "installed"):
            return False
        query = self.search.get_text().strip().casefold()
        return not query or query in row.search_key

    def _refilter(self):
        self.library_list.invalidate_filter()
        query = self.search.get_text().strip().casefold()
        section = self.section.get_active_name() or "installed"
        everything = self.show_all.get_active()
        for name, label in (("installed", "Installed"), ("downloading", "Downloading"), ("library", "Library")):
            count = sum(1 for a in self.apps if self._in_section(a, name))
            toggle = self.section.get_toggle_by_name(name)
            if toggle:
                toggle.set_label("%s (%d)" % (label, count) if count and name != "library" else label)
        self.show_all.set_visible(section == "library")
        pool = [a for a in self.apps if self._in_section(a, section)]
        shown = [a for a in pool if not query or query in (a.get("name") or "").casefold()]
        games = sum(1 for a in self.apps if self._primary(a))
        others = len(self.apps) - games
        count = "%d games" % games if games != 1 else "1 game"
        if others:
            count += " · %d other item%s %s" % (others, "" if others == 1 else "s", "shown" if everything else "hidden")
        self.library_count.set_label(count)
        if not shown:
            empty = {"installed": ("Nothing installed yet", "Install games from the Library tab."),
                     "downloading": ("No downloads", "Installs, updates and repairs show up here while they run."),
                     "library": ("No games yet", "Games you own on Steam show up here.")}[section]
            self.library_message.set_title("No matches" if query else empty[0])
            self.library_message.set_description(
                GLib.markup_escape_text("Nothing here matches “%s”." % self.search.get_text().strip())
                if query else empty[1])
            self.library_stack.set_visible_child_name("message")
        else:
            self.library_stack.set_visible_child_name("list")

    # ---------------------------------------------------------------- provider connection
    def send_bandwidth(self):
        if self.client.connected():
            limit = float(self.app.lib.settings.get("steam_bandwidth_mbps") or 0)
            self.client.send("bandwidth", bytes_per_second=str(int(limit * 1_000_000)))

    def provider_binary(self):
        return self.app.lib.settings.get("steam_provider") or DEFAULT_PROVIDER

    def _connect(self, force_start=False):
        if self.client.connected():
            return False
        if self.client.connect():
            self.client.send("status")
            return False
        binary = self.provider_binary()
        if not os.access(binary, os.X_OK):
            self._unavailable("The provider program was not found at %s. Build it with “make beer-steam” in the "
                              "Beer folder, or set its path in Loader settings." % binary)
            return False
        if self.started_provider and not force_start:
            self._unavailable("The provider stopped. Its log is in %s." % self._log_path())
            return False
        self.started_provider = True
        self.views.set_visible_child_name("connecting")
        self.connecting.set_title("Starting the Steam provider…")
        try:
            os.makedirs(storage.LOG_DIR, exist_ok=True)
            with open(self._log_path(), "ab") as log:
                subprocess.Popen([binary, "serve"], stdin=subprocess.DEVNULL, stdout=log, stderr=subprocess.STDOUT,
                                 start_new_session=True)
        except OSError as error:
            self._unavailable("Could not start %s: %s" % (binary, error))
            return False
        deadline = time.monotonic() + 5

        def retry():
            if self.client.connect():
                self.client.send("status")
                return False
            if time.monotonic() > deadline:
                self._unavailable("The provider did not start. Its log is in %s." % self._log_path())
                return False
            return True
        GLib.timeout_add(200, retry)
        return False

    @staticmethod
    def _log_path():
        return os.path.join(storage.LOG_DIR, "beer-steam.log")

    def _unavailable(self, reason):
        self.unavailable.set_description(GLib.markup_escape_text(reason))
        self.views.set_visible_child_name("unavailable")

    # ---------------------------------------------------------------- events
    def _on_event(self, event):
        kind = event.get("event")
        if kind == "state":
            self.state = event
            self._show_state()
        elif kind == "qr" and self.login_kind == "qr":
            self.qr_code.set_text(event.get("url", ""))
            self.qr_stack.set_visible_child_name("code")
        elif kind == "guard":
            self._show_guard(event)
        elif kind == "login":
            self._login_finished(event)
        elif kind in ("app_branches", "beta_password"):
            waiter = self.waiters.pop((kind, event.get("appid")), None)
            if waiter is None and kind == "beta_password":
                waiter = next((self.waiters.pop(k) for k in list(self.waiters) if k[0] == kind), None)
            if waiter:
                waiter(event)
        elif kind == "library":
            self._show_library(event)
        elif kind == "installs":
            self._show_installs(event)
        elif kind == "download":
            self._on_download(event)
        elif kind == "downloads":
            for job in event.get("jobs", []):
                self.jobs[job["appid"]] = job
                self._update_row(job["appid"])
        elif kind == "updates":
            self.updates = set(event.get("apps", []))
            for appid in self.updates:
                self._update_row(appid)
            if self.updates:
                self.app.toast("Updates are available for %d game%s" % (len(self.updates), "" if len(self.updates) == 1 else "s"))
        elif kind == "uninstalled":
            self.installs.pop(event.get("appid"), None)
            self._refresh_installs()
            self.app.toast("Uninstalled")
        elif kind == "error" and event.get("op"):
            appid = event.get("appid")
            self.jobs.pop(appid, None)
            self._update_row(appid)
            self.app.toast(event.get("error", "Steam request failed"))
        elif kind == "warning":
            self.app.toast(event.get("message", ""))
        elif kind == "disconnected":
            self.login_kind = None
            self.connecting.set_title("Connecting to Steam…")
            self.views.set_visible_child_name("connecting")
            GLib.timeout_add(500, self._connect)
        return False

    def _show_state(self):
        state = self.state.get("state")
        if state == "connecting":
            self.connecting.set_title("Connecting to Steam…")
            self.views.set_visible_child_name("connecting")
        elif state == "signed_in":
            self.login_kind = None
            persona = self.state.get("persona") or self.state.get("account", "")
            self.avatar.set_text(persona)
            self.profile_row.set_title(GLib.markup_escape_text(persona))
            self.profile_row.set_subtitle(GLib.markup_escape_text("Signed in as %s" % self.state.get("account", "")))
            self.steamid_row.set_subtitle(self.state.get("steamid", ""))
            self.storage_row.set_subtitle("System keyring" if self.state.get("secure_storage")
                                          else "Not stored: no keyring is available, so you will sign in again next time")
            if self.views.get_visible_child_name() != "signed_in":
                self.client.send("library")
                self._refresh_installs()
                self.send_bandwidth()
            self.views.set_visible_child_name("signed_in")
        elif self.login_kind is None or self.views.get_visible_child_name() != "guard":
            self.views.set_visible_child_name("signed_out")

    def _show_method(self):
        name = self.method.get_active_name() or "qr"
        self.methods.set_visible_child_name(name)
        if self.login_kind == "qr" and name != "qr":
            self._cancel_login()

    def _set_error(self, label, text):
        text = (text[0].upper() + text[1:]) if text else ""
        label.set_label(text)
        label.set_visible(bool(text))

    # ---------------------------------------------------------------- sign-in
    def _begin_qr(self):
        if self.login_kind:
            return
        self.login_kind = "qr"
        self._set_error(self.signin_error, None)
        self.qr_stack.set_visible_child_name("waiting")
        self.qr_cancel.set_visible(True)
        self.client.send("login_qr")

    def _begin_password(self):
        if self.login_kind:
            return
        account = self.account_row.get_text().strip()
        password = self.password_row.get_text()
        if not account or not password:
            self._set_error(self.signin_error, "Enter your account name and password.")
            return
        self.login_kind = "password"
        self._set_error(self.signin_error, None)
        self.sign_in.set_sensitive(False)
        self.client.send("login_password", account=account, password=password)
        self.password_row.set_text("")

    def _show_guard(self, event):
        types = event.get("types", [])
        hint = event.get("hint", "")
        hint_text = " (%s)" % hint if hint else ""
        codes = [GUARD_TEXT[t].format(hint=hint_text) for t in types if t in ("device_code", "email_code")]
        approvals = [GUARD_TEXT[t].format(hint=hint_text) for t in types
                     if t in ("device_confirmation", "email_confirmation")]
        takes_code, confirms = bool(codes), bool(approvals)
        if codes:
            text = codes[0] + (" You can also approve the sign-in in your Steam mobile app instead." if confirms else "")
        else:
            text = approvals[0] if approvals else "Steam needs to confirm this sign-in."
        self.guard_status.set_description(GLib.markup_escape_text(text))
        self.guard_fields.set_visible(takes_code)
        self.guard_waiting.set_visible(confirms and not takes_code)
        self._set_error(self.guard_error, event.get("error"))
        self.code_row.set_text("")
        self.views.set_visible_child_name("guard")
        if takes_code:
            self.code_row.grab_focus()

    def _submit_code(self):
        code = self.code_row.get_text().strip()
        if not code:
            return
        self._set_error(self.guard_error, None)
        self.client.send("guard_code", code=code)
        self.code_row.set_text("")

    def _cancel_login(self):
        if self.login_kind:
            self.client.send("login_cancel")

    def _login_finished(self, event):
        kind, self.login_kind = self.login_kind, None
        self.sign_in.set_sensitive(True)
        self.qr_cancel.set_visible(False)
        self.qr_stack.set_visible_child_name("idle")
        self.qr_code.set_text("")
        if event.get("ok"):
            self.app.toast("Signed in to Steam as %s" % event.get("account", ""))
            return
        error = event.get("error", "")
        if error != "cancelled":
            self._set_error(self.signin_error, "Sign-in failed: %s" % error)
        if kind and self.state.get("state") != "signed_in":
            self.views.set_visible_child_name("signed_out")

    def _confirm_sign_out(self):
        dialog = Adw.AlertDialog(heading="Sign out of Steam?",
                                 body="Beer forgets the stored session. Installed games stay on disk.")
        dialog.add_response("cancel", "Cancel")
        dialog.add_response("sign_out", "Sign out")
        dialog.set_response_appearance("sign_out", Adw.ResponseAppearance.DESTRUCTIVE)
        dialog.connect("response", lambda _d, response: response == "sign_out" and self.client.send("logout"))
        dialog.present(self.app.window)
