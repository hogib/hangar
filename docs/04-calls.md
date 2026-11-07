# Calls

Request and reply between modules: `thrusters.arm`, `recorder.start`,
`camera.snapshot`, `nav.set_origin`. A call is a short, typed exchange that the
serving module answers within its deadline. Anything that streams, or takes longer,
belongs on a topic of its own.

## The rules

- **A call is two messages on two topics.** The request goes on the caller's request
  topic, `<caller>.req`; the reply on the server's reply topic, `<server>.rep`. Both
  are ordinary topics ([01-topics](01-topics.md)): one writer, a ring, a futex,
  recorded like everything else. No socket, no address, no process in between; the
  manager is in no module's call.
- **The caller is the writer of the request topic.** A topic has one writer, named in
  its header, and the manager guarantees it ([01-topics](01-topics.md#permissions)).
  Nothing in a request asserts who sent it.
- **Requests and replies are fixed-size typed messages**, written in `.msg` files like
  topic messages.
- **Nothing blocks, and nothing has a thread.** `hg_call_start` publishes and returns;
  `hg_call_poll` reads the reply topic. A request or a reply wakes a module asleep in
  `hg_wait` as any message does.
- **Neither calls nor topics starve the other.** `hg_wait` reports every source that is
  ready, and `hg_serve` answers at most 8 requests per pass.
- **A call is answered within its deadline, 100 ms by default.** Longer work replies
  "accepted" at once and reports progress on a topic. A request that has outlived its
  deadline is never executed, however long it waited in the ring.
- **Callers are identified by module**, from the topic the request came on, and checked
  against the method's `Allow` list. Nothing in `Allow` is implicit: a caller on the
  other side of the tether, or an operator, passes only where the list names it.
  Methods of a module with an actuator output deny every caller not listed.

## Declaring calls

A call is named `<module>.<method>`: `thrusters.arm`. Method names follow the module
name rule (`[a-z0-9_]`, 1 to 31 bytes), and module names are unique across the vehicle
and the topside together, so a call names its module the same way on either side. The
server declares each method it serves; a caller declares each method it calls.

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
| `Allow` | `* admin`: every module on this machine, and `admin`; none if the module has an actuator output | Callers that may call, as tokens ([Allow tokens](#allow-tokens)): `safety`, `topside:pilot_console`, `*`, `topside:*`, `admin`, `topside:admin` |
| `Deadline` | `100ms` | How long the handler may take |

`[Call <module>.<method>]` has no keys when the module is enabled on this machine: the
types are the server's. For a method of the other side, whose manifest is not here,
`Request=` and `Reply=` name the types the caller was built with, so the manager can
size the request topic; the server checks the hashes regardless. The library refuses
a call its module did not declare. The manager checks that a caller on this machine is
in the method's `Allow` list, and warns of a declared call that nothing on this
machine serves and the bridge does not carry: such a call returns `HG_E_NOROUTE`.
Requests and replies are at most 16 KB each; larger data goes on a topic, and the call
names it. `set_params` is a method like any other, served by the library for a module
with `[Params]`: request type `set_params:<type>`, reply `set_params_rep`
([05-parameters](05-parameters.md#setting-parameters)).

## The two topics

The manager creates them from the manifests, as it creates `<module>.params`; their
names are reserved ([02-manifest](02-manifest.md#how-the-manager-uses-manifests)):

| Topic | Writer | Slots | Payload |
|---|---|---|---|
| `<caller>.req` | the caller; one for every module with a `[Call]` | 16 | request envelope, then the largest request type among the caller's `[Call]`s |
| `<server>.rep` | the server; one for every module with a `[Serve]` or `[Params]` | 16 | reply envelope, then the largest reply type among the methods it serves |

Both are event topics, of the generated types `req:<module>` and `rep:<module>`
([03-manager](03-manager.md#wiring)); a reader without the code (the recorder, Python)
decodes the envelope from the schema in the header, and `request` and `reply` from
their type hashes ([The envelopes](#the-envelopes)). The manager's own
`hangar.<role>.admin.req` is a request topic too, through which operators call
([Admin calls](#admin-calls)).

**Wiring.** From every `[Call]` and `[Serve]` the manager derives who reads what: a
server's wiring gets `sub <caller>.req` for each module on this machine that calls it,
`sub hangar.<role>.admin.req`, and, when the bridge may bring request topics from the
other side, `subglob *.req`, which `hg_wait` polls every 10 ms at most, as it polls any
glob subscription ([01-topics](01-topics.md#reading)); a caller's
gets `sub <server>.rep` for each module it calls, a mirrored topic when the server is
on the other side. The named subscriptions count toward the 127 topics one wait can
hold (128 futexes, one kept for the directory word, [01-topics](01-topics.md#reading)).
The request topics a server reads, named or matched by `*.req`, are implicit `Sources`
of every output of the server: a reply references the request it answers, and a
`safety.state` message may reference the `arm` request that led to it
([01-topics](01-topics.md#provenance)).

**Calling.** `hg_call_start` fills the envelope, publishes the request with no
references (`HG_NO_REFS`), and returns. A module has at most 15 calls in flight,
`Slots − 1`, what a reader in order can count on ([01-topics](01-topics.md#reading)):
a request in flight is never overwritten, and the sixteenth call gets `HG_E_BUSY`
locally. `hg_call_poll` reads the server's `.rep` topic in order, keeps the replies
whose `caller_id` is its own request topic's id and whose `call_id` is in flight, and
checks `reply_type` (`HG_E_BADTYPE`). A reply that arrives after its call timed out is
discarded by its call id; one that left the server's ring before the caller read it is
a timeout.

**Serving.** `hg_serve` reads each request topic in order, skips every message whose
`target_id` is not its own `.rep` topic's id, and for each request addressed to it:

1. publishes `HG_E_EXPIRED`, without calling the handler, if `deadline_ns` has passed;
2. identifies the caller from the topic ([Callers](#callers)) and publishes
   `HG_E_DENIED` if it is not in the method's `Allow`;
3. checks the method (`HG_E_NOMETHOD`) and the request type (`HG_E_BADTYPE`);
4. calls the handler and publishes its reply.

It takes the topics in turn, answers at most 8 requests per pass, and leaves the rest
in their rings, where `hg_wait` finds them unread and returns at once
([01-topics](01-topics.md#reading)). A handler that overruns `Deadline`
is not interrupted; the overrun is logged and visible in the recording (the reply's
`publish_ns` − `received_ns`). A request the server never read gets no reply: the
caller times out, and the recording shows the request and its fate.

**While the server restarts**, requests wait in their callers' rings; nothing holds
them for the server. At start the library reads each request topic from the oldest
message still in its ring, and its own `.rep` topic from the oldest reply still there:
a request already answered is skipped, an expired one is published `HG_E_EXPIRED`, the
rest are served. A request answered before the restart whose reply has since left the
`.rep` ring, and which is still in time, is served again. Methods should therefore be
**idempotent** where they can be (`arm` when armed answers `HG_OK`), so that a repeat,
by the server or by a caller after `HG_E_TIMEOUT`, is harmless.

## The envelopes

**Request envelope** (112 bytes), then the request:

| Offset | Type | Field | |
|---|---|---|---|
| 0 | u64 | `call_id` | per caller, counting from 1 |
| 8 | u64 | `deadline_ns` | `CLOCK_MONOTONIC`: when the caller stops waiting |
| 16 | u64 | `request_type` | type hash |
| 24 | u32 | `target_id` | topic id of the server's `<server>.rep` |
| 28 | u32 | — | reserved, 0 |
| 32 | char[32] | `method` | |
| 64 | char[48] | `caller` | empty; set by the manager on `hangar.<role>.admin.req` only ([Admin calls](#admin-calls)) |

**Reply envelope** (96 bytes), then the reply (absent unless `status` is `HG_OK`):

| Offset | Type | Field | |
|---|---|---|---|
| 0 | u64 | `call_id` | |
| 8 | u32 | `caller_id` | topic id of the request topic the request was read from |
| 12 | i32 | `status` | [Status codes](#status-codes) |
| 16 | u64 | `reply_type` | type hash |
| 24 | u64 | `received_ns` | `CLOCK_MONOTONIC`: when the server read the request |
| 32 | char[64] | `reason` | one line, when not `HG_OK` |

`target_id` and `caller_id` are topic ids ([01-topics](01-topics.md#names)), not
hashes of module names: the two machines' admin topics have the same writer, the
manager, and differ only by name, and one `.rep` topic carries replies to both.

Across the tether the bridge rewrites `deadline_ns` and `received_ns` into the
receiver's clock ([Across the tether](#across-the-tether)); no other byte of either
envelope is ever changed after it is published.

## The API

```c
// caller, without waiting: the module's loop carries on
arm_req req = {.arm = 1};
arm_rep rep;
hg_pending *pc;
hg_call_start("thrusters.arm", &ARM_REQ_TYPE, &req, &ARM_REP_TYPE, &pc,
              500 /* ms */);
...
hg_call_result r = hg_call_poll(pc, &rep);
// r.status: HG_PENDING until the reply or the timeout, then HG_OK or an error below;
// r.reason: one line when refused

// caller, blocking: for simple modules
r = hg_call("thrusters.arm", &ARM_REQ_TYPE, &req, &ARM_REP_TYPE, &rep, 500);

// server: a handler per method, registered once
static int on_arm(const hg_call_ctx *c, const void *req, void *rep, char *reason);
hg_serve_method("arm", on_arm, ctx);

for (;;) {
  if (hg_wait(subs, n, 100) == 0) continue;  // 0: timed out; else news, a glob match included
  hg_serve();                                // the callers' .req topics: at most 8 requests
  collect_replies();                         // hg_call_poll on each pending call
  read_topics();                             // the module's own subscriptions
}
```

`hg_wait` reports news on any of the module's topics, its callers' `.req` topics and
its servers' `.rep` topics among them ([01-topics](01-topics.md#reading)); in a server
with `subglob *.req` it returns at least every 10 ms, whatever timeout the module
passes, and reports a request on a mirrored topic like any other. Each pass then
serves, polls and reads, each a read of a ring. `hg_call_poll` returns
`HG_PENDING` until the reply or the timeout, then frees the pending call. `hg_call` is
`hg_call_start` followed by a wait on that server's `.rep` topic, and stops the
module's loop for up to the timeout: the library refuses it in a module with an
actuator output, and the safety and `mavlink` modules do not use it; the bridge makes
no calls at all. A handler
returns `HG_OK` and fills the reply, or `HG_E_REFUSED` with a reason ("not armed:
depth sensor stale"); `c` carries the caller's token, the call id, the deadline, and
the request's reference `req`, `{topic_id, seq}`, for `hg_publish_with`. A module
driven by a timer, or waiting through `hg_wait_fd`, calls `hg_serve()` and
`hg_call_poll()` once per iteration, at least every 10 ms when its wiring holds
`subglob *.req` ([01-topics](01-topics.md#reading)); each is a read of a ring, with no
system call.

## Status codes

| Code | Meaning | The handler ran? |
|---|---|---|
| `HG_OK` | done; the reply holds the result | yes |
| `HG_E_REFUSED` | the handler said no; `reason` says why | yes |
| `HG_E_DENIED` | the caller is not in `Allow` | no |
| `HG_E_NOMETHOD` | no such method | no |
| `HG_E_BADTYPE` | the request type differs from the server's; or, caller side, the reply type from the caller's | no |
| `HG_E_EXPIRED` | the request waited past its deadline | no |
| `HG_E_BUSY` | caller side: 15 calls are already in flight | no |
| `HG_E_NOROUTE` | caller side: no module serves the method on either side, known from the wiring | no |
| `HG_E_TIMEOUT` | caller side: no reply within the timeout | **unknown** |

`HG_E_TIMEOUT` leaves the outcome unknown; the server's `.rep` topic in the recording
says what actually happened.

## Callers

The caller is derived from the topic the request came on, never from the request:

| Request topic | Caller | `Allow` tokens that match |
|---|---|---|
| `<module>.req`, local (`origin_boot` all zero) | `<module>`: `safety` | `<module>`; `*`, every module on this machine |
| `<module>.req`, mirrored from the other side | `<role>:<module>`: `topside:pilot_console` | `<role>:<module>`; `<role>:*`, every module of that side |
| `hangar.<role>.admin.req`, local | `admin:<user>`, from `caller` | `admin` |
| `hangar.<role>.admin.req`, mirrored | `<role>:admin:<user>` | `<role>:admin` |

`<role>` is `vehicle` or `topside`. The library derives the token from the topic's
header (`writer`, and `origin_boot` zero or not) and the module's own role, which the
wiring carries; a mirrored topic's role is the other side's
([08-bridge](08-bridge.md#calls-across-the-tether)). No module can write another's
`.req`, no local module can bear a mirrored topic's name (a collision is refused at
the handshake), and the `caller` field is read on the admin topics alone. So no
caller, on either side, chooses its own name.

### Allow tokens

A token matches only its own form: `safety` never matches `topside:safety`, `*` never
matches a module of the other side or an operator, `admin` never matches a caller from
the other side. The default `Allow`, `* admin`, therefore admits nobody from the other
side; a method meant for the topside names its callers. A list is tokens separated by
blanks: `* topside:* admin topside:admin` admits every module and every operator on
both sides. User names are at most 32 bytes, so every caller name fits in 48 bytes.

### Admin calls

`hangar call` and `hangar param` ask the manager over the control socket
([03-manager](03-manager.md#control-socket), [12-tools](12-tools.md)). The manager is
the writer of `hangar.<role>.admin.req`: it refuses a process outside `hangar-admin`
and a method no module serves on either side, otherwise puts `admin:<user>` in
`caller`, publishes the request with the command's deadline, and answers with the
`call_id`. The command then reads the server's `.rep` topic like any caller, keeping
the reply whose `call_id` and `caller_id` are its own. The topic's type, `req:hangar`,
is the request envelope followed by 16 KB, the largest request a method may take, so a
request to any method on either side fits. Every server reads its own machine's admin
topic, and the other side's where the bridge carries it, through its `subglob *.req`
([03-manager](03-manager.md#wiring)). A process that is neither a module nor in
`hangar-admin` has no way to make a call.

## Across the tether

A call to a module of the other side is the same two messages, mirrored by the bridge
([08-bridge](08-bridge.md#calls-across-the-tether)): `<caller>.req` goes to the side
where the server runs and `<server>.rep` comes back, both `reliable` at `call`
priority. `[Calls] Remote=` in the bridge's configuration names the methods one side's
modules and operators may call across, and stands for those mirrors, this side's
`hangar.<role>.admin.req` among the request topics. The bridge translates
`deadline_ns` into the receiver's clock and shortens it by the clock's uncertainty, so
a request that expired on the way is dropped unexecuted, and `received_ns` back into
the caller's. While the link is down, requests wait in the caller's ring and cross when
it is back, by then expired; the caller has had `HG_E_TIMEOUT` meanwhile.

## Tests

- `hg_call_start`: the caller's loop keeps publishing at its rate while a call is in
  flight; the reply wakes it; `HG_E_TIMEOUT` after the timeout, and a late reply
  discarded by call id; `HG_E_BUSY` for a sixteenth call in flight; another caller's
  reply on the same `.rep` topic skipped. `hg_call` refused in a module with an
  actuator output.
- Starvation: one caller at its limit, re-issuing as each call concludes, and a topic
  publishing at 1 kHz, against one server; every loop iteration reads the topics and
  serves at most 8 requests; two callers' request topics taken in turn.
- Server restart: a request waiting in the ring is answered when in time and published
  `HG_E_EXPIRED` unexecuted when not; a request already answered, whose reply is still
  in the `.rep` ring, is not run again; a request to another server in the same ring is
  skipped. An unknown request or reply type gives `HG_E_BADTYPE`; a declared call that
  nothing on either side serves gives `HG_E_NOROUTE` at once.
- Callers: allowed and denied modules sharing one user; a module opening another's
  `.req` refused by the manager; deny-by-default for an actuator module; `admin` from a
  shell through `hangar.<role>.admin.req`, the command reading its reply from the
  server's `.rep`; the `caller` field set in a module's own request ignored; a process
  outside `hangar-admin` refused by the manager.
- `Allow` tokens: `topside:pilot_console`, `topside:*` and `topside:admin` matched only
  on a mirrored request topic; `*` matching no operator and no remote caller; the
  default list denying every caller from the other side.
- Provenance and recording: every reply references the request it answers; a
  `safety.state` message published from a handler references the request; every
  request and outcome in `<caller>.req` and `<server>.rep` of a session, decodable from
  their schemas and type hashes.
- Across the tether: a request held through a link outage arrives expired and is
  published `HG_E_EXPIRED`; `received_ns` of a remote reply in the caller's clock; a
  remote call in flight while `pilot.wrench` keeps its rate; a request on a mirrored
  `.req` topic, matched by `subglob *.req`, served within 10 ms of its arrival by a
  server whose loop passes `hg_wait` a 100 ms timeout.
