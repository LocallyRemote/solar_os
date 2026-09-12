+++
id = "lua.messaging"
title = "Lua contacts and messages API"
section = "api"
summary = "Contacts and messages: contacts, messages"
keywords = "lua solaros api messaging contacts messages"
packages_any = ["app_lua"]
agent_reference_sections = true
+++
# Lua contacts and messages API

[API overview](lua.md) · [Python contacts and messages](python.messaging.md)

## `solaros.contacts`

- `solaros.contacts`: `list`, `get` when provider-neutral messaging is compiled

## `solaros.messages`

- `solaros.messages`: `conversations`, `list`, `send`, `mark_read`, `cancel` when provider-neutral messaging is compiled

## Sending messages

`solaros.messages.send(conversation_id, body[, allow_untrusted])` queues a
message and returns its stable hexadecimal ID. `list()` also represents message
IDs as hexadecimal strings, and `cancel(id)` accepts that representation.
Blocked direct endpoints are always rejected; discovered endpoints require the
optional boolean for that one send. `solaros.contacts` returns only contact
summaries and endpoint IDs, never credentials or endpoint secret material.

## Quick reference

Use `solaros.contacts`, `solaros.messages` for contacts and messages.
See `man lua` for runtime conventions and service availability.
