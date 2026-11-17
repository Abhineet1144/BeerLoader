"""Mod packages (.bmod): build one from a mod folder, check one, install one into ~/.beer/mods.

A .bmod file is a zip archive with the mod's mod.json at its root (a single top-level folder that contains mod.json is
accepted too). Installing unpacks it into ~/.beer/mods/<id>/; an installed copy of the same mod is replaced, but the
user's settings.json and data.json are kept.

    python3 -m beer_mod.package pack <mod folder> [-o out.bmod]     build <id>-<version>.bmod
    python3 -m beer_mod.package check <file.bmod>                   validate and list the contents
    python3 -m beer_mod.package install <file.bmod>                 install without the manager
"""
import json
import os
import re
import shutil
import stat
import sys
import tempfile
import zipfile

EXTENSION = ".bmod"
MODS_DIR = os.path.join(os.path.expanduser("~"), ".beer", "mods")
USER_FILES = ("settings.json", "data.json")          # belong to the user, never packed, kept on reinstall
ID_PATTERN = re.compile(r"^[a-z0-9][a-z0-9._-]{0,63}$")
MAX_FILES = 2000
MAX_BYTES = 256 << 20                                  # unpacked size
SKIP_DIRS = {"__pycache__", ".git", ".hg", ".svn"}


class PackageError(Exception):
    """A .bmod that cannot be built or installed; the message says why."""


def check_manifest(manifest, where="mod.json"):
    if not isinstance(manifest, dict):
        raise PackageError("%s is not a JSON object" % where)
    mod_id = manifest.get("id")
    if not isinstance(mod_id, str) or not ID_PATTERN.match(mod_id):
        raise PackageError("%s: \"id\" must be lower-case letters, digits, '.', '_' or '-' (got %r)" % (where, mod_id))
    for key in ("name", "version"):
        if not isinstance(manifest.get(key), str) or not manifest[key].strip():
            raise PackageError("%s: \"%s\" is missing" % (where, key))
    entry = manifest.get("entry", "main.py")
    if not isinstance(entry, str) or os.path.isabs(entry) or ".." in entry.replace("\\", "/").split("/"):
        raise PackageError("%s: \"entry\" must be a file inside the mod (got %r)" % (where, entry))
    return mod_id


# ---- building -----------------------------------------------------------------------------------------------------
def pack(folder, output=None):
    """Builds a .bmod from a mod folder; returns its path."""
    folder = os.path.abspath(folder)
    try:
        with open(os.path.join(folder, "mod.json")) as f:
            manifest = json.load(f)
    except (OSError, ValueError) as error:
        raise PackageError("cannot read %s/mod.json: %s" % (folder, error))
    mod_id = check_manifest(manifest)
    entry = manifest.get("entry", "main.py")
    if not os.path.isfile(os.path.join(folder, entry)):
        raise PackageError("entry %s does not exist in %s" % (entry, folder))
    output = output or os.path.join(os.getcwd(), "%s-%s%s" % (mod_id, manifest["version"], EXTENSION))
    files = []
    for root, dirs, names in os.walk(folder):
        dirs[:] = sorted(d for d in dirs if d not in SKIP_DIRS and not d.startswith("."))
        for name in sorted(names):
            path = os.path.join(root, name)
            rel = os.path.relpath(path, folder).replace(os.sep, "/")
            if rel in USER_FILES or name.endswith((".pyc", EXTENSION)) or name.startswith(".") or os.path.islink(path):
                continue
            if os.path.abspath(path) == os.path.abspath(output):
                continue
            files.append((path, rel))
    if len(files) > MAX_FILES:
        raise PackageError("too many files (%d, at most %d)" % (len(files), MAX_FILES))
    tmp = output + ".part"
    with zipfile.ZipFile(tmp, "w", zipfile.ZIP_DEFLATED) as z:
        for path, rel in files:
            z.write(path, rel)
    os.replace(tmp, output)
    return output


# ---- reading ------------------------------------------------------------------------------------------------------
def inspect(path):
    """(manifest, members, prefix) of a .bmod after validating it. members are (zip name, relative path)."""
    try:
        z = zipfile.ZipFile(path)
    except (OSError, zipfile.BadZipFile) as error:
        raise PackageError("%s is not a mod package: %s" % (os.path.basename(path), error))
    with z:
        infos = [i for i in z.infolist() if not i.is_dir()]
        names = [i.filename for i in infos]
        prefix = ""
        if "mod.json" not in names:
            tops = {n.split("/", 1)[0] for n in names}
            if len(tops) == 1 and "%s/mod.json" % next(iter(tops)) in names:
                prefix = next(iter(tops)) + "/"
            else:
                raise PackageError("no mod.json at the top of %s" % os.path.basename(path))
        if len(infos) > MAX_FILES:
            raise PackageError("too many files (%d)" % len(infos))
        total, members = 0, []
        for info in infos:
            name = info.filename
            rel = name[len(prefix):] if name.startswith(prefix) else None
            parts = (rel or "").replace("\\", "/").split("/")
            if rel is None or not rel or name.startswith("/") or ".." in parts or ":" in parts[0]:
                raise PackageError("unsafe path in package: %r" % name)
            if stat.S_ISLNK(info.external_attr >> 16):
                raise PackageError("links are not allowed in packages: %r" % name)
            total += info.file_size
            if total > MAX_BYTES:
                raise PackageError("package unpacks to more than %d MB" % (MAX_BYTES >> 20))
            if rel in USER_FILES:
                continue                            # never overwrite the user's own files
            members.append((name, rel))
        try:
            manifest = json.loads(z.read(prefix + "mod.json").decode("utf-8"))
        except ValueError as error:
            raise PackageError("mod.json is not valid JSON: %s" % error)
    check_manifest(manifest)
    entry = manifest.get("entry", "main.py")
    if entry not in {rel for _, rel in members}:
        raise PackageError("the package has no %s (its \"entry\")" % entry)
    return manifest, members, prefix


# ---- installing ---------------------------------------------------------------------------------------------------
def installed_manifest(mod_id, mods_dir=MODS_DIR):
    try:
        with open(os.path.join(mods_dir, mod_id, "mod.json")) as f:
            return json.load(f)
    except (OSError, ValueError):
        return None


def install(path, mods_dir=MODS_DIR):
    """Installs a .bmod; returns (manifest, previous manifest or None). Unpacks into a temporary folder first, so a
    broken package never leaves a half-installed mod behind."""
    manifest, members, _prefix = inspect(path)
    mod_id = manifest["id"]
    os.makedirs(mods_dir, exist_ok=True)
    target = os.path.join(mods_dir, mod_id)
    previous = installed_manifest(mod_id, mods_dir)
    staging = tempfile.mkdtemp(prefix=".install-%s-" % mod_id, dir=mods_dir)
    try:
        with zipfile.ZipFile(path) as z:
            for name, rel in members:
                dest = os.path.join(staging, *rel.split("/"))
                if not os.path.abspath(dest).startswith(os.path.abspath(staging) + os.sep):
                    raise PackageError("unsafe path in package: %r" % name)
                os.makedirs(os.path.dirname(dest), exist_ok=True)
                with z.open(name) as src, open(dest, "wb") as out:
                    shutil.copyfileobj(src, out)
        if os.path.isdir(target):
            for name in USER_FILES:
                if os.path.isfile(os.path.join(target, name)):
                    shutil.copy2(os.path.join(target, name), os.path.join(staging, name))
            old = tempfile.mkdtemp(prefix=".old-%s-" % mod_id, dir=mods_dir)
            os.rmdir(old)
            os.rename(target, old)
            os.rename(staging, target)
            shutil.rmtree(old, ignore_errors=True)
        else:
            os.rename(staging, target)
    except BaseException:
        shutil.rmtree(staging, ignore_errors=True)
        raise
    return manifest, previous


def uninstall(mod_id, mods_dir=MODS_DIR):
    """Deletes an installed mod, including its settings and saved data."""
    if not ID_PATTERN.match(mod_id or ""):
        raise PackageError("bad mod id %r" % mod_id)
    target = os.path.join(mods_dir, mod_id)
    if os.path.isdir(target):
        shutil.rmtree(target)


def main(argv):
    if len(argv) >= 2 and argv[0] == "pack":
        out = argv[argv.index("-o") + 1] if "-o" in argv else None
        print(pack(argv[1], out))
    elif len(argv) == 2 and argv[0] == "check":
        manifest, members, _ = inspect(argv[1])
        print("%s %s (%s): %d files" % (manifest["name"], manifest["version"], manifest["id"], len(members)))
        for _, rel in members:
            print("  " + rel)
    elif len(argv) == 2 and argv[0] == "install":
        manifest, previous = install(argv[1])
        print("installed %s %s%s" % (manifest["id"], manifest["version"],
                                     " (was %s)" % previous.get("version") if previous else ""))
    else:
        print(__doc__.strip())
        return 2
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main(sys.argv[1:]))
    except PackageError as error:
        print("error: %s" % error, file=sys.stderr)
        sys.exit(1)
