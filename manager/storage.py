"""Where the manager keeps its files: everything lives under ~/.beer (prefixes, library, settings, logs)."""
import os
import shutil

BEER_HOME = os.path.expanduser("~/.beer")
LOG_DIR = os.path.join(BEER_HOME, "logs")
# Shader caches (Vulkan pipeline caches and translated shaders); the loader gets the folder as BEER_CACHE_DIR.
CACHE_DIR = os.path.join(BEER_HOME, "cache")
_LEGACY_CACHE = os.path.join(os.environ.get("XDG_CACHE_HOME", os.path.expanduser("~/.cache")), "beer")

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


def migrate_cache(target):
    """First use of a cache folder: the caches the loader kept in ~/.cache/beer move there (a rename on the same
    filesystem, so no extra disk space and nothing is lost), and games do not compile everything again. On another
    filesystem the new folder starts empty instead of copying gigabytes. Runs once: an existing target is left alone."""
    if os.path.exists(target) or not os.path.isdir(_LEGACY_CACHE):
        os.makedirs(target, exist_ok=True)
        return
    try:
        os.makedirs(os.path.dirname(target) or ".", exist_ok=True)
        os.rename(_LEGACY_CACHE, target)
    except OSError:
        os.makedirs(target, exist_ok=True)
