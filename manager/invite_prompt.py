#!/usr/bin/env python3
"""Join prompt for a friend's lobby invite (Steam's invite notification and its Join button, which the Steam overlay
shows on Windows). The loader opens it when an invite reaches the signed-in account while a game runs.

    invite_prompt.py --name "Friend"     exit status 0: join, 1: not now (closed, declined or timed out)
"""
import argparse
import sys

import gi

gi.require_version("Gtk", "4.0")
gi.require_version("Adw", "1")
from gi.repository import Adw, GLib, Gtk  # noqa: E402

TIMEOUT_SECONDS = 60


class Prompt(Adw.Application):
    def __init__(self, name):
        super().__init__(application_id=None)
        self.name = name
        self.result = 1

    def do_activate(self):
        window = Adw.ApplicationWindow(application=self, title="Game invite", default_width=380, resizable=False)
        box = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=12, margin_top=18, margin_bottom=18,
                      margin_start=18, margin_end=18)
        title = Gtk.Label(label="%s invited you to play" % self.name, wrap=True, css_classes=["title-3"])
        box.append(title)
        box.append(Gtk.Label(label="Join their lobby now?", css_classes=["dim-label"]))
        buttons = Gtk.Box(spacing=12, halign=Gtk.Align.END, margin_top=6)
        ignore = Gtk.Button(label="Not now")
        ignore.connect("clicked", lambda *_: self.finish(1))
        join = Gtk.Button(label="Join", css_classes=["suggested-action"])
        join.connect("clicked", lambda *_: self.finish(0))
        buttons.append(ignore)
        buttons.append(join)
        box.append(buttons)
        window.set_content(box)
        window.connect("close-request", lambda *_: self.finish(1) or False)
        window.present()
        join.grab_focus()
        GLib.timeout_add_seconds(TIMEOUT_SECONDS, lambda: self.finish(1))

    def finish(self, result):
        self.result = result
        self.quit()
        return False


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--name", required=True)
    args = parser.parse_args()
    prompt = Prompt(args.name)
    prompt.run([sys.argv[0]])
    return prompt.result


if __name__ == "__main__":
    sys.exit(main())
