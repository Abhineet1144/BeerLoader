"""RE Engine reflection: find classes, fields and live objects by NAME in Capcom RE Engine games.

An optional helper library for mods, kept outside the core SDK (beer_mod): only mods for RE Engine games import it.

RE Engine games carry a type database ("TDB") in memory: every managed class with its namespace, parent, size and
fields (name, type, offset). Each live managed object starts with a pointer to its class's runtime object info, so
"every live object of class X" is a single memory search. Nothing here is specific to one game; layouts are decoded
from the TDB of the running game.

    from reengine import REEngine       # mods/libs is on PYTHONPATH for every mod the manager starts
    re = REEngine(mod.game)
    dc = re.type("app.DamageController")
    off = dc.field("HealthInfo").offset
    for obj in re.instances("app.DamageController"):
        ...

Supported TDB versions: 69 (Resident Evil Village). Other versions raise ModError until their layout is added.
"""
import hashlib
import json
import os
import re as _re
import struct
import time

from beer_mod import ModError

MAGIC = 0x00424454          # "TDB\0"
OBJECT_HEADER = 0x10        # managed objects: object info pointer + reference count, fields follow
CACHE_DIR = os.path.join(os.path.expanduser("~"), ".beer", "cache", "reengine")

# Per TDB version: header offsets of counts and table pointers, entry sizes and bit layouts.
LAYOUTS = {
    69: {
        "count_types": 0x0C, "count_fields": 0x14, "count_type_impl": 0x18, "count_field_impl": 0x1C,
        "string_pool_size": 0x50,
        "ptr_types": 0x60, "ptr_type_impl": 0x68, "ptr_fields": 0x80, "ptr_field_impl": 0x88, "ptr_strings": 0xC8,
        "type_size": 80, "type_impl_size": 48, "field_size": 8, "field_impl_size": 12,
        "type_obj_size": 0x14, "type_object_info": 0x48,
        "impl_shift": 100,
    },
}


class Field:
    __slots__ = ("name", "offset", "type_index", "attributes", "_re")

    def __init__(self, engine, name, offset, type_index, attributes):
        self._re, self.name, self.offset, self.type_index, self.attributes = engine, name, offset, type_index, attributes

    @property
    def type_name(self):
        return self._re.type_name(self.type_index)

    @property
    def is_static(self):
        return bool(self.attributes & 0x10)

    def __repr__(self):
        return "<Field %s +%#x : %s%s>" % (self.name, self.offset, self.type_name, " static" if self.is_static else "")


class Type:
    """One managed class. `fields` holds this class's own fields; `field(name)` also searches the parents."""

    def __init__(self, engine, index):
        self._re, self.index = engine, index

    @property
    def name(self):
        return self._re.type_name(self.index)

    @property
    def size(self):
        return self._re._type_u32(self.index, self._re.L["type_obj_size"])

    @property
    def parent(self):
        p = self._re._bits(self.index, 18)
        return Type(self._re, p) if p and p != self.index else None

    @property
    def object_info(self):
        """The value every live instance of exactly this class holds in its first qword. Read live: the engine fills
        it in when the class is first used, which may be after the tables were loaded."""
        cached = self._re._type_u64(self.index, self._re.L["type_object_info"])
        if cached:
            return cached
        live = self._re.game.read_ptr(self._re.types_ptr + self.index * self._re.L["type_size"] + self._re.L["type_object_info"])
        return live or 0

    @property
    def fields(self):
        return self._re._fields_of(self.index)

    def field(self, name):
        t = self
        while t:
            for f in t.fields:
                if f.name == name or f.name == "<%s>k__BackingField" % name:
                    return f
            t = t.parent
        raise ModError("%s has no field %r" % (self.name, name))

    def all_fields(self):
        chain, t = [], self
        while t:
            chain.append(t)
            t = t.parent
        return [f for t in reversed(chain) for f in t.fields]

    def subclasses(self):
        return [Type(self._re, k) for k in self._re._subclass_indices(self.index)]

    def __repr__(self):
        return "<Type %s size %#x>" % (self.name, self.size)


class REEngine:
    def __init__(self, game, progress=None, wait=180.0):
        """Waits up to `wait` seconds for the game to set up its type database: a mod may start the moment the game
        does, before the database's tables are in place."""
        self.game = game
        self.progress = progress or (lambda text: None)
        deadline = time.monotonic() + wait
        while True:
            if self._locate():
                break
            if time.monotonic() > deadline:
                raise ModError("the RE Engine type database never became ready (game still loading?)")
            self.progress("waiting for the game to finish starting...")
            time.sleep(1.0)
        self._load_tables()
        self._names = {}
        self._by_name = None
        self._fields = None
        self._children = None
        self._info_to_index = None

    # ---- loading --------------------------------------------------------------------------------------------------
    def _locate(self):
        modules = self.game.modules()
        exe_name, (base, size) = next(iter(modules.items()))
        self.exe_name, self.exe_base = exe_name, base
        candidates = [MAGIC | (v << 32) for v in range(40, 120)]     # magic + version qword
        found = self.game.find_values(candidates, limit=4, writable=False)
        hits = sorted((addr, value >> 32) for value, (_, addrs) in found.items() for addr in addrs
                      if base <= addr < base + size)
        if not hits:
            return False                          # not there yet (or not an RE Engine game: the wait ends it)
        for addr, version in hits:
            if version not in LAYOUTS:
                raise ModError("RE Engine TDB version %d is not supported yet (supported: %s)" %
                               (version, ", ".join(map(str, LAYOUTS))))
            if self._header_ready(addr, LAYOUTS[version], base, size):
                self.tdb, self.version, self.L = addr, version, LAYOUTS[version]
                return True
        return False                              # found, but its tables are not set up yet

    def _header_ready(self, addr, L, base, size):
        """Counts are set and every table pointer points into the executable image (the copy the game uses)."""
        header = self.game.read_bytes(addr, 0x100)
        if not header or len(header) < 0x100:
            return False
        u32 = lambda off: struct.unpack_from("<I", header, off)[0]
        u64 = lambda off: struct.unpack_from("<Q", header, off)[0]
        if not u32(L["count_types"]) or not u32(L["count_fields"]):
            return False
        for key in ("ptr_types", "ptr_type_impl", "ptr_fields", "ptr_field_impl", "ptr_strings"):
            if not base <= u64(L[key]) < base + size:
                return False
        return True

    def _read_block(self, addr, size):
        data, chunk = bytearray(), 65536
        while len(data) < size:
            want = min(chunk, size - len(data))
            piece = self.game.read_bytes(addr + len(data), want)
            if piece is None and chunk > 8192:
                chunk = 8192                      # older loaders: 8 KiB reads
                continue
            if piece is None:
                raise ModError("cannot read the type database at %#x" % (addr + len(data)))
            data += piece
        return bytes(data)

    def _load_tables(self):
        L = self.L
        header = self.game.read_bytes(self.tdb, 0x100)
        u32 = lambda off: struct.unpack_from("<I", header, off)[0]
        u64 = lambda off: struct.unpack_from("<Q", header, off)[0]
        self.n_types, self.n_fields = u32(L["count_types"]), u32(L["count_fields"])
        self.n_type_impl, self.n_field_impl = u32(L["count_type_impl"]), u32(L["count_field_impl"])
        self.types_ptr = u64(L["ptr_types"])
        plan = {
            "types": (self.types_ptr, self.n_types * L["type_size"]),
            "type_impl": (u64(L["ptr_type_impl"]), self.n_type_impl * L["type_impl_size"]),
            "fields": (u64(L["ptr_fields"]), self.n_fields * L["field_size"]),
            "field_impl": (u64(L["ptr_field_impl"]), self.n_field_impl * L["field_impl_size"]),
            "strings": (u64(L["ptr_strings"]), u32(L["string_pool_size"])),
        }
        key = hashlib.sha1(header + self.exe_name.encode()).hexdigest()[:16]
        folder = os.path.join(CACHE_DIR, key)
        self.tables = {}
        if all(os.path.exists(os.path.join(folder, n)) for n in plan):
            for n in plan:
                with open(os.path.join(folder, n), "rb") as f:
                    self.tables[n] = f.read()
            # The tables live in the executable image, but runtime object-info pointers can move between runs:
            # re-read the (small) types table so object_info is current.
            self.tables["types"] = self._read_block(*plan["types"])
            return
        total = sum(size for _, size in plan.values())
        done = 0
        for n, (addr, size) in plan.items():
            self.progress("reading RE Engine type database: %d%%" % (100 * done // total))
            self.tables[n] = self._read_block(addr, size)
            done += size
        os.makedirs(folder, exist_ok=True)
        for n, data in self.tables.items():
            with open(os.path.join(folder, n), "wb") as f:
                f.write(data)

    # ---- raw access ---------------------------------------------------------------------------------------------
    def _type_u32(self, k, off):
        return struct.unpack_from("<I", self.tables["types"], k * self.L["type_size"] + off)[0]

    def _type_u64(self, k, off):
        return struct.unpack_from("<Q", self.tables["types"], k * self.L["type_size"] + off)[0]

    def _bits(self, k, shift, width=18):
        base = k * self.L["type_size"] + (shift // 64) * 8
        q = struct.unpack_from("<Q", self.tables["types"], base)[0]
        q2 = struct.unpack_from("<Q", self.tables["types"], base + 8)[0] if (shift % 64) + width > 64 else 0
        return (((q2 << 64) | q) >> (shift % 64)) & ((1 << width) - 1)

    def _string(self, off):
        pool = self.tables["strings"]
        if not 0 <= off < len(pool):
            return ""
        end = pool.find(b"\0", off)
        return pool[off:end].decode("utf-8", "replace")

    # ---- names --------------------------------------------------------------------------------------------------
    def type_name(self, k):
        name = self._names.get(k)
        if name is not None:
            return name
        if not 0 <= k < self.n_types:
            return "#%d" % k
        impl = self._bits(k, self.L["impl_shift"])
        rec = impl * self.L["type_impl_size"]
        n_off, ns_off = struct.unpack_from("<II", self.tables["type_impl"], rec)
        short, namespace = self._string(n_off), self._string(ns_off)
        declaring = self._bits(k, 36)
        if namespace:
            name = namespace + "." + short
        elif declaring and declaring != k and len(self._names) < 10 ** 7:
            self._names[k] = short          # guards cycles
            name = self.type_name(declaring) + "." + short
        else:
            name = short
        self._names[k] = name
        return name

    def type(self, name):
        """The class called `name` (full name, e.g. "app.DamageController"). Raises ModError when unknown."""
        if self._by_name is None:
            self._by_name = {}
            for k in range(1, self.n_types):
                self._by_name.setdefault(self.type_name(k), k)
        k = self._by_name.get(name)
        if k is None:
            raise ModError("no class named %r" % name)
        return Type(self, k)

    def find_types(self, pattern):
        """Classes whose full name matches the regular expression `pattern`."""
        self.type("System.Object")
        rx = _re.compile(pattern)
        return [Type(self, k) for n, k in self._by_name.items() if rx.search(n)]

    def find_fields(self, pattern):
        """[(Type, Field)] for every field whose name matches the regular expression `pattern`."""
        self._build_fields()
        rx = _re.compile(pattern)
        return [(Type(self, k), f) for k, fl in self._fields.items() for f in fl if rx.search(f.name)]

    # ---- fields -------------------------------------------------------------------------------------------------
    def _build_fields(self):
        if self._fields is not None:
            return
        self._fields = {}
        data, impl = self.tables["fields"], self.tables["field_impl"]
        fsz, isz = self.L["field_size"], self.L["field_impl_size"]
        for j in range(self.n_fields):
            (v,) = struct.unpack_from("<Q", data, j * fsz)
            declaring, fid, offset = v & 0x3FFFF, (v >> 18) & 0x3FFFF, v >> 38
            if fid >= self.n_field_impl:
                continue
            a, b, name_off = struct.unpack_from("<3I", impl, fid * isz)
            attributes = (a >> 16) & 0xFFFF
            off = offset if attributes & 0x10 else offset + OBJECT_HEADER
            self._fields.setdefault(declaring, []).append(Field(self, self._string(name_off), off, b & 0x3FFFF, attributes))

    def _fields_of(self, k):
        self._build_fields()
        return self._fields.get(k, [])

    def _subclass_indices(self, k):
        if self._children is None:
            self._children = {}
            for i in range(1, self.n_types):
                p = self._bits(i, 18)
                if p and p != i:
                    self._children.setdefault(p, []).append(i)
        out, todo = [], [k]
        while todo:
            for c in self._children.get(todo.pop(), []):
                out.append(c)
                todo.append(c)
        return out

    # ---- live objects -------------------------------------------------------------------------------------------
    def instances(self, cls, subclasses=True, limit=256):
        """Addresses of live objects of class `cls` (a name or Type), including subclasses unless told otherwise."""
        t = self.type(cls) if isinstance(cls, str) else cls
        types = [t] + (t.subclasses() if subclasses else [])
        infos = {x.object_info: x for x in types if x.object_info}
        found = self.game.find_values(list(infos), limit=limit) if infos else {}
        objects = []
        for info, (_, addrs) in found.items():
            objects += addrs
        return objects

    def type_of(self, obj):
        """The Type of the managed object at `obj`, or None."""
        info = self.game.read_ptr(obj)
        entry = self.game.read_ptr(info) if info else None
        if not entry:
            return None
        k, rem = divmod(entry - self.types_ptr, self.L["type_size"])
        return Type(self, k) if rem == 0 and 0 < k < self.n_types else None

    def read_string(self, ptr, max_chars=512):
        """A System.String object as text (None when ptr is null or not a string)."""
        if not ptr:
            return None
        length = self.game.read_i32(ptr + OBJECT_HEADER)
        if length is None or not 0 <= length <= max_chars:
            return None
        if length == 0:
            return ""
        data = self.game.read_bytes(ptr + OBJECT_HEADER + 4, length * 2)
        return data.decode("utf-16-le", "replace") if data else None

    def object_name(self, obj, search=0x80):
        """The name of a native engine object such as a via.GameObject ("em1000", "pl1000"): the first System.String
        it points to within its first `search` bytes. The offset found is remembered for later calls."""
        if not obj:
            return None
        string_info = getattr(self, "_string_info", None)
        if string_info is None:
            string_info = self._string_info = self.type("System.String").object_info
        offset = getattr(self, "_name_offset", None)
        if offset is not None:
            ptr = self.game.read_ptr(obj + offset)
            if ptr and self.game.read_ptr(ptr) == string_info:
                return self.read_string(ptr)
        blob = self.game.read_bytes(obj, search) or b""
        for off in range(0, len(blob) - 7, 8):
            (ptr,) = struct.unpack_from("<Q", blob, off)
            if 0x10000 <= ptr < 0x7FFFFFFFFFFF and self.game.read_ptr(ptr) == string_info:
                self._name_offset = off
                return self.read_string(ptr)
        return None

    def describe(self, cls):
        """Multi-line listing of a class: parents and every field with offset and type (for exploring)."""
        t = self.type(cls) if isinstance(cls, str) else cls
        lines, chain = [], []
        p = t
        while p:
            chain.append(p.name)
            p = p.parent
        lines.append("%s  (size %#x)  : %s" % (t.name, t.size, " -> ".join(chain[1:])))
        for f in sorted(t.all_fields(), key=lambda f: (f.is_static, f.offset)):
            lines.append("  %s+%#05x  %-40s %s" % ("static " if f.is_static else "", f.offset, f.name, f.type_name))
        return "\n".join(lines)
