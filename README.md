# RedisLite

A Redis-like in-memory key-value server written in C++17 with no external dependencies. Implements a non-blocking event loop, custom binary protocol, TTL expiry, and sorted sets — all from scratch.

---

## Build & Run

```bash
make              # build → bin/redislite
make run          # build and start the server (port 1234)
make clean        # remove build artifacts
```

---

## Testing

> The test driver (`tests/test_client.cpp`) is AI-generated. Treat it as an integration smoke-test.

Start the server in one terminal, then in another:

```bash
make test         # builds and runs bin/test_client against localhost:1234
```

---

## Commands

### Strings
| Command | Returns | Description |
|---------|---------|-------------|
| `get <key>` | `STR` / `NIL` | Get a value |
| `set <key> <value>` | `NIL` | Set a value |
| `del <key>` | `INT` 1/0 | Delete a key |
| `keys` | `ARR[STR]` | List all keys |

### TTL
| Command | Returns | Description |
|---------|---------|-------------|
| `pexpire <key> <ms>` | `INT` 1/0 | Set TTL in milliseconds |
| `pttl <key>` | `INT` | Remaining ms; `-1` = no TTL; `-2` = not found |

### Sorted Sets
| Command | Returns | Description |
|---------|---------|-------------|
| `zadd <zset> <score> <name>` | `INT` 1/0 | Add or update a member |
| `zrem <zset> <name>` | `INT` 1/0 | Remove a member |
| `zscore <zset> <name>` | `DBL` / `NIL` | Get a member's score |
| `zquery <zset> <score> <name> <offset> <limit>` | `ARR` | Range query — first entry ≥ `(score, name)`, skip `offset`, up to `limit` pairs |

---

## Protocol

Custom binary format (not RESP). Each request is a 4-byte length header followed by a packed array of strings. Responses are tagged with a 1-byte type: `NIL`, `ERR`, `STR`, `INT`, `DBL`, or `ARR`. Pipelining is supported.

---

## Architecture

- **Event loop** — single-threaded, `poll()`-based, all sockets non-blocking
- **Hashtable** — two-table progressive rehashing (no stop-the-world pause)
- **Sorted set** — dual-indexed: AVL tree for sorted range queries + hashtable for O(1) name lookup
- **TTL** — min-heap of expiry timestamps, checked every loop iteration
- **Idle timeouts** — intrusive doubly-linked list, connections closed after 5s of inactivity
- **Thread pool** — 4 workers used only for async deletion of large ZSets (>1000 members)

---
