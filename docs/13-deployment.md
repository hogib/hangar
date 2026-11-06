# Deployment

How a build gets onto the vehicle and the topside, how the two stay at the same
version, and how an upgrade is undone. One signed release, one installer, two package
forms, and an upgrade path over the tether.

## The rules

- **One release for both sides.** The vehicle and the topside install the same release;
  what differs is the profile. The bridge refuses to mirror between versions that differ.
- **Every release is signed**, and the installer verifies the signature before it touches
  anything.
- **An upgrade never touches recordings, parameters or configuration.** It adds
  templates; it rewrites nothing the operator or the system has written.
- **The previous version stays installed**, and `--rollback` puts it back in one step.
- **Installing is idempotent.** Running the installer twice is the same as once.
- **The vehicle is upgraded from the topside**, over the tether, without a shell.

## A release

`hangar-<version>-<arch>.tar.zst`, built by CI from one tag for `aarch64`, `armv7` and
`x86_64`:

| Path | |
|---|---|
| `bin/` | the static binaries: `hangard`, `hangar`, `hangar-install`, every module program |
| `share/hangar/modules/` | manifest templates ([02-manifest](02-manifest.md)) |
| `share/hangar/profiles/<profile>` | the manifests a profile enables: `vehicle`, `topside`, `sim` |
| `share/hangar/msg/`, `datasets/`, `policies/` | message types, dataset specs, policy specs |
| `share/hangar/ardusub.param` | ArduSub's parameters for phase 1 ([10-mavlink](10-mavlink.md)) |
| `share/hangar/udev/` | rules giving devices to the exclusive groups ([07-safety](07-safety.md#the-actuator-gate)) |
| `share/hangar/systemd/hangard.service` | |
| `MANIFEST` | the release version, the target, the SHA-256 of every file, and the source: the git commit and the repository URL the binaries were built from (the GPL's corresponding source, `LICENSING.md`) |
| `MANIFEST.minisig` | the manifest's signature |

Signing is minisign (ed25519): the project's public key is built into `hangar-install`
and `hangard`, the private key is held by the maintainer, and there is no certificate
chain to manage. A tarball whose signature or hashes fail is refused with the reason.

The same CI job builds a Debian package per architecture, `hangar_<version>_<arch>.deb`,
from the same tree, with `hangar-install` as its `postinst` ([Debian package](#debian-package)).

## The installer

`hangar-install <tarball> --role vehicle|topside [--profile <name>] [--dev <checkout>]`,
a static program in the tarball, run as root:

1. verify `MANIFEST.minisig` and every hash;
2. create the users and groups if missing: `hangar`, `hangar-admin`, `hangar-rec`, one
   `hg-<module>` per actuator and per module whose template asks for one, the exclusive
   device groups (`nav-spi`, `nav-i2c`, `nav-gpio`, …); add the installing user to
   `hangar` and `hangar-admin`;
3. install the udev rules and reload them;
4. unpack to `/usr/lib/hangar/<version>/` and point the symlink `/usr/lib/hangar/current`
   at it; `/usr/bin/hangar` and `/usr/bin/hangard` are symlinks through `current`;
5. write `/etc/hangar/hangard.conf` with `Role=` if there is none (an existing one is
   kept);
6. enable the profile: for each manifest in the profile, a symlink in
   `/etc/hangar/modules/` to the template under `current`, unless a file of that name is
   already there (an operator's edited copy wins); remove symlinks of modules the
   profile no longer has; never remove a plain file;
7. put every new template under `/etc/hangar/*.d/` as `<name>.new` when a modified copy
   exists, so the operator can diff;
8. install the unit and `systemctl daemon-reload`; then `systemctl reload hangard`
   (re-execution: modules keep running, [03-manager](03-manager.md#restarting-the-manager))
   or `enable --now` on a first install.

`hangar-install --rollback` points `current` at the previous version and reloads;
`--list` shows installed versions; `--prune` removes all but the current and the
previous. `--dev <checkout>` makes `current` point at a build directory instead of a
copied tree, with the same steps otherwise, for development on a bench.

The installer's own log goes to `/var/log/hangar-install.log` and to the journal.

## What survives an upgrade

| | Written by | Upgrade |
|---|---|---|
| `/etc/hangar/hangard.conf`, `bridge.conf`, `recorder.conf`, `pairer.conf`, `mavlink.conf` | operator | kept; new templates beside them as `.new` |
| `/etc/hangar/modules/*` | installer (symlinks) and operator (copies) | symlinks follow the new version; copies kept |
| `/etc/hangar/bridge.key`, `mavlink.key` | installer, once | kept |
| `/var/lib/hangar/<module>/params` | modules | kept; loaded by the strict loader, which takes defaults for new fields and refuses unknown ones ([05-parameters](05-parameters.md#saved-values)) |
| `/var/lib/hangar/policy_<name>/*.hgw` | policies | kept |
| `/var/lib/hangar/recordings/` | recorder | never touched; `.hgr` files carry their schema and stay readable by any version |

A manifest or message type that changed incompatibly is caught by the manager's graph
check or the parameter loader at the first start after the upgrade, and reported by
`hangar check`, never applied silently.

## Version lock-step across the tether

The bridge handshake carries the release version and the hash of the installed `msg/`
tree. If either differs, `bridge.<role>.link.config_match` is false, nothing is
mirrored, `hangar check` fails on both sides with the two versions named, and the
console shows it. There is no compatibility mode between versions: the two sides are
upgraded together, as below.

## Upgrading the vehicle over the tether

The topside is upgraded first, by hand (`hangar-install`, or `apt`). The vehicle is then
upgraded from the console or the command line, with no shell on the vehicle:

```sh
hangar upgrade-vehicle hangar-1.4.0-aarch64.tar.zst
```

1. the topside sends the tarball with `bridge.send` ([08-bridge](08-bridge.md#files)),
   which checks its hash on arrival;
2. it calls `installer.apply` on the vehicle, a method of the `installer` module, which
   runs only while the vehicle is `disarmed` and refuses otherwise;
3. the vehicle's `installer` module runs `hangar-install` on the received file
   (signature verified again on the vehicle), and the manager re-executes;
4. the bridge reconnects at the new version; `hangar check` on the topside confirms
   both sides match.

While step 3 runs, the link is down for the time of the re-execution (under a second);
the vehicle is disarmed throughout, and ArduSub, as a module, keeps running. The
`installer` module is a `Legacy=no` module with one method, allowed to
`topside:pilot_console topside:admin`, and it is the one module that runs as root (with
`Capabilities=` for the installer's needs); it does nothing else. `installer.rollback`
is the same path backwards.

## Debian package

`hangar_<version>_<arch>.deb` (`Source:` and `Vcs-Git:` naming the tagged commit) installs the same tree under `/usr/lib/hangar/<version>/`
and runs `hangar-install` from `postinst` with the role from `debconf` (asked once). An
`apt upgrade` is then an ordinary upgrade; `apt install hangar=<older>` is a rollback.
The package `Recommends` `ffmpeg` and the `v4l2loopback-dkms` module on the topside
profile. Pi OS and Debian hosts use the package; the tarball is for everything else and
for the tether path, which installs a tarball, never a `.deb`, so that one artefact
format crosses the tether.

## Checks

`hangar check` includes: both sides at the same version and `msg/` hash; the signature
of the installed release still valid (`MANIFEST` re-verified against the files); the
previous version present for rollback; the ArduSub parameter file applied
([10-mavlink](10-mavlink.md)).

## Tests

- Installer in a sandbox (a user namespace with the system directories replaced, as
  electric-eye's installer was tested): fresh install, re-run, upgrade, rollback, profile
  change, an edited manifest copy kept, a bad signature refused.
- Upgrade over the tether in the simulation ([14-simulation](14-simulation.md)): both
  sides end at the new version, recordings and parameters unchanged, and the vehicle
  refuses `installer.apply` while armed.
- A `.deb` installed, upgraded and downgraded on a clean Debian container.
