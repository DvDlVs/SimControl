# SimControl

Steering assist (steering only) for Linux: takes your gamepad, publishes a
virtual wheel and filters its input with live game telemetry.

Games: **Automobilista 2**, **Project CARS 2**, **Project CARS 1**,
**Assetto Corsa Evo**, **Assetto Corsa Rally**, **RaceRoom**,
**Automobilista 1**, **rFactor 2**, **Live for Speed**, **Forza Horizon 6**.

## Usage

```bash
cd ~/Projetos/simcontrol-linux
make
./simcontrol --list
./simcontrol
```

Then start the game. Steam Input **off**. Bind **SimControl Racing Wheel**.

AMS2 / PCars 2: controller type Wheel, model **Custom** (not G29).
Shared Memory = Project CARS 2.

No bridge, no second Proton. Config: `simcontrol.conf` or
`python3 ui/simcontrol_config.py`.

To launch everything together with the game (Steam → Properties → Launch
Options):

```
/path/to/simcontrol-linux/scripts/launch-with-simcontrol.sh %command%
```

## Turn OFF every in-game assist

SimControl replaces the game's own steering help — leaving both on makes them
fight each other. Before driving, disable the game's internal aids:

- **Steering assist / auto-steer / stability control**: OFF (all games)
- **Countersteer / "understeer reduction" / "yaw assist"**: OFF — this is the
  one that conflicts directly with SimControl's self-steer
- **ABS / TC / auto-clutch**: personal preference, they don't interfere
- **Speed-sensitive steering / steering lock helpers**: OFF if present
- AMS1/rF2: also check per-car setup/tuning screens (some cars ship with
  driver aids enabled by default)

## Slip angle: differences from the original AGA

The steering math is ported from adam10603's **Advanced Gamepad Assist**, but
one of its inputs cannot be replicated here. The original reads each wheel's
**normalized tire load sensitivity (`ndSlip`)** from AC's shared memory — that
is how it knows whether a tire is *at* its grip peak or *scrubbing past* it,
and it adapts the target slip angle accordingly.

Linux shared memory from the supported games does not expose that value
(rF2/AMS1 offer a partial `gripFract`, everything else offers nothing), so
SimControl works from geometry only (slip angles, yaw rates, speeds). In
practice this means:

- the target slip angle is a fixed tuning value (preset + scale), not
  self-adapting per car/tire like the original
- in slow hairpins (< ~70 km/h) full lock can still scrub the front tires;
  ease steering input there instead of trusting the limiter
- fast-corner behavior is unaffected and matches the original's feel

Per-car presets are the intended compensation: save one preset per car in
the game's preset folder. Forza Horizon 6 also ships factory
`Stock`/`Drift` presets to start from.

## Project CARS 1

Enable **Shared Memory** in Options -> Visual -> Hardware. simcontrol
detects `pCARS.exe`/`pCARS64.exe` automatically and logs
"Project CARS 1 shared memory attached". The reader implements the
official SharedMemory v5 layout (version-checked) with a frozen-map
watchdog, so stale sections from a crashed session are never trusted.

## RaceRoom

Nothing to configure in-game: the native shared memory (`$R3E`, API v3.x) is
always on. simcontrol detects `RRRE.exe`/`RRRE64.exe` automatically and logs
"RaceRoom shared memory attached". If signals feel inverted, adjust
`yaw_sign`/`lat_sign` in the conf.

## Automobilista 1

AMS1 has no native shm: it relies on the community plugin
**[$rFactorShared$](https://github.com/dallongo/rFactorSharedMemoryMap)**
(Dan Allongo, 32-bit).

1. Download `rFactorSharedMemoryMap.dll` and place it in the game's
   **`Plugins/`** folder:

   ```
   <Steam library>/steamapps/common/Automobilista/Plugins/
   ```

   (same folder where `RealFeelPlugin.dll`, `TrackIRPlugin.dll` etc. already
   live — gMotor2 only loads plugins from there, **not** from the install root)
2. Restart the game (plugins load only at boot); simcontrol logs
   "AMS1 shared memory attached ($rFactorShared$)" when it attaches
3. Without the plugin, simcontrol will not attach (by design)

Layout validated against the version anchor at the section start. If
self-steer fights the slide, set `yaw_sign = -1` in the conf.

## rFactor 2

Depends on **[rF2SharedMemoryMapPlugin](https://github.com/TheIronWolfModding/rF2SharedMemoryMapPlugin)**
(The Iron Wolf, 64-bit) — the same plugin Crew Chief uses.

1. Download the DLL (64-bit build) and place it in `Bin64/Plugins/` inside the
   rF2 installation
2. Start the game; simcontrol logs "rFactor 2 telemetry attached"
3. In menus the assist falls back to passthrough by itself (telemetry freezes
   outside a session)

The reader uses `vehicles[0]` from the map — correct offline; for multiplayer
slots other than 0, ask to have the scoring map added.

## Live for Speed

Pure UDP game (no shared memory). It streams two packs set in `cfg.txt`
(Options → Misc has equivalents in-game): **OutSim** for physics and
**OutGauge** for the car model code (used as the preset key) and RPM.

1. In `cfg.txt` set (then restart LFS):
   ```
   OutSim Mode 2
   OutSim IP 127.0.0.1
   OutSim Port 26001
   OutGauge Mode 2
   OutGauge IP 127.0.0.1
   OutGauge Port 26000
   ```
2. Optional: point the ports in `simcontrol.conf` (`lfs_outsim_port` /
   `lfs_outgauge_port`) at whatever LFS uses
3. Drive; simcontrol logs "LFS OutSim telemetry live"

OutGauge only streams while the camera is inside the car — in external views
the car falls back to the generic `LFS` preset instead of the model code.
Signs follow the rFactor 2 convention (`yaw_sign`/`lat_sign`/`fwd_sign` in
the conf correct any inverted steering, as with the other sources).

## Forza Horizon 6

Pure UDP input, same shape as LFS. In-game enable
**Settings → Difficulty → Data Out**: `ON`, IP `127.0.0.1`, port `5300`
(any port — point `fh6_out_port` in `simcontrol.conf` at it; `0` disables).

The reader parses the Forza "Data Out" 324-byte packet. `IsRaceOn` drives
`playing` (latched, since FH6 flickers it mid-drive), `EngineCurrentRpm`
feeds the HUD, and `CarOrdinal` is the per-car preset key (`FH6-<ordinal>`
folder). The velocity is already in the car frame (X right, Y up, Z
forward) and is mapped into the same rFactor 2 sign convention as every
other source (`local_vx` left+, `local_vz` back+, `ang_y` yaw rate left+).

The FH6 preset folder ships with two factory presets: `Stock` (the
validated FH6 stock feel) and `Drift`, a variant tuned for drift cars
from the validated FH6-3249 tune — it trims the self-steer headroom
(`max_self_steer_angle` ~3°) with slightly more damping, because FH's
automatic countersteer saturates hard even at weak settings. To apply
`Drift` to a car, import `Drift.json` in the panel and hit Save; per-car
presets then override both as usual.

`fh6_out_port = 0` switches the block fully off.
