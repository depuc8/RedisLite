# RedisLite

A from-scratch Redis-like in-memory key-value server written in C++17, built with no external dependencies. Implements the core networking model, data structures, and TTL system found in real Redis.

---

## Features

- Non-blocking, single-threaded event loop using `poll()`
- Custom binary wire protocol with request pipelining
- String key-value store with `GET`, `SET`, `DEL`
- Sorted sets (`ZADD`, `ZREM`, `ZSCORE`, `ZQUERY`)
- Per-key TTL expiry (`PEXPIRE`, `PTTL`)
- Idle connection timeout (5 seconds)
- Progressive rehashing hashtable (no stop-the-world rehash)
- Self-balancing AVL tree with rank-based navigation
- Background thread pool for async cleanup of large data structures

---

## Requirements

- Linux
- `g++` with C++17 support (`g++ >= 7`)
- `make`
- `pthread` (usually included with glibc)

---

## Build

```bash
make
```

The compiled binary is placed at `bin/redislite`.

### Other targets

```bash
make run      # build and run the server
make clean    # remove build artifacts
```

---

## Running

```bash
./bin/redislite
```

The server listens on **`0.0.0.0:1234`** by default. You will see connection logs on stderr:

```
new client from 127.0.0.1:54321
removing idle connection: 5
```

---

## Protocol

RedisLite uses a **custom binary protocol** (not RESP).

### Request format

```
+--------+-------+--------+-------+--------+
| nstr   | len   | str1   | len   | str2   |  ...
| 4 bytes| 4bytes| n bytes| 4bytes| n bytes|
+--------+-------+--------+-------+--------+
```

Each request is prefixed with a **4-byte little-endian message length**, followed by the number of strings (`nstr`) and then each string as a `(length, data)` pair.

### Response types

Every response begins with a 1-byte type tag:

| Tag | Value | Description |
|-----|-------|-------------|
| `TAG_NIL` | `0` | Null / not found |
| `TAG_ERR` | `1` | Error — `uint32 code` + `uint32 len` + message |
| `TAG_STR` | `2` | String — `uint32 len` + bytes |
| `TAG_INT` | `3` | 64-bit signed integer |
| `TAG_DBL` | `4` | 64-bit double |
| `TAG_ARR` | `5` | Array — `uint32 count` + N elements |

### Pipelining

The protocol supports **request pipelining** — multiple requests can be sent back-to-back without waiting for individual responses. The server processes them in order and flushes all responses together.

---

## Commands

### String operations

| Command | Syntax | Returns | Description |
|---------|--------|---------|-------------|
| `get` | `get <key>` | `STR` or `NIL` | Retrieve a string value |
| `set` | `set <key> <value>` | `NIL` | Set a string value |
| `del` | `del <key>` | `INT` (1/0) | Delete a key |
| `keys` | `keys` | `ARR[STR]` | List all keys in the store |

### TTL operations

| Command | Syntax | Returns | Description |
|---------|--------|---------|-------------|
| `pexpire` | `pexpire <key> <ttl_ms>` | `INT` (1/0) | Set a TTL in milliseconds |
| `pttl` | `pttl <key>` | `INT` | Remaining TTL in ms; `-1` = no TTL; `-2` = not found |

### Sorted set operations

| Command | Syntax | Returns | Description |
|---------|--------|---------|-------------|
| `zadd` | `zadd <zset> <score> <name>` | `INT` (1=added, 0=updated) | Add or update a member |
| `zrem` | `zrem <zset> <name>` | `INT` (1/0) | Remove a member |
| `zscore` | `zscore <zset> <name>` | `DBL` or `NIL` | Get a member's score |
| `zquery` | `zquery <zset> <score> <name> <offset> <limit>` | `ARR[STR,DBL]` | Range query: find first entry ≥ `(score, name)`, skip `offset`, return up to `limit` pairs |

---

## Architecture

### Event loop

The server runs a single-threaded, non-blocking event loop. All socket file descriptors are set to `O_NONBLOCK`. The loop uses `poll()` to wait for I/O readiness across all connections simultaneously.

```
main()
 └── poll() loop
      ├── handle_accept()    — new TCP connection
      ├── handle_read()      — parse requests, generate responses
      ├── handle_write()     — flush outgoing buffer
      └── process_timers()   — expire idle connections & TTL keys
```

Each `Conn` struct holds:
- `incoming` / `outgoing` byte buffers
- Intent flags: `want_read`, `want_write`, `want_close`
- A timer node for idle-connection tracking

### Timer system

Two independent timer mechanisms run in the same event loop thread:

| Timer | Structure | Purpose |
|-------|-----------|---------|
| Idle connection | Doubly-linked list (`DList`) | Close connections inactive for **5 seconds** |
| Key TTL | Min-heap (`HeapItem`) | Delete keys at their expiry timestamp |

`next_timer_ms()` derives the correct `poll()` timeout from both, so the loop wakes up exactly when needed.

### Thread pool

A pool of **4 worker threads** is used exclusively for **asynchronous deletion** of large ZSets (> 1000 members). This prevents the event loop from stalling on expensive `free()` calls. Small deletions happen inline.

---

## Data Structures

### Hashtable (`hashtable.h/cpp`)

A two-table hashmap that performs **progressive rehashing**:
- When the load factor exceeds 8×, a new larger table is created.
- On every operation, up to **128 keys** are migrated from `older` → `newer`.
- This spreads rehashing cost across requests, avoiding any pause.
- Collision resolution: separate chaining (linked list per bucket).
- Hash function: FNV-1a (`str_hash` in `common.h`).

### AVL Tree (`avl.h/cpp`)

A self-balancing binary search tree used as the **sorted index** inside ZSets:
- Each node stores `height` and `cnt` (subtree size).
- `avl_fix()` restores balance after insert/delete via rotations.
- `avl_offset(node, n)` navigates to the node at rank `± n` in **O(log N)** — used for ZSet range queries.

### Sorted Set / ZSet (`zset.h/cpp`)

Mirrors Redis's `ZSET` type. Each ZNode is **dual-indexed**:
- `HMap` (hashtable) — O(1) lookup by name.
- `AVLNode` (AVL tree) — O(log N) sorted iteration by `(score, name)`.

`zset_seekge()` finds the first entry ≥ a given `(score, name)` for range queries.

### Min-Heap (`heap.h/cpp`)

A standard binary min-heap storing `(expiry_ms, &heap_idx)` pairs:
- The root is always the soonest-expiring key.
- Each `HeapItem` holds a back-pointer (`ref`) to the owning Entry's `heap_idx` field, so the Entry always knows its position in the heap as items are swapped during rebalancing.

### Doubly-Linked List (`list.h`)

An intrusive circular `DList` embedded in `Conn` structs:
- Connections are ordered by last-active time.
- On activity, a connection is detached and re-inserted at the tail — O(1).
- The head is always the oldest (soonest to time out).

---

## File Structure

```
RedisLite/
├── src/
│   ├── server.cpp          # Event loop, command handlers, protocol, timers
│   ├── hashtable.cpp       # Progressive-rehashing hashtable
│   ├── avl.cpp             # AVL tree (rotations, rebalancing, rank navigation)
│   ├── zset.cpp            # Sorted set (AVL tree + hashtable)
│   ├── heap.cpp            # Min-heap for TTL expiry
│   └── thread_pool.cpp     # Worker thread pool for async cleanup
├── include/
│   ├── common.h            # container_of macro, FNV hash function
│   ├── hashtable.h         # HNode, HTab, HMap structs + API
│   ├── avl.h               # AVLNode struct + API
│   ├── zset.h              # ZSet, ZNode structs + API
│   ├── heap.h              # HeapItem struct + API
│   ├── list.h              # DList struct + inlined helpers
│   └── thread_pool.h       # TheadPool struct + API
├── bin/
│   └── redislite           # Compiled binary (after build)
└── Makefile
```

---

## Error Codes

| Code | Name | Meaning |
|------|------|---------|
| `1` | `ERR_UNKNOWN` | Unknown command |
| `2` | `ERR_TOO_BIG` | Response exceeds 32 MB limit |
| `3` | `ERR_BAD_TYP` | Wrong value type for command (e.g. `get` on a ZSet) |
| `4` | `ERR_BAD_ARG` | Bad argument (e.g. non-numeric TTL) |
