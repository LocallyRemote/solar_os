+++
id = "python.storage"
title = "Python storage and files API"
section = "api"
summary = "Storage and files: storage"
keywords = "python solaros api storage storage"
packages_any = ["app_python"]
agent_reference_sections = true
+++
# Python storage and files API

[API overview](python.md) · [Lua storage and files](lua.storage.md)

## Files and imports

`open(path[, mode])` opens a file through SolarOS storage. Text mode is the
default; add `b` for bytes. The supported base modes are `r`, `w`, `a`, and
exclusive-create `x`, and each can use `+` for updating. File objects support
`read`, `readinto`, `readline`, `readlines`, `write`, `seek`, `tell`, `flush`,
and `close`, and can be used as context managers.

```python
with open("/notes/example.txt", "w") as output:
    output.write("hello from Python\n")

with open("/notes/example.txt") as source:
    print(source.read())
```

Paths use the same preferred-storage and explicit-mount rules as
`solaros.storage`. They cannot bypass SolarOS mounts to reach an unrelated
host filesystem. Long reads and writes remain responsive to app cancellation.

External imports support `.py`, `.mpy`, and package directories containing
`__init__.py` or `__init__.mpy`. A file-run starts its search in the script's
directory and then searches the other entries in `sys.path`. The REPL and
source-runner start relative imports at the preferred storage root.

```text
/apps/weather/main.py
/apps/weather/sensors.py
/apps/weather/display/__init__.py
```

From `main.py`, both `import sensors` and `import display` resolve beside the
script. Imported files pass through the same SolarOS path resolver as
`open()`.

```python
print(solaros.storage.resolve("/.shell/history"))
```

## `solaros.storage`

Storage functions expose SD mount and filesystem service operations.

- `status()`: return a human-readable status string for the preferred mounted
  persistent storage (SD when mounted, otherwise internal flash).
- `is_mounted()`: return whether the default storage volume is mounted.
- `mount()`: mount the default storage volume.
- `unmount()`: unmount the default storage volume.
- `mount_point()`: return the preferred mounted persistent-storage path (SD
  when mounted, otherwise internal flash).
- `usage([path])`: return disk usage for the default volume or the volume containing `path`.
- `resolve(path)`: return the internal resolved path.
- `read_file(path[, max_bytes])`: return up to `max_bytes` bytes from a regular
  file. The default is 4096 and the maximum is 65536.
- `rescan()`: rescan SD block devices and partitions.
- `blocks()`: return a list of block device and partition dictionaries.
- `block_count()`: return the number of known blocks.
- `block(index)`: return one block dictionary.
- `usage_for_block(index)`: return usage for one mounted block.
- `mkdir(path)`: create a directory.
- `rmdir(path)`: remove an empty directory.
- `remove(path)`: remove a file.
- `rename(old_path, new_path)`: rename or move a file or directory.
- `copy(source_path, dest_path)`: copy a file.
- `mount_volume(name[, mount_point])`: mount a named block or partition.
- `unmount_volume(target)`: unmount by volume name or mount point.

Example:

```python
import solaros

if not solaros.storage.is_mounted():
    solaros.storage.mount()

print(solaros.storage.usage("/"))
print(solaros.storage.read_file("/notes/example.txt", 512))
for block in solaros.storage.blocks():
    print(block["name"], block["type"], block["mounted"], block["mount_point"])
```

## Quick reference

Use `solaros.storage` for storage and files.
See `man python` for runtime conventions and service availability.
