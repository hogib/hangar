# The drivable ArduSub demo

A BlueROV2 in a Gazebo Harmonic world, ArduSub SITL as its autopilot, QGroundControl
on the host as the pilot's console with its on-screen joystick. Gazebo, SITL and the
`ardupilot_gazebo` plugin run in one Docker container with the host's GPU; nothing
but Docker, the NVIDIA container runtime and QGroundControl is installed on the host.

Files: `Dockerfile` (the image), `run.sh` (host side: build, up, tmux, shell, down),
`launch.sh` (inside the container: starts Gazebo and SITL in tmux), `work/` (logs,
created by `up`, not committed).

What is in the image, and why these versions:

| Piece | Pinned at | Verified against |
|---|---|---|
| Ubuntu | 24.04 (Noble) | Gazebo Harmonic has Noble binaries ([install page](https://gazebosim.org/docs/harmonic/install_ubuntu)) |
| Gazebo Harmonic | package `gz-harmonic`, OSRF `ubuntu-stable` repo | the install page's commands, used verbatim |
| ArduPilot | tag `Sub-4.7.2` (commit `fb773567`, `ArduSub V4.7.2`) | `git ls-remote`: on 2026-11-07 the moving tag `ArduSub-stable` pointed at this commit; the last `ArduSub-4.x.y` tag is `ArduSub-4.5.7` (2025-12), Sub now ships in the unified `ArduPilot-4.7` branch |
| ardupilot_gazebo | `main` at `f116b035` | its README (`libgz-sim8-dev`, `GZ_VERSION=harmonic`, the cmake line, the two `GZ_SIM_*` variables); its CI builds on `ubuntu-24.04` with Harmonic |
| BlueROV2 model | [clydemcqueen/bluerov2_ignition](https://github.com/clydemcqueen/bluerov2_ignition) `main` at `4451a5d` (package `bluerov2_gz`, MIT) | its README "BlueROV2 in Gazebo Harmonic": worlds `bluerov2_underwater.world` (frame `vectored`) and `bluerov2_heavy_underwater.world` (frame `vectored_6dof`) |

Neither `ardupilot_gazebo` nor `ArduPilot/SITL_Models` ships a BlueROV2 (checked their
`models/` and `worlds/` on 2026-11-07: iris, zephyr, gimbal; rovers, planes, a
BlueBoat), so the model comes from `bluerov2_gz`, a third-party model (Clyde McQueen's
repository) written for exactly this use. Its models carry the `ArduPilotPlugin`
block (`fdm_port_in` 9002, `lock_step`, the thruster channels) and use Gazebo's own
buoyancy, hydrodynamics and thruster systems. The default here is the Heavy
(`SIM_CONFIG=heavy`, frame `vectored_6dof`), the configuration hangar's own simulator
uses for its parameter defaults ([docs/14](../../docs/14-simulation.md#physics));
`SIM_CONFIG=base tools/sim/run.sh up` gives the six-thruster BlueROV2 with `vectored`.

## Host steps, in order

The state of this machine was read on 2026-11-07; the first three steps are
checks, since most of the work turned out to be done already.

1. **Docker group.** `/etc/group` already lists you in `docker`, but the running
   session does not have the group (`id -Gn` lacks it): log out and in, or for this
   shell `newgrp docker`. If `getent group docker` ever stops listing you, the command
   is `sudo usermod -aG docker $USER`, then re-login. (Anyone in `docker` is root
   equivalent, the Arch wiki's warning; it is the price of `docker run` without sudo.)
   Check: `docker info` prints without a permission error.

2. **NVIDIA container runtime and QGroundControl.** Both are installed already:
   `nvidia-container-toolkit 1.20.1` comes from the official `extra` repository (it is
   not in the AUR; `aur.archlinux.org/rpc` returns nothing for the name), and
   `qgroundcontrol-bin 5.1.5` from the AUR. If either is missing:
   `paru -S nvidia-container-toolkit qgroundcontrol-bin`. The runtime is what makes
   `docker run --gpus all` mount the host driver's OpenGL into the container; QGC is
   the console.
   Check: `pacman -Q nvidia-container-toolkit qgroundcontrol-bin`.

3. **Register the runtime with Docker.** Also done already: `/etc/docker/daemon.json`
   has `"runtimes": {"nvidia": {...}}`. If it did not, the commands from the
   [toolkit install guide](https://docs.nvidia.com/datacenter/cloud-native/container-toolkit/latest/install-guide.html)
   are `sudo nvidia-ctk runtime configure --runtime=docker && sudo systemctl restart docker`.
   The Docker service is `active` but `disabled`, so after a reboot
   `sudo systemctl start docker` (or `enable --now` once).
   Check, once step 1 is done: `docker run --rm --gpus all ubuntu:24.04 nvidia-smi`
   lists the RTX 3060 Ti (the toolkit mounts `nvidia-smi` into the container).

4. **Build the image:** `tools/sim/run.sh build`. It downloads Gazebo Harmonic and its
   development packages, OpenCV and GStreamer, clones ArduPilot with its submodules,
   runs ArduPilot's own `install-prereqs-ubuntu.sh` (a Python venv with MAVProxy),
   builds ArduSub for SITL and builds the plugin. Expect roughly 20 to 40 minutes on
   this machine (12 threads, mostly download and the two compiles) and an image in
   the 8 to 12 GB range; the numbers are estimates, not measured. `docker build`
   caches each step, so a change late in the Dockerfile rebuilds only from there.

5. **Start it:** `tools/sim/run.sh up`. A Gazebo window opens with the BlueROV2 Heavy
   hanging in blue water over a sand seabed, running (the play state; `-r`). In the
   background SITL starts and MAVProxy connects to it; `work/mavproxy.log` shows the
   parameters loading and `work/ardusub.log` the autopilot's own output. SITL waits
   for the Gazebo plugin on UDP 9002 and runs in lock step with it once connected.

6. **Start QGroundControl** on the host. The AUR package (`qgroundcontrol-bin`
   5.1.5) installs the AppImage tree under `/opt/qgroundcontrol` with root-only
   permissions and no command on `PATH`, so it does not launch as installed; once:
   `sudo chmod -R a+rX /opt/qgroundcontrol && sudo ln -sf /opt/qgroundcontrol/AppRun
   /usr/local/bin/qgroundcontrol`. Then `qgroundcontrol` (on Wayland it runs under
   XWayland; `QT_QPA_PLATFORM=xcb qgroundcontrol` if it complains about the display).
   A real gamepad is set up under **Vehicle Setup** (the Q icon → Vehicle Setup →
   **Joystick**), not under Application Settings; that page exists only while a
   vehicle is connected: calibrate, then tick *Enable joystick input*. It listens on UDP 14550 by default and MAVProxy inside
   the container sends there (`sim_vehicle.py` adds `--out 127.0.0.1:14550` itself;
   `--net=host` makes the container's 127.0.0.1 the host's), so within a few seconds
   QGC shows a connected submarine, `Manual` mode, `Disarmed`.

7. **Enable the virtual joystick:** the **Q** icon in the top toolbar →
   **Application Settings** → **General** tab → check **Virtual joystick**
   ([QGC guide, Stable_V5.1](https://docs.qgroundcontrol.com/Stable_V5.1/en/qgc-user-guide/settings_view/virtual_joystick.html);
   the setting is `virtualJoystick` in QGC's `App.SettingsGroup.json`, label
   "Virtual Joystick"). Two thumbsticks appear at the bottom of the Fly view. In QGC's
   source they show for any vehicle when the setting is on; the ArduSub plugin
   declares joystick (MANUAL_CONTROL) support, and QGC's own docs do not restrict the
   feature by firmware, so it should drive SITL. Which stick does what on a submarine
   (expect: left stick heave and yaw, right stick surge and sway) is **unverified**;
   try it.

8. **Arm and drive.** Click the `Disarmed` indicator in QGC's toolbar and arm; if
   ArduSub refuses, the `PreArm:` reason is in QGC's messages and `work/ardusub.log`
   (see Troubleshooting). Move the sticks: in Gazebo the thrusters turn and the
   vehicle moves and turns; depth and attitude in QGC's HUD follow. `Manual` is raw
   pilot input, no stabilization; QGC's mode menu offers ArduSub's `Stabilize` and
   `Depth Hold` if you want to see them (hangar will refuse exactly this, [docs/10](../../docs/10-mavlink.md)).

9. **Look inside** if you want: `tools/sim/run.sh tmux` attaches to the container's
   tmux session, window `gazebo` (Gazebo's log) and window `sitl` (the MAVProxy
   prompt: `arm throttle`, `mode alt_hold`, `param show FS_*`, ...). `Ctrl-b d`
   detaches. `tools/sim/run.sh shell` is a plain shell.

10. **Stop:** `tools/sim/run.sh down` removes the container (the image stays; `up`
    starts clean, with default parameters, every time). Quit QGC.

## Where hangar goes

Today QGroundControl talks to SITL directly through MAVProxy on 14550; there is no
hangar in the loop. When the two `mavlink` modules of [docs/10](../../docs/10-mavlink.md)
exist, the topside module takes 127.0.0.1:14550 for QGroundControl and the vehicle
module talks to SITL on its own port (docs/10 uses 14660; SITL's MAVProxy would be
told `--out 127.0.0.1:14660`, or SITL run without MAVProxy), and QGC's
`MANUAL_CONTROL` becomes `pilot.wrench` → mux → `ctl.wrench` → `MANUAL_CONTROL` to
SITL, arming becomes `safety.arm`. The demo then is the phase-1 test of docs/14 with
one change in `launch.sh` (the output port) and hangar's `sim` profile started beside
it. The Gazebo side enters hangar through the `sim_bridge` module of
[docs/14](../../docs/14-simulation.md#physics): it subscribes to the camera and pose
topics Gazebo publishes over gz-transport (the model would get a camera sensor; the
exact pose topic is to be read off `gz topic -l` when the time comes) and publishes
`cam.raw` and `sim.pose`, so that the vision chain and the recorder see a rendered
vehicle without `sim_vehicle`'s own physics. None of this ships: it is a development
machine's container, and docs/14's position on Gazebo (no ROS tree in hangar) holds;
`ardupilot_gazebo`'s `main` branch and `bluerov2_gz` need no ROS.

## Troubleshooting

- **Gazebo opens no window, or `Unable to create the rendering window` /
  `currentGLContext was specified` in `work/gazebo.log`.** Gazebo's Qt GUI does not
  work on Wayland ([Harmonic troubleshooting](https://gazebosim.org/docs/harmonic/troubleshooting),
  "Wayland issues": Ogre and Qt). `run.sh` therefore sets `QT_QPA_PLATFORM=xcb` and
  passes the XWayland display (`DISPLAY=:1`, `/tmp/.X11-unix`); the Wayland socket is
  passed too but is not what Gazebo uses. On this host `xhost` shows
  `SI:localuser:oguzb` and the container's user is uid 1000, which the X server
  resolves to the same name, so no `xhost` change should be needed; if the log says
  `Authorization required` or `cannot open display`, run `xhost +si:localuser:$USER`
  on the host (expected not to be necessary; **unverified** until the first run).
  Another Gazebo failure mode is an old OpenGL: Ogre 2 wants OpenGL 3.3, preferably
  4.3+; see the next item.

- **Is the GPU in the container?** `tools/sim/run.sh shell`, then `nvidia-smi` (the
  runtime mounts it; the driver here is 615.78.08) and `glxinfo -B` (`mesa-utils`),
  which must name NVIDIA as the OpenGL renderer. If `glxinfo` names `llvmpipe`, the
  driver libraries were not mounted: the image sets
  `NVIDIA_DRIVER_CAPABILITIES=graphics,display,utility,compute` (the variable replaces
  the default `utility,compute`), and `run.sh` passes `--gpus all`; check that
  `docker info` lists the `nvidia` runtime. The error `could not select device driver
  "" with capabilities: [[gpu]]` from `docker run` means the runtime is not registered
  (step 3).

- **`permission denied while trying to connect to the Docker daemon socket`.** Step 1:
  the shell does not have the `docker` group yet.

- **SITL's `--console` and `--map`.** `launch.sh` starts MAVProxy without them (there is
  no terminal to pop them in; the image installs `python3-wxgtk4.0`, which the console
  needs, and the venv sees system packages). To get the console, add `--console` to the
  `sim_vehicle.py` line in `launch.sh` (it is bind-mounted, no rebuild) and `up` again;
  it opens as an X window like Gazebo's. `--map` also needs `matplotlib` in the venv.

- **Port 14550 already taken** (QGC shows nothing, or `Address already in use` in a
  log). Something else on the host is on UDP 14550 (another QGC, a MAVProxy, a
  previous container that `down` did not remove: `docker ps -a`). `ss -lunp | grep 14550`
  names it. SITL's own ports are 5760 (TCP) and 9002 (UDP, the Gazebo plugin), and the
  MAVProxy outputs are `14550 + 10 * instance`, so a second instance would use 14560.

- **ArduSub does not arm.** `work/ardusub.log` and QGC's messages carry the `PreArm:`
  reason. With `-w` every start has default parameters (`sub-6dof.parm` for the Heavy),
  so a parameter changed in QGC is gone after `down`/`up`; edit `launch.sh` to drop
  `-w` if you want them kept across restarts (the eeprom then lives in the container
  until `down`).

- **Switching `SIM_CONFIG`** between `heavy` and `base` needs `down` first (the frame
  and the world are fixed at `up`); `-w` already does what the bluerov2_gz README asks
  for when switching frames.

## Unverified, by name

Everything else in these files was read from the upstream source or page named next
to it. These were not:

- The build time and image size in step 4 are estimates.
- The virtual joystick's axis mapping on ArduSub (step 7), and that QGC 5.1.5's
  virtual joystick drives ArduSub well enough to be pleasant; its presence and the
  menu path are verified.
- That X access from the container needs no `xhost` change (the `SI:localuser` match
  on uid is how the X server works, but this exact combination has not been run).
- The `ubuntu` uid-1000 user in the `ubuntu:24.04` image is from community reports
  (an AskUbuntu question, a VS Code dev-container issue), not Canonical's docs; the
  Dockerfile removes it only if it exists, so either way is fine.
- The Wayland socket pass-through has not been exercised (Gazebo does not use it).
- The `gz fuel download -u <model url>` form: the fuel_tools page shows it with a
  world URL; the Sun model URL answers 200.
