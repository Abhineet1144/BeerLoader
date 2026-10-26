"""Where the manager keeps its files: everything lives under ~/.beer (prefixes, library, settings, logs)."""
import os
import shutil

BEER_HOME = os.path.expanduser("~/.beer")
LOG_DIR = os.path.join(BEER_HOME, "logs")

# Files the manager used to keep in XDG locations; copied (never moved or deleted) on first start.
_LEGACY = {
    "library.json": os.path.join(os.environ.get("XDG_CONFIG_HOME", os.path.expanduser("~/.config")), "beer", "library.json"),
    "manager.json": os.path.join(os.environ.get("XDG_CONFIG_HOME", os.path.expanduser("~/.config")), "beer", "manager.json"),
    "overlay.json": os.path.join(os.environ.get("XDG_CONFIG_HOME", os.path.expanduser("~/.config")), "beer", "overlay.json"),
}


def path(name):
    return os.path.join(BEER_HOME, name)


def migrate():
    os.makedirs(BEER_HOME, exist_ok=True)
    os.makedirs(LOG_DIR, exist_ok=True)
    for name, old in _LEGACY.items():
        new = path(name)
        if not os.path.exists(new) and os.path.exists(old):
            try:
                shutil.copy2(old, new)
            except OSError:
                pass
