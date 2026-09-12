+++
id = "lua.storage"
title = "Lua storage and files API"
section = "api"
summary = "Storage and files: storage"
keywords = "lua solaros api storage storage"
packages_any = ["app_lua"]
agent_reference_sections = true
+++
# Lua storage and files API

[API overview](lua.md) · [Python storage and files](python.storage.md)

## `solaros.storage`

- `solaros.storage`: `status`, `is_mounted`, `mount`, `unmount`, `mount_point`, `usage`, `resolve`, `read_file`, `rescan`, `blocks`, `block_count`, `block`, `usage_for_block`, `mkdir`, `rmdir`, `remove`, `rename`, `copy`, `mount_volume`, `unmount_volume`

## Quick reference

Use `solaros.storage` for storage and files.
See `man lua` for runtime conventions and service availability.
