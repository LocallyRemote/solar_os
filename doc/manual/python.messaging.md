+++
id = "python.messaging"
title = "Python contacts and messages API"
section = "api"
summary = "Contacts and messages: contacts, messages"
keywords = "python solaros api messaging contacts messages"
packages_any = ["app_python"]
agent_reference_sections = true
+++
# Python contacts and messages API

[API overview](python.md) · [Lua contacts and messages](lua.messaging.md)

## `solaros.contacts` and `solaros.messages`

Provider-neutral messaging builds expose:

- `solaros.contacts.list()`: bounded contact summaries.
- `solaros.contacts.get(contact_id)`: one contact with endpoint IDs, or `None`.
- `solaros.messages.conversations()`: bounded conversation summaries.
- `solaros.messages.list(conversation_id)`: retained messages.
- `solaros.messages.send(conversation_id, body, allow_untrusted=False)`: queue
  a message and return its stable hexadecimal ID.
- `solaros.messages.mark_read(conversation_id)`: mark linked message state read.
- `solaros.messages.cancel(message_id)`: cancel a queued message using the
  hexadecimal string returned by `send()` or `list()`.

Scripts cannot read credentials or endpoint secret material. Blocked direct
endpoints are rejected, and discovered endpoints require
`allow_untrusted=True` for that one send.

## Quick reference

Use `solaros.contacts`, `solaros.messages` for contacts and messages.
See `man python` for runtime conventions and service availability.
