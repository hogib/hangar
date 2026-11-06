# Calls

Request and reply between modules: `thrusters.arm`, `recorder.start`,
`camera.snapshot`, `nav.set_origin`. A call is a short, typed exchange that the
serving module answers within its deadline. Anything that streams, or takes longer,
belongs on a topic.

## The rules

- **A call goes straight to the serving module.** No process sits between client and
  server; the manager is not even asked for the address.
- **The manager owns each listening socket**, and the module fetches it from the
  manager at every start, so calls made while the module restarts wait in the socket
  instead of failing.
- **Requests and replies are fixed-size typed messages**, written in `.msg` files like
  topic messages.
- **No threads on either side.** A request wakes a server asleep in `hg_wait`; a reply
  wakes a client asleep in `hg_wait`.
- **A call need not stop the caller.** `hg_call_start` sends the request and returns at
  once; the reply is collected later. A module that must keep publishing while it waits
  (the bridge, the safety and `mavlink` modules, every module with an actuator output)
  makes its calls this way only.
- **Neither calls nor topics starve the other.** `hg_wait` reports every source that is
  ready, and `hg_serve` answers at most 8 requests per pass.
- **A call is answered within its deadline, 100 ms by default.** Longer work replies
  "accepted" at once and reports progress on a topic.
- **A request that has outlived its client's deadline is never executed.** A command
  that waited in the socket through a restart cannot run after its caller gave up.
- **Every call is recorded**, on the server's `<module>.calls` topic.
- **Callers are identified by module**, not only by user, and checked against the
  method's `Allow` list. Nothing in `Allow` is implicit: a caller on the other side of
  the tether, or an operator, passes only where the list names it. Methods of a module
  with an actuator output deny every caller not listed.

## Names and addresses

A call is named `<module>.<method>`: `thrusters.arm`. Method names follow the module
name rule (`[a-z0-9_]`, 1 to 31 bytes). The address follows from the module's name:
the `SOCK_SEQPACKET` socket `/run/hangar/call/<module>`, beside the module's wake word
`/run/hangar/call/<module>.wake`. Every module has a wake word, whether it serves calls
or not: requests wake a server through it, and replies wake a client
([03-manager](03-manager.md#module-page)).

Module names are unique across the vehicle and the topside together, so a call names
its module the same way on either side. The one exception is the `bridge` module,
present on both machines, whose methods are never remote. A method of a module on the
other side is reached through the bridge, which serves a stand-in socket at the same
address on this side when `[Calls] Remote=` lists the method; a `Remote` entry that
matches a module enabled on this machine is refused as remote, and the call stays local
([08-bridge](08-bridge.md#calls-across-the-tether)).

## Declaring calls

The server declares each method it serves; a client declares each method it calls.

```ini
# thrusters.module
[Serve arm]
Request=arm_req
Reply=arm_rep
Allow=safety
Deadline=100ms
```

```ini
# safety.module
[Call thrusters.arm]
```

| `[Serve <method>]` key | Default | |
|---|---|---|
| `Request` | — (required) | Message type of the request |
| `Reply` | — (required) | Message type of the reply |
| `Allow` | `* admin`: every module on this machine, and `admin`; none if the module has an actuator output | Callers that may call, as tokens ([Callers](#callers)): `safety`, `topside:pilot_console`, `*`, `topside:*`, `admin`, `topside:admin` |
| `Deadline` | `100ms` | How long the handler may take |

`[Call <module>.<method>]` has no keys: the types are the server's. The library
refuses a call its module did not declare, as it refuses an undeclared topic. The
manager checks, when it loads the manifests, that a client on this machine is in the
method's `Allow` list, and reports a declared call to a method nobody serves as a
warning.

Requests and replies are at most 16 KB each. Larger data goes on a topic, and the call
names it.

## Using calls

```c
// client, without waiting: the module's loop carries on
arm_req req = {.arm = 1};
arm_rep rep;
hg_pending *pc;
hg_call_start("thrusters.arm", &ARM_REQ_TYPE, &req, &ARM_REP_TYPE, &pc,
              500 /* ms */);
...
hg_call_result r = hg_call_poll(pc, &rep);
// r.status: HG_PENDING until the reply or the timeout, then HG_OK or an error below;
// r.reason: one line when refused

// client, blocking: for simple modules and the `hangar call` command
r = hg_call("thrusters.arm", &ARM_REQ_TYPE, &req, &ARM_REP_TYPE, &rep, 500);

// server: a handler per method, registered once
static int on_arm(const hg_call_ctx *c, const void *req, void *rep, char *reason);
hg_serve_method("arm", on_arm, ctx);

for (;;) {
  unsigned ready = hg_wait(subs, n, 100);         // a set; 0 on timeout
  if (ready & HG_READY_CALLS)   hg_serve();       // at most 8 requests
  if (ready & HG_READY_REPLIES) collect_replies(); // hg_call_poll on each pending call
  if (ready & HG_READY_TOPICS)  read_topics();
}
```

The client passes the types it was built with; their hashes go in the envelope, so a
client built against another version of a `.msg` gets `HG_E_BADTYPE`, not a misread
reply.

`hg_wait` returns a **ready set**, not one result:

| Bit | Set when |
|---|---|
| `HG_READY_CALLS` | a request is waiting in one of the module's sockets, or the last `hg_serve` stopped at its limit with requests still unread |
| `HG_READY_REPLIES` | a reply has arrived for a call started with `hg_call_start` |
| `HG_READY_TOPICS` | a subscribed topic has news |

A loop that acts on every bit it is given serves at most 8 requests and reads its
topics in the same iteration, so a flood of requests cannot stop a module reading its
inputs, and a busy topic cannot stop it answering calls.

`hg_call_start` takes the same arguments as `hg_call`, without the reply buffer, and
returns at once with a pending call, or with `HG_E_BUSY` when the module already has 8
calls in flight. `hg_call_poll` copies the reply when it has arrived, returns
`HG_E_TIMEOUT` once the timeout has passed without one, and frees the pending call on
any result but `HG_PENDING`. A reply that arrives after its call timed out is
discarded by its call id. `hg_call` is `hg_call_start` followed by a wait for that one
reply, and stops the module's loop for up to the timeout: the library refuses it in a
module with an actuator output or a `[Bridge]` section, and the safety and `mavlink`
modules do not use it.

A handler returns `HG_OK` and fills the reply, or `HG_E_REFUSED` with a reason
("not armed: depth sensor stale"). `c` names the calling module, the call id and the
request's deadline. A module whose loop is driven by a timer rather than `hg_wait`
calls `hg_serve()` and `hg_call_poll()` once per iteration; each costs one `poll` with
a zero timeout. So does a module that waits through `hg_wait_fd`
([01-topics](01-topics.md#reading)), each time its `eventfd` fires.

## Protocol

One connection per client and server, opened at the first call and kept. Each request
is one packet and each reply one packet, matched by call id; a client may have several
calls in flight on one connection. Native byte order.

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
a sequentially consistent atomic add and calls `FUTEX_WAKE`. `hg_call_start` returns
there. `hg_call` waits for the reply with `poll` until its timeout.

**The server**, in `hg_wait`, reads its own wake word (acquire) *before* checking its
sockets and its pending calls, and includes it, with that value, in the `futex_waitv`
set alongside its topics. A request or a reply that arrives at any point after the
read changes the word, so the wait returns at once; none is missed. On a wake, the
library tells requests from replies by checking its sockets without blocking.
`hg_serve` accepts pending connections and reads pending requests without blocking,
at most 8 per call, taking the connections in turn, then for each:

(The server keeps a fixed table of connections, sized at `hg_init` from the clients
the manifests declare plus 8 for operators; when it is full, a new connection is
answered `HG_E_BUSY` and closed, so a flood of clients cannot make the module
allocate.)

1. drops it with `HG_E_EXPIRED`, without calling the handler, if `deadline_ns` has
   passed;
2. identifies the caller ([Callers](#callers)) and refuses it with `HG_E_DENIED`
   if it is not allowed, then closes the connection;
3. answers `HG_E_BUSY` and closes the connection if it has sent more than 100
   requests in the last second;
4. checks the method and the request type;
5. calls the handler, sends the reply, and publishes the record.

After every reply it sends, the server wakes the client: it increments the wake word
of the calling module (the module the connection belongs to, the bridge for a
forwarded call) with a sequentially consistent atomic add and calls `FUTEX_WAKE`. A
caller that is not a module has no wake word and uses `hg_call`.

A handler that overruns `Deadline` is not interrupted; the overrun is logged and
visible in the record (`replied_ns − received_ns`).

## Status codes

| Code | Meaning | The handler ran? |
|---|---|---|
| `HG_OK` | done; the reply holds the result | yes |
| `HG_E_REFUSED` | the handler said no; `reason` says why | yes |
| `HG_E_DENIED` | the caller is not in `Allow`; the connection is closed | no |
| `HG_E_NOMETHOD` | no such method | no |
| `HG_E_BADTYPE` | the request type differs from the server's | no |
| `HG_E_EXPIRED` | the request waited past its deadline | no |
| `HG_E_BUSY` | the server's connection table is full, or the connection sent more than 100 requests in one second; or, client side, 8 calls are already in flight | no |
| `HG_E_NOSERVER` | no socket: the module is not enabled on this machine, and no `Remote` entry names it | no |
| `HG_E_NOLINK` | the method is on the other side of the tether, and the link is down | no |
| `HG_E_TIMEOUT` | client side: no reply within the timeout | **unknown** |
| `HG_E_LOST` | client side: the connection broke mid-call (the server died) | **unknown** |

`HG_PENDING` is not a status on the wire: `hg_call_poll` returns it while the reply has
not arrived and the timeout has not passed.

`HG_E_TIMEOUT` and `HG_E_LOST` leave the outcome unknown; the record on
`<module>.calls` says what actually happened. Methods should therefore be
**idempotent** where they can be (`arm` when armed answers `HG_OK`), so that a client
can safely repeat a call.

## While the server restarts

The listening socket belongs to the manager and outlives the module; at each start,
`hg_init` fetches it from the manager over the control socket
([03-manager](03-manager.md#starting-a-module)). A client that connects while the
module is down is queued in the socket's backlog, and its request waits there; the new
instance answers it if its deadline has not passed, and drops it with `HG_E_EXPIRED`
otherwise. A connection that was open to the old instance breaks: every call in flight
on it returns `HG_E_LOST`, and the next call reconnects.

## Callers

The server's library identifies the calling process by its PID (`SO_PEERCRED`) and
looks it up in the manager's registry, `/run/hangar/registry`, which every member of
`hangar` can read ([03-manager](03-manager.md#adoption)), skipping its `system:` line; so a caller is identified as
a module even when modules share a user. A match counts only if the entry's state is a
running one and the start time in `/proc/<pid>/stat` equals the registry's: a PID
reused by another process after a module died is not taken for that module. The
identity is fixed when the connection is accepted, re-reading the registry if it has
changed since; a PID not in it is re-read once, for a module started a moment ago. A
process that is not a module is identified as `admin:<user>` if it is in the
`hangar-admin` group (`SO_PEERGROUPS`): the `hangar call` command, an operator at a
shell. Any other process is refused with `HG_E_DENIED`.

A call forwarded across the tether arrives from the bridge, with the original caller
in `on_behalf_of`. The bridge that makes the call writes the caller's name with the
role of its peer in front (`topside:pilot_console`, `topside:admin:<user>`), so the
other side cannot choose it. The server takes that name as the caller, and checks it
against `Allow`, only when the calling process is the module with the `[Bridge]`
section; from any other caller the field is refused with `HG_E_DENIED`
([08-bridge](08-bridge.md#calls-across-the-tether)).

A caller is one of four forms, and `Allow` lists name them by token:

| Caller | `Allow` tokens that match it | |
|---|---|---|
| `<module>` | `<module>`, `*` | a module on this machine: `safety`. `*` is every module on this machine |
| `<role>:<module>` | `<role>:<module>`, `<role>:*` | a module on the other side, through the bridge: `topside:pilot_console`. `topside:*` is every module of the topside |
| `admin:<user>` | `admin` | a process in `hangar-admin` on this machine |
| `<role>:admin:<user>` | `<role>:admin` | a process in `hangar-admin` on the other side, through the bridge: `topside:admin` |

`<role>` is `vehicle` or `topside`. A token matches only its own form: `safety` never
matches `topside:safety`, `*` never matches a module of the other side, and neither
`*` nor `topside:*` matches an operator; `admin` never matches a caller from the other
side. The default `Allow`, `* admin`, therefore admits nobody from the other side; a
method meant for the topside names its callers. A list is written as tokens separated
by blanks: `* topside:* admin topside:admin` admits every module and every operator on
both sides. User names are at most 32 bytes, so every caller name fits in 48 bytes
with its terminating NUL.

## The call record

Each module that serves calls publishes `<module>.calls`, an event topic of 64 slots,
one message per request, including the denied, the expired and the busy ones:

```
char method[32]
char caller[48]       # "<module>", "admin:<user>", "<role>:<module>" or "<role>:admin:<user>"
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

- No lost wake: requests sent at every point of the server's check-then-wait sequence;
  replies sent at every point of the client's.
- `hg_call_start`: the client's loop keeps publishing at its rate while a call is in
  flight; the reply wakes it; `HG_E_TIMEOUT` after the timeout, and a late reply
  discarded; `HG_E_BUSY` for a ninth call in flight.
- `hg_call` refused in a module with an actuator output and in the `[Bridge]` module.
- Starvation: one client sending requests back to back, and a topic publishing at
  1 kHz, against one server; every loop iteration reads the topics and serves at most
  8 requests.
- A denied request closes its connection; a connection past 100 requests in one second
  is answered `HG_E_BUSY` and closed.
- A request queued through a server restart is answered when in time and dropped
  unexecuted when expired.
- `HG_E_LOST` for every call in flight when the server is killed; the next call
  reconnects.
- Callers: allowed and denied modules sharing one user; deny-by-default for an actuator
  module; `admin` from a shell; a process outside `hangar-admin` refused.
- `Allow` tokens: `topside:pilot_console`, `topside:*` and `topside:admin` matched
  only through the bridge; `*` matching every module on this machine and no operator
  and no remote caller; `on_behalf_of` refused from any other process; the default list
  denies every caller from the other side.
- Every outcome appears on `<module>.calls`, with the request and reply decodable.
