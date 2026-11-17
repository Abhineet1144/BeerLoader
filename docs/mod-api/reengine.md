# RE Engine reflection (`reengine` helper library)

Capcom's RE Engine keeps a **type database** (TDB) in every game's memory. It lists each managed class: namespace,
parent, size and every field with its name, type and offset. Every live managed object starts with a pointer to its
class's runtime info. With both, a mod can:

- look up a class and its fields by name;
- find every live object of a class in one memory pass;
- ask any object for its class.

It needs no addresses, so it keeps working after game updates.

This is an optional helper library, not part of the core SDK. It lives in `<Beer>/mods/libs/reengine.py`, and the
manager puts `mods/libs` on `PYTHONPATH` for every mod. Only mods for RE Engine games import it. The SDK and the loader
contain nothing engine-specific; the library is built only on generic SDK calls (`find_values`, `read_bytes`,
`read_ptr`).

```python
from beer_mod import Mod
from reengine import REEngine

mod = Mod()
re = REEngine(mod.game)                      # finds and reads the TDB (about 1 s; cached on disk afterwards)
damage = re.type("app.DamageController")
print(re.describe(damage))                   # parents and every field with offset and type

INFO = damage.field("HealthInfo").offset     # 0x48 in RE8, but never hard-code it
HP = re.type("app.HealthInfo").field("Health").offset
for obj in re.instances(damage):             # every live DamageController (and subclasses)
    info = mod.game.read_ptr(obj + INFO)
    print(mod.game.read_f32(info + HP))
```

## Supported games

Each TDB version has its own binary layout. Supported now: **version 69, Resident Evil Village**. Any other version
raises `ModError("RE Engine TDB version N is not supported yet")`. Adding a version means adding its layout to
`LAYOUTS` in `mods/libs/reengine.py`.

## Reference

| Member | Purpose |
|---|---|
| `REEngine(game, progress=None)` | Finds the TDB in the executable and loads its tables. `progress(text)` is called while it reads (first run only). |
| `re.version`, `re.tdb` | TDB version and address |
| `re.type(name)` | `Type` for a full class name such as `"app.DamageController"` or `"via.GameObject"`. Nested classes use dots: `"app.HitController.DamageInfo"`. |
| `re.find_types(regex)` | Every `Type` whose full name matches |
| `re.find_fields(regex)` | `[(Type, Field)]` for every field whose name matches. Use it to explore, e.g. `re.find_fields("Health")`. |
| `re.instances(cls, subclasses=True, limit=256)` | Addresses of live objects of a class. One memory pass, about 0.3 s, and it does not disturb the Memory window's scan. |
| `re.type_of(obj)` | `Type` of the managed object at `obj`, or `None` |
| `re.read_string(ptr)` | Text of a `System.String` object |
| `re.object_name(obj)` | Name of a native engine object such as a `via.GameObject` (`"em1000"`) |
| `re.describe(cls)` | Multi-line listing: parent chain, then every field (own and inherited) with offset and type |

**Type**

| Member | Purpose |
|---|---|
| `name`, `size`, `parent` | Full name, instance size, parent `Type` (or `None`) |
| `fields` | This class's own fields |
| `field(name)` | A field of this class or any parent. Properties' backing fields are found by the property name (`"owner"` finds `<owner>k__BackingField`). |
| `all_fields()` | Own and inherited fields |
| `subclasses()` | Every class deriving from this one |
| `object_info` | The pointer every live instance of exactly this class holds at offset 0 |

**Field**

| Member | Purpose |
|---|---|
| `name`, `offset`, `type_name`, `is_static` | Offsets are from the start of the object, so read `obj + field.offset`. |

## Finding what to read

1. Look for likely names: `re.find_fields("(?i)health|hitpoint")` or `re.find_types("Enemy")`.
2. List a candidate with `print(re.describe("app.SomeClass"))`.
3. Check live values: `re.instances("app.SomeClass")`, then `game.read_f32(obj + offset)`.

Fields of class types hold pointers to other managed objects. Follow them with `game.read_ptr(obj + offset)`.

## Notes

- The tables are cached under `~/.beer/cache/reengine/`, keyed by the TDB header. The runtime part (`object_info`) is
  re-read at every start, because it moves between runs.
- Managed objects start with a 16-byte header. Field offsets already include it.
