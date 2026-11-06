# Calls

Request and reply between modules: `thrusters.arm`, `recorder.start`,
`camera.snapshot`, `nav.set_origin`. A call is a short, typed exchange that the
serving module answers within its deadline. Anything that streams, or takes longer,
belongs on a topic.

## The rules

- **A call goes straight to the serving module.** No process sits between client and
  server; the manager is not even asked for the address.
- **The manager owns each listening socket** and hands it to the module at every
  start, so calls made while the module restarts wait in the socket instead of
  failing.
- **Requests and replies are fixed-size typed messages**, written in `.msg` files like
  topic messages.
- **No threads on either side.** A call wakes a server asleep in `hg_wait`.
- **A call is answered within its deadline, 100 ms by default.** Longer work replies
  "accepted" at once and reports progress on a topic.
- **A request that has outlived its client's deadline is never executed.** A command
  that waited in the socket through a restart cannot run after its caller gave up.
- **Every call is recorded**, on the server's `<module>.calls` topic.
- **Callers are identified by module**, not only by user, and checked against the
  method's `Allow` list. Methods of a module with an actuator output deny every
  module not listed.

## Names and addresses

A call is named `<module>.<method>`: `thrusters.arm`. Method names follow the module
name rule (`[a-z0-9_]`, 1 to 31 bytes). The address follows from the module's name:
the `SOCK_SEQPACKET` socket `/run/hangar/call/<module>`, beside the module's wake word
`/run/hangar/call/<module>.wake`.

## Declaring calls

The server declares each method it serves; a client declares each method it calls.

```ini
# thrusters.module
[Serve arm]
Request=arm_req
Reply=arm_rep
Allow=pilot_console teleop
Deadline=100ms
```

```ini
# pilot_console.module
[Call thrusters.arm]
```

| `[Serve <method>]` key | Default | |
|---|---|---|
| `Request` | — (required) | Message type of the request |
| `Reply` | — (required) | Message type of the reply |
| `Allow` | every module; none if the module has an actuator output | Module names that may call |
| `Deadline` | `100ms` | How long the handler may take |

`[Call <module>.<method>]` has no keys: the types are the server's. The library
refuses a call its module did not declare, as it refuses an undeclared topic. The
manager checks, when it loads the manifests, that a client is in the method's `Allow`
list and reports a declared call to a method nobody serves as a warning.

Requests and replies are at most 16 KB each. Larger data goes on a topic, and the call
names it.

## Using calls

```c
// client
arm_req req = {.arm = 1};
arm_rep rep;
hg_call_result r = hg_call("thrusters.arm", &ARM_REQ_TYPE, &req,
                           &ARM_REP_TYPE, &rep, 2000 /* ms */);
// r.status: HG_OK or an error below; r.reason: one line when refused

// server: a handler per method, registered once
static int on_arm(const hg_call_ctx *c, const void *req, void *rep, char *reason);
hg_serve_method("arm", on_arm, ctx);

for (;;) {
  switch (hg_wait(subs, n, 100)) {
  case HG_CALL: hg_serve(); break;      // answers every pending request
  case HG_MSG:  /* topics */ break;
  case HG_TIMEOUT: break;
  }
}
```

The client passes the types it was built with; their hashes go in the envelope, so a
client built against another version of a `.msg` gets `HG_E_BADTYPE`, not a misread
reply. `hg_wait` reports `HG_CALL` before any topic, so a busy topic cannot starve the
module's calls.

A handler returns `HG_OK` and fills the reply, or `HG_E_REFUSED` with a reason
("not armed: depth sensor stale"). `c` names the calling module, the call id and the
request's deadline. A module whose loop is driven by a timer rather than `hg_wait`
calls `hg_serve()` once per iteration; it costs one `poll` with a zero timeout.

## Protocol

One connection per client and server, opened at the first call and kept. One request
packet, one reply packet, matched by call id. Native byte order.

**Request envelope** (112 bytes), then the request:

| Offset | Type | Field | |
|---|---|---|---|
| 0 | char[4] | `magic` | `"HGC1"` |
| 4 | u32 | `version` | 1 |
| 8 | u64 | `call_id` | per connection, counting from 1 |
| 16 | u64 | `deadline_ns` | `CLOCK_MONOTONIC`: when the client stops waiting |
| 24 | u64 | `request_type` | type hash |
| 32 | char[32] | `method` | |
| 64 | char[48] | `on_behalf_of` | the remote caller, set by the bridge only ([Callers](#callers)); empty otherwise |

**Reply envelope** (96 bytes), then the reply (absent unless `status` is `HG_OK`):

| Offset | Type | Field | |
|---|---|---|---|
| 0 | char[4] | `magic` | `"HGC1"` |
| 4 | i32 | `status` | below |
| 8 | u64 | `call_id` | |
| 16 | u64 | `reply_type` | type hash |
| 24 | u64 | `received_ns` | when the server read the request |
| 32 | char[64] | `reason` | one line, when not `HG_OK` |

**The client** connects (once), sends the request, then wakes the server: after
`send` returns, it increments the 32-bit word in `/run/hangar/call/<module>.wake` with
a sequentially consistent atomic add and calls `FUTEX_WAKE`. It waits for the reply
with `poll` until its timeout.

**The server**, in `hg_wait`, reads the wake word (acquire) *before* checking its
sockets, and
includes it, with that value, in the `futex_waitv` set alongside its topics. A request
sent at any point after the read changes the word, so the wait returns at once; none
is missed. `hg_serve` accepts pending connections and reads every pending request
without blocking, then for each:

(The server keeps a fixed table of connections, sized at `hg_init` from the clients
the manifests declare plus 8 for operators; when it is full, a new connection is
answered `HG_E_BUSY` and closed, so a flood of clients cannot make the module
allocate.)

1. drops it with `HG_E_EXPIRED`, without calling the handler, if `deadline_ns` has
   passed;
2. identifies the caller ([Callers](#callers)) and refuses it with `HG_E_DENIED`
   if it is not allowed;
3. checks the method and the request type;
4. calls the handler, sends the reply, and publishes the record.

A handler that overruns `Deadline` is not interrupted; the overrun is logged and
visible in the record (`replied_ns − received_ns`).

## Status codes

| Code | Meaning | The handler ran? |
|---|---|---|
| `HG_OK` | done; the reply holds the result | yes |
| `HG_E_REFUSED` | the handler said no; `reason` says why | yes |
| `HG_E_DENIED` | the caller is not in `Allow` | no |
| `HG_E_NOMETHOD` | no such method | no |
| `HG_E_BADTYPE` | the request type differs from the server's | no |
| `HG_E_EXPIRED` | the request waited past its deadline | no |
| `HG_E_NOSERVER` | no socket: the module is not enabled on this machine | no |
| `HG_E_NOLINK` | the method is on the other side of the tether, and the link is down | no |
| `HG_E_TIMEOUT` | client side: no reply within the timeout | **unknown** |
| `HG_E_LOST` | client side: the connection broke mid-call (the server died) | **unknown** |

The last two leave the outcome unknown; the record on `<module>.calls` says what
actually happened. Methods should therefore be **idempotent** where they can be
(`arm` when armed answers `HG_OK`), so that a client can safely repeat a call.

## While the server restarts

The listening socket belongs to the manager and outlives the module. A client that
connects while the module is down is queued in the socket's backlog, and its request
waits there; the new instance answers it if its deadline has not passed, and drops it
with `HG_E_EXPIRED` otherwise. A connection that was open to the old instance breaks:
a call in flight on it returns `HG_E_LOST`, and the next call reconnects.

## Callers

The server's library identifies the calling process by its PID (`SO_PEERCRED`) and
looks it up in the manager's registry, `/run/hangar/registry`, which every member of
`hangar` can read ([03-manager](03-manager.md#adoption)); so a caller is identified as
a module even when modules share a user. A match counts only if the entry's state is a
running one and the start time in `/proc/<pid>/stat` equals the registry's: a PID
reused by another process after a module died is not taken for that module. The
identity is fixed when the connection is accepted, re-reading the registry if it has
changed since; a PID not in it is re-read once, for a module started a moment ago. A process that is not a module is allowed only if
it is in the `hangar-admin` group (`SO_PEERGROUPS`): the `hangar call` command, an
operator at a shell.

A call forwarded across the tether arrives from the bridge, with the original caller
in `on_behalf_of` (`topside:pilot_console`, `topside:admin:<user>`). The server takes
that name as the caller, and checks it against `Allow`, only when the calling process
is the module with the `[Bridge]` section; from any other caller the field is refused
with `HG_E_DENIED` ([08-bridge](08-bridge.md#calls-across-the-tether)).

## The call record

Each module that serves calls publishes `<module>.calls`, an event topic of 64 slots,
one message per request, including the denied and the expired ones:

```
char method[32]
char caller[48]       # module name, "admin:<user>", or "topside:<module>"
u64  call_id
u64  received_ns
u64  replied_ns
i32  status
char reason[64]
u64  request_type
u64  reply_type
u8   request[R]       # R, P: the largest request and reply among the module's methods
u8   reply[P]
```

The recorder decodes `request` and `reply` from their type hashes. Operator commands,
mode changes and every refusal thereby sit in the recorded data beside the topics, in
the same time base, with nothing written by the module's author. The topic name is
reserved: no manifest may declare a topic `<module>.calls` of its own.

## Tests

- No lost wake: requests sent at every point of the server's check-then-wait sequence.
- A request queued through a server restart is answered when in time and dropped
  unexecuted when expired.
- `HG_E_LOST` for a call in flight when the server is killed; the next call reconnects.
- Callers: allowed and denied modules sharing one user; deny-by-default for an actuator
  module; `hangar-admin` from a shell.
- Every outcome appears on `<module>.calls`, with the request and reply decodable.
