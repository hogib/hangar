# Dawn Patrol: the console module

Module identifier `console`, program **Dawn Patrol**, binary `dawn-patrold`
([docs/names.md](../../docs/names.md)). The monitoring half of docs/11's
`pilot_console`, as `lib/hg/PLAN.md` "Stage 3 addition: the web console" describes
it: one web page showing every process the manager keeps alive, every topic, the
recent log, with start, stop and restart buttons. It shows what `hangar top`,
`hangar topics` and `hangar echo` show ([docs/12-tools.md](../../docs/12-tools.md)),
in a browser, over a slow SSH port forward. The pilot's controls (arm, mode,
promotion) come later, when stage 2b and the safety module exist.

GPL-3.0-or-later, C11 (`-std=c11 -Wall -Wextra -pedantic`, clean under gcc, clang
and `zig cc`), libc, pthreads, `lib/hg` and `lib/net` only; one static binary for
musl like the manager. A module like any other: `hg_init`, a manifest, the module
page for ready, alive and its status line.

## Layout

| File | |
|---|---|
| `dawn_patrold.c` | the process: arguments, the role, the two subscriptions and `hg_wait_fd`, the listening socket, the epoll loop, HTTP connections over `lib/net/http.c`'s parser, routing, the control-socket POSTs |
| `console_api.c` | what the manager's topics, the topic directory and the fallback log say, as JSON; the module table with the console's own restart count; the measured rates |
| `console.h` | shared between the two |
| `web/index.html` | the page: HTML, CSS and a plain script, one file, nothing external. Embedded in the binary at build time |
| `console.module` | the manifest template (`/usr/share/hangar/modules/console.module`) |
| `console.mk` | the make fragment: the embed step, the binary, `check-console`; included by the root Makefile |
| `tests/test_console.c` | the suite |

## What the page shows

A dark page, one column, three sections under a sticky header, polling the API once
a second (the log every two) and saying `live` or `dawn-patrold unreachable` in the
header.

- **The header**: the system state as a coloured pill (`nominal` green, `degraded`
  amber, `failsafe` red, `no manager` grey when `hangar.<role>.system` has no
  message), how long it has been in it, the reason the manager gave, the role, the
  manager's pid and module count; a `Reload manifests` button (`hangar reload`).
- **Modules**: one row per module from `hangar.<role>.modules`: name, state as a
  pill (ready green; starting, backoff and stopping amber; unhealthy, failed and
  refused red; stopped grey), pid, restarts seen by the console, failures as the
  manager counts them, last exit (`exit:0`, `signal:9`, `-`), time in the state, the
  status line (`hg_status`), and `start`, `stop`, `restart` buttons (start disabled
  while running, stop while not; stop, restart and reload ask once). The outcome of
  a button, `ok` or the manager's refusal verbatim, shows under the header.
- **Topics**: every file of the topic directory: name, type hash, declared rate
  (`event` for 0), rate measured by the console, age of the newest message, writer
  and its pid, slots, payload bytes, write sequence, flags (`closed`, `died`,
  `replaced`, `stale`; `live` when there are none and a message exists). Click a
  header to sort by it (name by default; a second click reverses). Click a row and
  the panel beside the table shows the newest message decoded by the schema in the
  topic's header: writer, size, sequence, age, the stale-inputs flag, then one row
  per field with its value, type and the `.msg` comment (where the units are),
  refreshed every second while selected.
- **Log**: a selector over the manager's fallback log files
  (`/run/hangar/log/<module>.log`, written when there is no journald) and the last
  200 lines of the chosen one, following the end while you leave it scrolled there.
  With journald present the pane says where the lines are (`journalctl -t <module>`).

## The API

Every reply is JSON (`application/json`), `Connection: close`, `Cache-Control:
no-store`. Times are from the manager's and the topics' `CLOCK_MONOTONIC` values,
converted to ages by the console on the same machine.

| Request | Reply |
|---|---|
| `GET /` | the page, `text/html` |
| `GET /api/system` | `{"role","topic","present"}` and, when a message exists, `state` (`nominal`, `degraded`, `failsafe`), `since_ns`, `in_state_s`, `modules`, `manager_pid`, `age_ms` (of the newest message), `seq`, `reason` |
| `GET /api/modules` | `{"role","topic","modules":[...]}`: per module `name`, `state` (`starting`, `ready`, `unhealthy`, `backoff`, `failed`, `stopped`, `refused`, `stopping`), `state_code`, `pid` (0 when not running), `failures` (the manager's count since the last `ResetAfter`), `restarts` (pid changes the console saw since it started), `last_exit` (`exit:<n>`, `signal:<n>`, `-`), `since_ns`, `in_state_s`, `age_ms`, `status` |
| `GET /api/topics` | `{"dir","topics":[...]}`: per file `name`, `type_hash` (16 hex), `topic_id`, `rate_hz` (declared, 0 for an event topic), `measured_hz` (`null` until two ticks have seen it), `age_ms` (`null` before the first message), `stale_after_ms`, `writer`, `writer_pid`, `slots`, `payload_bytes`, `schema_bytes`, `seq`, `first_seq`, `flags` (an array of `closed`, `died`, `replaced`, `stale`) |
| `GET /api/topic/<name>` | the newest message: `topic`, `type_hash`, `writer`, `payload_bytes`, `present`; with a message `seq`, `stamp_ns`, `publish_ns`, `age_ms`, `stale` (computed from stale inputs), `gap`, and `fields`, an array of `{"name","type","count","comment","value"}` in schema order. `char[N]` is a string; an array of up to 64 elements is an array; a longer one (a frame's pixels) is its size as a string, `"u8[1382400]: 1382400 bytes"`; a float that is not finite is `null`. Without a message, `result` says `no message yet`, `closed` or `absent`. 404 for a name that is not a topic or has no file |
| `GET /api/log[?lines=N]` | `{"journald","dir","note","files":[{"name","tail","bytes"}]}`: the last N lines (default 100, at most 1000, within the last 16 KB) of each `<dir>/<module>.log`; `journald` true and no files when the journald socket is there |
| `POST /api/module/<name>/start`, `/stop`, `/restart` | `start <name>` etc. on the control socket. `{"ok":true,"request","reply"}` (200); `{"ok":false,"request","error"}` with the manager's reason: 403 for `permission denied: hangar-admin required for restart`, 409 for any other refusal (`no module nobody`), 502 when the manager cannot be reached |
| `POST /api/reload` | `reload` on the control socket, the same replies |

Anything else is 404 `{"error":"no such page"}`; a method other than GET or POST,
or a POST to a GET path, is 405. A malformed request is 400, a head over 8 KB 431,
a body over 4 KB 413, from `lib/net/http.c`'s parser (fuzzed, `make fuzz-http`).
A POST must carry `Content-Type: application/json` (403 otherwise): a page on
another site cannot send that header to this server without a preflight it never
grants, so a form elsewhere cannot restart a module through the operator's browser.
The page's own `fetch` sends it.

**Who may press the buttons.** The POSTs reach the manager as this process's
user, which the manager identifies by `SO_PEERCRED` and `SO_PEERGROUPS`
([docs/03-manager.md](../../docs/03-manager.md#control-socket)). Monitoring needs
only the `hangar` group every module has; the buttons need `hangar-admin`, so the
manifest template gives the module `SupplementaryGroups=hangar-admin`. Without it
the manager refuses, the page shows `refused: permission denied: hangar-admin
required for restart`, and nothing else changes. The console never escalates: the
browser's user is nobody to the manager, and the page is bound to `127.0.0.1` by
default; expose it only through an SSH port forward or a host you trust.

## Running it

Under the manager: install `dawn-patrold` as `/usr/lib/hangar/dawn-patrold`,
symlink `console.module` into `/etc/hangar/modules/`, `hangar reload`. The
template names the vehicle's topics; a topside copy says
`[Subscribe hangar.topside.system]` and `.modules`. The role the binary serves
comes from that wiring (`sub hangar.<role>.system`), so the copy is the only
edit. Open `http://127.0.0.1:8080/`, or from the topside
`ssh -L 8080:127.0.0.1:8080 vehicle` and the same address.

By hand, as the manager's user (which counts as `hangar-admin` for a manager run
by hand: `hangard/README.md`, deviation 5), against a manager under `$HANGAR_ROOT`
as `hangard/README.md` "Running it standalone" sets one up:

```sh
HANGAR_ROOT=$R builddir/hangard &
HANGAR_ROOT=$R builddir/dawn-patrold --listen 127.0.0.1:8080      # role from $R/etc/hangar/hangard.conf
HANGAR_ROOT=$R builddir/dawn-patrold --listen 0.0.0.0:8080 --role topside
HANGAR_ROOT=$R builddir/dawn-patrold --listen 127.0.0.1:0         # a free port; the log line says which
curl -s http://127.0.0.1:8080/api/modules
curl -s -X POST -H 'Content-Type: application/json' http://127.0.0.1:8080/api/module/talker/restart
```

`--listen` takes `host:port`, `[v6]:port` or `port` (every interface); the
default is `127.0.0.1:8080`. `$HANGAR_CONTROL` and `$HANGAR_TOPIC_DIR` override the
paths under `$HANGAR_ROOT`, as for the `hangar` command. Unwired (no manager
started it), the role is `--role`, else `Role` from `hangard.conf`, else `vehicle`.
`SIGTERM`, `SIGINT` and `SIGHUP` stop it. Its own output: one line at start with
the address, role, topic directory and control socket, one at stop, and a line for
each subscription or listen failure; nothing per request.

## How it is built

- **One thread, one epoll loop.** The listening socket, every client socket, the
  eventfd `hg_wait_fd()` returns for the two manager topics (the library's helper
  thread is the only other thread), a 1 s `timerfd` and a `signalfd`. Each tick
  drains the topics, scans the topic directory and measures every topic's rate from
  its `write_seq` delta since the last tick, drops modules the manager stopped
  describing after 5 s, calls `hg_alive()`, and closes a client that has had 10 s
  without finishing its request or taking its answer. At most 32 clients at once;
  the 33rd gets 503.
- **`lib/net/http.c` as the parser, not the server.** electric-eye's server runs a
  thread per connection with blocking reads; the module wanted the topics' eventfd
  and the sockets in one loop. So `http_parse_request()` (the fuzzed part) parses
  each request from the connection's buffer, and the sockets, the body wait, the
  response head (the same format as `http_respond`: `Content-Length`,
  `Connection: close`, `Cache-Control: no-store`, a charset for text) and the
  non-blocking writes are a ~150-line adapter in `dawn_patrold.c`. Hellion drove
  the server directly (`http_start`); nothing in `lib/net` was changed.
- **Topics by header, messages by the library.** `/api/topics` maps each file's
  first 4 KB and reads the header words by offset, as `hangar topics` does (the
  publish line is read as words: a torn counter costs one odd sample).
  `/api/topic/<name>` opens an `hg_sub` for the request, reads the newest message
  with `hg_latest`, parses the header's schema with `hg_schema_parse`, and closes
  it: the seqlock makes the copy consistent. The two manager topics are typed
  subscriptions (`HANGAR_SYSTEM_TYPE`, `HANGAR_MODULES_TYPE`), the modules one
  opened from the oldest slot so a full round is in hand at start.
- **The page inside the binary.** `console.mk` runs `legacy/hellion/embed.c`
  (built with `$(HOSTCC)`, so a cross build still runs it here) over
  `web/index.html` into `$(OBJDIR)/gen/console_page.c`, a `const unsigned char
  console_page_html[]` and its length, linked in. `GET /` serves it from memory.
  Nothing is installed beside the binary.
- **Static for musl.** `make BUILDDIR=... CC="zig cc -target aarch64-linux-musl
  -mcpu=cortex_a72" HOSTCC="zig cc" LDFLAGS=-static builddir/dawn-patrold` gives a
  2.3 MB static aarch64 binary (`getaddrinfo` with numeric hosts only, so no
  resolver is pulled in at run time). `tools/release.sh` ships it as
  `bin/dawn-patrold`.

## Tests

`make check-console` (part of `check`), under 30 s, as an ordinary user under a
`$HANGAR_ROOT` of its own on tmpfs (`hangard/tests/hd_test.h`'s harness, reused as
is): a manager with the toy `talker` (`test.depth` at 50 Hz) and the console as a
module, on a port the suite picks. It checks that the console is `ready` through
the page with its status line naming the address; `/api/modules` names the talker
and the console as ready with the talker's pid and status line; `/api/system` is
nominal; `/api/topics` lists `test.depth` with its declared rate, slots, payload,
writer, the header's type hash and, within a few ticks, a measured rate near 50 Hz;
`/api/topic/test.depth` decodes the three fields with their comments and values,
and `hangar.vehicle.system` decodes too; `/` is the page, self-contained; wrong
paths are 404, wrong methods 405; malformed or oversized requests (garbage, a
chunked body, a bare LF, a NUL, an oversized body or head) get 400, 413 or 431, a
client that hangs up gets nothing, and the module stays ready through them; a
POST without the JSON content type is 403; a restart through the console gives the
talker a new pid, `failures` stays 0 and the console's `restarts` reads 1; a
restart of a module that does not exist is 409 with the manager's reason, a bad
name or verb 404; reload is ok; stop shows `stopped`, pid 0 and `exit:0`, start
brings it back with `restarts` 2; `/api/log` serves the fallback files with the
talker's goodbye line and the console's own start line; the manager's shutdown
takes the console with it.

## Deviations from the docs, with reasons

1. **`restarts` is the console's own count**, beside the manager's `failures`.
   docs/12 has `hangar top` show "restarts"; the manager's message carries
   `failures`, which a restart by hand clears (`hangard/README.md`). The console
   counts the pid changes it sees since it started (so a manual restart counts, and
   the count starts over with the console). Both are shown; the page labels them
   `Restarts` and `Fails`.
2. **The manifest has `[Subscribe *]`** beside the two manager topics. The topic
   browser decodes any topic by name, as `hangar echo` does unwired; under the
   manager a module may open only what its wiring lists, and a glob subscription is
   the documented way to list "whatever exists" (docs/02, glob subscriptions, the
   recorder). The glob is never opened or waited on; only named opens pass through
   it. The template therefore subscribes to three things, not two.
3. **`/api/topic/<name>` nests the fields** in an array of `{name, type, count,
   comment, value}` instead of `hangar echo --json`'s flat object, so that the page
   gets the order, the type and the `.msg` comment (the units) without a second
   request for the schema. The envelope keys (`seq`, `stamp_ns`, `publish_ns`,
   `stale`) are `echo --json`'s.
4. **Long arrays are summarised** (over 64 elements: the size as a string) so a
   frame topic does not become a million-number JSON body at 1 Hz.
5. **The server loop is the module's, not `lib/net/http.c`'s** (above): the parser
   and the response format are electric-eye's; the thread-per-connection driver is
   not used, since the brief asked for one epoll loop beside `hg_wait_fd`.
6. **A POST needs `Content-Type: application/json`** (403 otherwise). Not in any
   doc; a cheap guard against a cross-site form pressing the buttons through an
   operator's browser that can reach `127.0.0.1:8080`.
7. **The log is per module file, not one stream.** docs/03 "Logs" has one fallback
   file per module; `/api/log` serves them all with a tail each, and the page picks
   one. With journald there is nothing the module can read without `libsystemd`,
   so it says where the lines are instead.
8. **The role is read from the wiring first.** The brief said "from its own config
   or the manager's conf"; both work unwired (`--role`, then `hangard.conf`), but
   under the manager the wiring already names `hangar.<role>.system`, and taking
   the role from there means the topside's manifest copy is the only edit.
9. **`hangar-admin` is a supplementary group in the template**, not a dedicated
   `User=`: the module needs nothing else and runs as `hangar` like any other;
   the group alone gives the buttons their standing. Installers that want the page
   read-only drop the line.
