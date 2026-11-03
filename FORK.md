# OneFamily — multi-DualSense fork

Fork of [ChiricoG/OneFamily](https://github.com/ChiricoG/OneFamily) (MIT) that lets
**more than one DualSense play at once over Bluetooth**, each as its own player or
guest, instead of every controller driving Player 1 together.

No code from Control4Free is used, so this fork stays MIT. See
[Why not merge Control4Free](#why-not-merge-control4free).

Reported upstream as
[ChiricoG/OneFamily#29](https://github.com/ChiricoG/OneFamily/issues/29). That
repository has pull requests disabled, so this fork is where the work lives; the
findings are in the issue for the maintainer to take or leave.

> **Applying this as patches** (`git format-patch` / `git am`) on a machine with
> `core.autocrlf=true`: `.gitattributes` arrives in the same commit as
> `tools/build_daemon.sh`, so git has already written the script by the time it
> starts honouring `eol=lf`, and the CRLF shebang makes it unrunnable in the
> container. `git checkout -- .` after the `am` fixes it. Cloning the fork is
> unaffected — the attributes are in the index before anything is checked out.

## What upstream did, and where it stopped

Upstream already solves the hard part. The PS4's `bthid` module carries a
hardcoded allowlist of VID/PID pairs, and the DualSense (`054C:0CE6`) is not in
it, so pairing is refused before any driver is involved. OneFamily patches that
allowlist from ring 0 (`SYS_KEXEC`), registers the DualSense with the Bluetooth
driver (`ioctl 0x8004622B`), reads its HID reports off `/dev/bt`
(`ioctl 0x80206233`), translates them into a DualShock 4 sample, and injects that
into a **virtual DS4** created through `scePadVirtualDevice*` — the same API
Remote Play uses, which is why it works on the XMB and in every game.

The Bluetooth layer was never the limit. Each `/dev/bt` event already carries the
**Bluetooth address of the controller that produced it**, and the report ioctl is
already addressed per device. What capped it at one DualSense was the layer above:

| Where | What it did |
|---|---|
| `daemon/include/vpad.h` | `VPAD_MAX_SLOTS 2` — a hard ceiling of two virtual pads |
| `daemon/src/vpad.c` | `vpad_slot_open()` reused a slot **by source** (USB/BT), so a second DualSense was handed the first one's pad |
| `daemon/src/bt_driver.c` | single globals `s_dualsense_connected`, `s_bt_vpad_slot`, `s_physical_address`, `s_last_input_time` |
| `daemon/src/bt_driver.c` | rumble and light bar went to the one `s_physical_address` |

So two DualSense moved the same player, and only one could be given an account.

## The registration limit

Raising the slot ceiling was not enough on its own, and the reason turned out to
be one line of upstream's Bluetooth setup.

`ioctl 0x8004622B` on `/dev/bt` tells the Bluetooth driver to route devices
matching a VID/PID to that descriptor. Upstream calls it **once**. The driver
keeps **one device entry per successful call**, so with a single registration the
second DualSense got no route: it paired, the console listed it, and the system
even created an MBus device for it —

```
sceMbusEvent (eventId=1 ADD, deviceId=0x31001)
#LOGIN MGR# Receive Event : SCE_MBUS_EVENT_DEVICE_ADDED [DeviceId:0x31001][type:1][subType:18]
```

— and then nothing. No `DeviceQueueMgr::Enqueue`, no `QueueEventTypeSelectUser`,
so no *"Who's using this controller?"* screen, and not a single `/dev/bt` event
for its address, so the daemon never knew it existed. Instrumenting every raw
event (`-DONEFAMILY_BT_DIAG`) showed 7,271 events from one address and zero from
any other, which is what ruled the daemon out and pointed at the driver.

Registering the same pair again returns `0`, and each registration admits one
more controller. On FW 12.52, with four registrations:

```
register dualsense (054C:0CE6)        = 0
register dualsense (slot) (054C:0CE6) = 0
register dualsense (slot) (054C:0CE6) = 0
register dualsense (slot) (054C:0CE6) = 0
DualSense BT: new device 0x48188DF2DA18 (entry 0)  -> Slot 0 -> Player 1
DualSense BT: new device 0xAC361B29DF1C (entry 1)  -> Slot 1 -> Player 2
```

So `ONEFAMILY_BT_REGISTRATIONS` (default `BT_MAX_DEVICES`) registrations are
taken at startup and given back one for one on shutdown. The kernel allowlist
patch governs whether a DualSense may *pair*; this registration count governs
how many may be *delivered*. The two are separate limits, and upstream only
lifted the first.

The Edge's own PID (`054C:0DF2`) is refused on this firmware
(`0x802F3701`), so registering it is off by default behind
`ONEFAMILY_BT_REGISTER_EDGE` rather than logging an error on every boot.

## What this fork changes

1. **`VPAD_MAX_SLOTS` 2 → 4**, the PS4's own four-player ceiling.
2. **Slots are keyed by physical device, not by source.** New
   `vpad_slot_open_dev(source, device_key, &slot)` matches on
   `(source, device_key)`, the key being the controller's Bluetooth address.
   `vpad_slot_open()` stays as a wrapper with key `0`, so the USB path is
   untouched.
3. **Per-device state in `bt_driver.c`.** A `BtDevice s_devices[BT_MAX_DEVICES]`
   table keyed on the Bluetooth address replaces the globals, holding each
   controller's virtual pad, connection flag, last-input time and output-report
   sequence tag. `bt_dev_acquire()` claims an entry the first time an address is
   seen, and reports go to that entry's pad only — the old fallback to
   `vpad_send_data()` is gone, since with several pads open it would have picked
   an arbitrary slot and moved the wrong player.
4. **Per-device disconnect timeout.** `bt_check_timeouts()` expires each
   controller on its own 3-second clock, so one player setting a pad down no
   longer drops everybody else. An entry that is claimed but never produces a
   decodable report is reclaimed quietly.
5. **Per-device feedback.** `bt_driver_send_rumble_slot(slot, …)` addresses the
   controller that owns that virtual pad. The old `bt_driver_send_rumble()`
   remains for callers with no slot in hand, such as the companion app's
   vibration test, and targets the first connected controller.
6. **One Bluetooth registration per controller**, as above — the change without
   which none of the rest is reachable.
7. **A controller's pad survives a dropout, and so keeps its account.** Upstream
   deleted the virtual pad the moment a controller went quiet. The PS4 binds an
   account to the *device*, so the pad that came back was a new one with no
   owner, re-opening *"Who's using this controller?"* — and an answer given on
   the wrong controller silently moved the guest's pad to the main user. Now a
   quiet controller keeps its pad for `BT_PAD_GRACE_US` (60 s) while a neutral
   sample goes out so a drop mid-press cannot leave a button held, and the same
   Bluetooth address returning reattaches to the same pad with no picker at all.
8. **Liveness no longer depends on a report being decodable.** `last_input_time`
   was only refreshed when the decoder used a whole report, so a run of reports
   it skipped — an unexpected length, a failed CRC, a reserved D-pad value —
   expired a controller that was transmitting perfectly well. That destroyed its
   pad and its account, and recreated both seconds later: the cause of pads
   churning through handles and of the behaviour feeling random. A valid pad
   report for an address now marks that controller alive whatever the decoder
   then makes of its contents, and `decode_drops` is counted and logged.
9. **The daemon log carries timestamps.** Relative seconds since the daemon
   started. Without them the gap between two lines said nothing about elapsed
   time, which is the only question that matters when asking whether a
   controller expired too early.
7. **Diagnostics behind `-DONEFAMILY_BT_DIAG`.** The normal path drops any event
   whose type falls outside `0x18..0x20`, and any report that fails validation,
   in silence — so a log showing one controller could not distinguish "no event
   ever arrived" from "events arrived and were discarded". The diagnostic build
   accounts for every raw event per Bluetooth address, gives the report ioctl's
   verdict with the reason for each rejection, and prints a tally every 5 s. It
   is what found the registration limit, and it is compiled out of the normal
   build entirely.
8. **Build.** `build_daemon.ps1` took its toolchain from one developer's
   hardcoded path. It now reads `OO_PS4_TOOLCHAIN`, and `docker/Dockerfile` plus
   `tools/build_daemon.sh` build the payload with nothing installed on the host
   but Docker. `EXTRA_CFLAGS` and `OUT_NAME` let a diagnostic build sit beside
   the normal one.

User assignment needed no change: upstream already pulses the PS button per slot,
so each new pad raises the PS4's own *"Who's using this controller?"* screen,
which is where the second account or **Guest** is chosen.

## Building

```sh
docker build -f docker/Dockerfile -t onefamily-build:llvm18 docker/
docker run --rm -v "$PWD:/src" onefamily-build:llvm18 /src/tools/build_daemon.sh
```

The payload lands at `bin/onefamily.bin`.

On a host that already has the OpenOrbis toolchain:

```powershell
$env:OO_PS4_TOOLCHAIN = "C:/OpenOrbis/PS4Toolchain"
./build_daemon.ps1
```

## Installing

The companion PKG only *deploys* the payload, so no PKG rebuild is needed:
replace the payload and restart it.

1. Keep the OneFamily companion app that is already installed.
2. FTP `bin/onefamily.bin` over `/data/payloads/onefamily.bin` on the console.
3. Restart the payload by **rebooting the console**, so `[AutoRun]` loads it.

> **Do not open the companion app while this payload is installed.**
> `app/src/main.cpp` calls `Installer::isPayloadUpToDate()` on launch, which
> compares the installed payload against the one bundled in the PKG byte for
> byte. Any custom build therefore reads as out of date, and the app runs
> `Installer::runInstallation()` immediately — no prompt — overwriting
> `/data/payloads/onefamily.bin` with the bundled v1.2.1 payload and quitting.
> That is also the quickest way to undo this install deliberately.
>
> For the same reason, do not restart the payload through GoldHEN's PayLoader
> while the old daemon is still running: `/dev/bt` takes a single reader, and two
> daemons would fight over it and register duplicate virtual pads. Reboot
> instead.
4. Pair each DualSense the normal way: hold **Create + PS** until the light bar
   blinks, then *Settings › Devices › Bluetooth Devices*.
5. Read `/data/onefamily/onefamily.log`. One pair of lines per controller is
   expected: `DualSense BT: new device 0x…`, then
   `Virtual DS4 Slot 1 (Bluetooth, key=0x…) … -> Player 2`.

To roll back, FTP `bin/onefamily-upstream-rollback.bin` (pristine upstream, built
with the same toolchain) over `/data/payloads/onefamily.bin`. Relaunching the
companion app also redeploys its own bundled payload.

## Status

Tested on a console on firmware 12.52: two DualSense over Bluetooth, the second
signed in as **Guest**, playing a game together. The log for a healthy two-pad
session reads:

```
DualSense BT: 4 of 4 registrations held -> room for 4 simultaneous controllers
DualSense BT: new device 0x…  (entry 0)
Virtual DS4 Slot 0 (Bluetooth, key=0x…) … -> Player 1
DualSense BT: new device 0x…  (entry 1)
Virtual DS4 Slot 1 (Bluetooth, key=0x…) … -> Player 2
```

Three and four controllers are untested; the registrations and the slots are
there for them.

Which controller becomes Player 1 follows **connection order**, not identity, so
it can swap between sessions. That matches how the console behaves with its own
pads.

If a second pad ever fails again, the log says which stage broke:

- fewer registrations held than asked for → the driver ran out of device entries;
- registrations fine but no second `new device` line → nothing is reaching
  `/dev/bt`; rebuild with `-DONEFAMILY_BT_DIAG` to account for every raw event,
  and capture the kernel log from GoldHEN's klog server on TCP 3232 to see what
  the driver and LOGIN MGR make of the device;
- `new device` but no `Virtual DS4 Slot` line → `scePadVirtualDeviceAddDevice`
  refused another pad; see the ABI note below;
- both lines but nothing moves → the pad exists but was never assigned an
  account on the *"Who's using this controller?"* screen.

## Known limitations

- **The system screen does not switch to a guest when both controllers are
  DualSense.** Two DualSense play together fine, each on its own account, but
  pressing PS on the guest's controller never brings up that user's screen, and
  the console's own *"press the PS button on the other controller"* prompt goes
  unanswered. Everything observable about that pad is correct:

  ```
  owner_watch: Slot 1 (handle=0x8030C) is MBus DeviceId 0x8030C
  owner_watch: Slot 1 (Player 2, MBus 0x8030C) belongs to 'Convidado1' (0x20B7C46E) [owner_changed]
  vpad_send_data_slot(1): PS Button ACTIVE (btn=0x00010000, ret=0x00000000)
  ```

  The pad exists, the kernel says the guest owns it, the press is held for
  200-300 ms and `libScePad` accepts it — and `ShellCore` either does nothing or
  brings up the *main* user's screen. The same press on the same kind of pad
  *does* switch the screen when the other controller is a real DualShock 4, so a
  virtual pad is not the problem: two virtual pads are.

  Five explanations were tested against the console and none survived:

  | Idea | Outcome |
  |---|---|
  | The guest's pad is really bound to the main user | No: the kernel logs `OWNER_CHANGED … 'Convidado1'` |
  | The controllers were swapped in the tester's hands | No: all six PS presses were on the guest's slot, and the slots are keyed to Bluetooth addresses |
  | The PS press is too short for `ShellCore` to latch | No: 200-300 ms, the same range as presses that work |
  | The automatic PS pulse on pad creation interferes, as Control4Free's docs warn | No: `auto_ps_pulse = 0` changed nothing, and both users still signed in |
  | Upstream declares a 16-byte `AddDevice` param where 32 is right, leaving the pads indistinguishable | No: `vda_param_size = 32` is accepted and returns the same handles |

  Nor is the failure observable from here. The shell resolves a user's pad in its
  own process, and neither `scePadGetHandle(userId, 0, index)` nor
  `scePadOpen` + `scePadGetDeviceId` reports anything from a payload that
  differs between the working user and the broken one — see the inconclusive
  `vpad_pad_probe()` and the reasoning kept with it. Going further means reverse
  engineering `ShellCore`, so this is recorded rather than fixed.

  **DualSense as the guest plus a real DualShock 4 works completely**, including
  the screen switch. Connect the DualSense first: the power-off below only fires
  when the first virtual pad opens, so a DualShock connected afterwards survives.

- **Connecting a DualSense powers off an already-connected DualShock 4.**
  Upstream's `vpad_poweroff_physical_ds4()` disconnects the physical DS4 of every
  logged-in user before the first virtual pad is created. Intentional upstream;
  connect the DualSense first if you want to keep a DS4.

- **Two DualSense contend for the Bluetooth radio.** Each one's input stream goes
  silent for seconds at a stretch — ~3 s typically, sometimes tens of seconds —
  with `decode_drops` at zero, so the reports are not arriving at all rather than
  arriving and being discarded. Replacing one DualSense with a DualShock 4 makes
  the remaining one steady. Nothing in a payload can schedule the console's radio,
  so the timeouts here are set to tolerate the gaps rather than treat them as a
  disconnect. `enable_rumble = 0` in `onefamily.ini` is the one lever available:
  rumble is the only Bluetooth *output* traffic this daemon produces.

- **Use the default `AUTO` mode.** `MERGE` and `SECONDARY` force an account onto
  `g_vpad_handle`, which upstream left as a single-pad global pointing at the
  most recently opened slot. `AUTO` leaves assignment to the PS4's own picker,
  which is what multiple controllers need anyway.
- **The companion app's UI still shows one controller.** The daemon handles
  several; the app's tester, calibration and remap screens were not extended.
- **The USB path is still one pad**, keyed `0`, as upstream had it. A second USB
  controller would need a key per USB device — the same change, one layer over.
- **DualSense Edge** (`054C:0DF2`) is still not registered with the Bluetooth
  driver; upstream only registers `0CE6`.

## Why not merge Control4Free

[Control4Free](https://github.com/MoHadiShibli/Control4Free) does run four pads,
but its network layer, a WebRTC page on port 4264, is only how input arrives from
a phone — it has nothing to do with the controller count. Both projects already
reach the PS4 the same way, through `scePadVirtualDevice*`; Control4Free simply
allocated four slots. So the work here was to unblock OneFamily's slots, not to
graft anything on.

Two further reasons to keep them apart:

- **Licence.** OneFamily is MIT, Control4Free is GPL-3.0, its virtual-device code
  deriving from seregonwar/SplashDown. Copying code across would relicense this
  fork.
- **They disagree about the ABI of `scePadVirtualDeviceAddDevice`.** Control4Free
  calls it as `AddDevice(void *param, int32_t deviceType = 3)` with a 32-byte
  param and recovers the handle from `/dev/klog`, because on its console the call
  returns a status rather than a handle. OneFamily calls it as
  `AddDevice(param*, int *out_handle)` with a 16-byte param and `type = 1`.

  **On FW 12.52, OneFamily's reading is the correct one, and its `out_handle` is
  the MBus DeviceId itself.** The owner watcher settled it by printing both:

  ```
  owner_watch: Slot 0 (handle=0x6030D) is MBus DeviceId 0x6030D
  owner_watch: Slot 1 (handle=0x8030C) is MBus DeviceId 0x8030C
  ```

  So there is no second namespace to translate between, and no need for the klog
  technique to recover a handle. (An earlier reading of a kernel log here guessed
  the opposite, from a `DeviceId:0x90300` that belonged to a pad from another
  session.) The watcher is still worth having, because the *owner* of a pad
  appears nowhere but the kernel log.
