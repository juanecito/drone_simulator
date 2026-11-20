# Flight Model

How Drone Simulator flies an aerial drone (milestone M3): the coordinate frames, the physical
models, the onboard flight controller, the reference profiles and how the model was validated.
All code lives in `src/core/physics`, `src/core/weather` and `src/core/sim`. It has no GUI or
OpenGL code and runs headless.

Everything is implemented from textbook physics and the control-engineering literature. No
code from flight-control firmware (Betaflight, INAV, ArduPilot, PX4) is used.

## 1. Frames and conventions (`frames.hpp`)

| Frame | Axes | Use |
|---|---|---|
| World | right-handed, `x` = East, `y` = Up, `z` = South (−North); `double` | terrain, graphics, physics |
| Body | aerospace FRD: `x` forward, `y` right, `z` down | forces, torques, sensors |

- The orientation `q` rotates body vectors into the world frame.
- Euler angles (display, Angle mode) use the NED sequence `R = Rz(ψ)·Ry(θ)·Rx(φ)`:
  - yaw ψ is clockwise from north;
  - pitch θ is positive nose up;
  - roll φ is positive right side down.
- Body rates `p, q, r` are positive for rolling right, pitching the nose up and yawing the
  nose right.
- The level attitude is `orientationFromEuler({})`, **not** the identity quaternion (the body
  and world axes differ).

## 2. Simulation loop (`sim/`)

- **Timing:** fixed step, 500 Hz by default (240–2000 Hz allowed), on its own thread. Time
  scale 0.1–4×, locked to 1× during missions.
- **Order of one tick** (FR-LOOP-05):
  environment → sensors + estimator → flight controller → RK4 integration → collisions →
  events.
- **Output:** the GUI reads triple-buffered snapshots. The renderer extrapolates the pose by
  at most 50 ms to the frame time.
- **Determinism:** the same configuration, seed and input sequence give the same flight
  (FR-X-05).

## 3. Rigid body (`rigid_body.hpp`)

Six degrees of freedom, integrated with classical RK4. The forces and torques are evaluated
four times per step.

- `m·v̇ = F_world` (gravity 9.80665 m/s², rotor thrust, rotor H-force, body drag, ground
  contact).
- `I·ω̇ = τ − ω × I·ω` (diagonal inertia tensor in the body frame).
- `q̇ = ½·q ⊗ (0, ω)`, renormalised after every step.
- **NaN guard (FR-PHY-17):** a non-finite state resets the vehicle and publishes
  `NumericalResetEvent`.

## 4. Powertrain (`powertrain.hpp`)

### Motor (brushless DC)

- **Motor equation:** `V·d = ω/Kv + R·(I0 + Q(ω)/Kt)`, with `Kt = 1/Kv` (SI) and ESC duty `d`.
- **Speed:** ω is solved per motor every tick, then lagged by the rotor inertia (motor plus
  propeller).
- **Current:** each motor draws `min(demand, (d·V − ω/Kv)/R + I0)`.
- **Battery solve:** the battery current is found self-consistently by bisection on the
  terminal voltage `V`, because the motor current depends on `V`, and `V` on the total
  current.

### Propeller

- **Static thrust and torque:** `T = C_T·ρ·n²·D⁴` and `Q = C_Q·ρ·n²·D⁵` (n in rev/s).
- **Forward flight:** both coefficients fall with the advance ratio `J = V/(n·D)`; thrust
  falls linearly to zero at `J₀` (`zeroThrustAdvanceRatio`).
- **H-force (rotor drag):** `H = −k·T·μ` in the rotor plane, with `μ = V∥/(ω·R)`. At hover
  it acts as a linear drag `d ≈ k·g/(ω·R)` on the velocity. The reference profiles use
  `k = 1.5` (`rotorDragCoefficient`), so `d` ≈ 0.2–0.3 s⁻¹, inside the 0.1–0.6 s⁻¹ measured
  on real quadrotors (Faessler 2018, Svacha 2017). The body drag areas are fitted so that,
  together with the rotor drag, the top speeds match the references (§9).
- **Ground effect:** `T / (1 − (R/4z)²)` (Cheeseman–Bennett), capped.
- **Vortex ring state:** up to 25 % thrust loss when descending at about the hover induced
  velocity `v_h = √(T/(2ρA))`. There is no loss below `0.3·v_h` or above `2·v_h`.

### Battery (equivalent circuit)

- **Terminal voltage:** `V = N·(OCV(SoC, T) − V_RC) − I·R0(T)`. `R0` follows an Arrhenius law
  in temperature (Ea/R = 2257 K: twice the 25 °C value at 0 °C), and one RC branch (τ = 15 s,
  half of `R0` at 25 °C) models the slow polarisation of charge transfer and diffusion, which
  slows much more in the cold (Ea/R = 6000 K for LiPo, 5000 K for Li-ion).
- **Charge:** coulomb counting. The usable capacity falls in the cold (LiPo: 60 % at −20 °C,
  75 % at −10 °C, 85 % at 0 °C; Li-ion slightly better).
- **Cold weather (NFR-REAL-11, FR-WX-08):** the pack starts at the air temperature of the
  weather. Discharged at 0.5 C to 3.0 V per cell, a 5 Ah LiPo cell and a 2 Ah Li-ion cell
  deliver their capacity, and hold their mid-discharge voltage, within 10 % and 3 % of the
  reference curves in `tests/fixtures/battery/temperature_curves.csv`
  (`Battery.ColdDischargeFollowsTheDatasheetCurves`). Those curves are representative values
  of the "discharge at different temperatures" graphs of generic LiPo pouch and 18650
  datasheets, not of one product; compare them with the datasheet of the cells a new profile
  models.
- **Temperature:** a lumped thermal model (Joule heating, convective cooling to the air).
- **Chemistry:** LiPo and Li-ion open-circuit voltage curves.
- **Warnings:** low and critical, by loaded cell voltage and state of charge.
- **Depleted:** at SoC = 0, or when the loaded voltage stays at the ESC cut-off for 0.5 s. A
  spin-up current spike does not latch "depleted".

## 5. Air and aerodynamics

### Atmosphere (`atmosphere.hpp`)

- **Temperature:** ISA lapse rate from the ground temperature set in the Environment tab.
- **Pressure:** ISA barometric formula from QNH, so a barometer set to QNH reads the true
  altitude in an ISA profile.
- **Density:** moist air, using the humidity.
- **Accuracy:** NFR-REAL-03 is checked against ISA tables in `physics_test.cpp`.

### Body drag (`aerodynamics.hpp`)

`F = −½·ρ·C_dA_i·|v_i|·v_i` per body axis, with `v` = vehicle velocity minus wind.

### Rain (FR-WX-06)

- **Water on the airframe:** in drizzle, rain or sleet a film of up to 0.1 mm (thinner below
  4 mm/h, `∝ √R`) covers a wetted surface taken as twice the sum of the drag areas: 28 g on the
  survey quad, 7 g on the camera drone. It builds up with a 60 s time constant and is blown off
  with 300 s once the rain stops; it is carried at the centre of mass.
- **Drops:** the falling precipitation hits the airframe with its momentum flux
  `ρ_w·|v_rel|·v_rel·A`, where `ρ_w = R / v_fall` is the water in the air, `v_rel` the drops'
  velocity (fall speed plus wind) relative to the vehicle and `A` the drag area across
  `v_rel`. Fall speeds: drizzle 2 m/s, rain 5–8 m/s growing with the rate, sleet 4 m/s, snow
  1 m/s. Even heavy rain pushes with well under 1 % of the weight: rain matters through the
  water's weight, the wet ground (ground drones) and the visibility, not through drag.

### Wind (`wind.hpp`, `weather/wind_levels.hpp`)

- **Levels:** 11 levels (0–10) following the Beaufort scale, read from
  `assets/environment/wind_levels.json` (SPECIFICATION §4.2.1).
- **Mean wind:** logarithmic profile `U(h) = U10·ln(h/z₀)/ln(10/z₀)`, with `z₀` = 0.03 m and
  the speed held constant above 300 m. The direction slowly wanders within the level's swing.
- **Turbulence:** Dryden filters (MIL-F-8785C intensities, scaled by the level), advanced
  along the vehicle's path.
- **Gusts:** discrete "1 − cos" gusts up to the level's gust peak, at random intervals around
  the level's mean interval.
- **Seeding:** every quantity is seeded from the session seed, so a given seed gives the same
  wind.

## 6. Sensors and estimator (`sensors.hpp`, `estimator.hpp`)

### Sensors

| Sensor | Model |
|---|---|
| Gyro | white noise, turn-on bias, bias random walk |
| Accelerometer | white noise, bias, motor vibration (averaged over the tick to avoid aliasing) |
| Barometer | 50 Hz, noise; pressure altitude against QNH |
| GPS | 10 Hz, slowly varying position error (1σ ≈ 1.2 m horizontal, 2.5 m vertical), velocity noise, about 25 s to the first fix after power-on |
| Magnetometer | true heading with noise (no declination or deviation yet; the HUD compass gets the declination from the WMM2025 model in `core/geo`) |

### Estimator

- **Attitude:** a complementary filter. It integrates the gyro and corrects towards gravity
  with gain 0.25 s⁻¹. In flight it also learns the gyro bias slowly (integral gain 0.002).
- **Accelerometer filter and trust:** the accelerometer is low-passed at 2 Hz (in the world
  frame, so that rotating the body does not lag it), as flight controllers do. Without the
  filter, motor vibration fails the magnitude check most of the time. The accelerometer is
  then trusted fully when `|a|` equals the expected magnitude, falling to zero at ±0.1 g
  (ArduPilot's DCM: ±0.5 g). With a GPS fix, the GPS-derived acceleration (smoothed, at
  most 0.5 s old) is removed from the reference first, so steady turns and acceleration do
  not tilt the horizon.
- **Gyro calibration:** while the vehicle is disarmed, still and on the ground, the gyro bias
  is the running mean of the readings since it came to rest (later, the mean of the last
  2 s), as flight controllers calibrate before arming.
- **Altitude and climb rate:** from the barometer, plus the GPS position and velocity once
  there is a fix.
- **Without a GPS fix:** the accelerometer of a multirotor reads mostly thrust, so it sees
  the tilt only through the rotor drag on the velocity. While the drone brakes, that drag
  reads as tilt, as on a real flight controller. After a full bank in Angle mode the drone
  leans a few degrees against its motion (at most 7°, the racer from 12 m/s), then settles
  level within about 25 s, with no sustained oscillation or drift (NFR-REAL-04, §10).

## 7. Flight controller (`flight_controller.hpp`)

### Rate control

- **Rate curves:** per axis, with a centre rate, a maximum rate and expo.
- **PID:** cascaded. In Angle mode the angle loop feeds the rate loop. The D-term acts on the
  measurement and is low-pass filtered.
- **Mixer:** X-quad with airmode, so the craft keeps attitude authority at zero throttle.
  The idle command is 5 %.

### Flight modes

- **Acro:** the sticks command body rates.
- **Angle:** the sticks command tilt, up to the profile's limit (55° for the racer).
- **Altitude hold:** the throttle stick commands the climb rate. Centre holds the altitude,
  full stick climbs at `maxClimbMs`, zero stick descends at `maxDescentMs`.
- **Position hold (mode 4):** altitude hold plus GPS position hold, all from the estimate
  (the controller never sees the true position). Pitch and roll sticks command a horizontal
  speed in the heading frame, up to `maxHorizontalSpeedMs` (8 m/s). Released, the drone
  brakes to under 0.3 m/s, takes that point as its target and holds it:
  `v = positionGain · (target − position)`. A velocity PI loop (`velocityP`, `velocityI`;
  the integrator learns the wind) gives the acceleration, and the acceleration gives the
  tilt, `θ = atan(a / g)` in the heading frame. The acceleration is limited by the Angle-mode
  tilt limit, so a wind beyond the profile's capability blows the drone away (FR-WIND-05/06).
  The integrator stops while the limit is reached. Without a GPS fix the mode flies as
  altitude hold and the HUD says so.
- **Return to home (RTH switch, key `H`):** home is the estimated position at arming, or at the
  first GPS fix after it. The return has three stages:
  1. *Climb:* hold the position and turn the nose towards home while climbing to
     `rthAltitudeM` (30 m) above the arming altitude (or stay higher if already above). Within
     10 m of home the climb is skipped.
  2. *Return:* fly home at `rthSpeedMs` (8 m/s), slowing down over the last few metres.
  3. *Land:* within 1.5 m of home, descend at the landing speed, hold the position over home,
     and disarm 1 s after touching down.

  Turning the switch off gives control back to the selected mode. Without a fix or a home the
  switch is ignored (HUD warning).

### Arming and failsafe

- **Pre-arm checks:**
  - throttle low (at or below centre in the hold modes);
  - tilt under 25°;
  - battery not critical, not crashed;
  - RC link present, the arm switch cycled since start-up and the RTH switch off;
  - a GPS fix when Position hold is selected.
- **Failsafe:** 1 s after the RC link is lost the craft returns home with RTH if it has a
  GPS fix and a home; otherwise it lands where it is at 1.5 m/s. When the link comes back, the
  pilot has control again.

## 8. Contact and crashes

- **Contact points:** the landing feet, the propeller tips and the top of the frame. They
  use penalty spring–damper normal forces and regularised Coulomb friction against the
  terrain under the vehicle.
- **Crash:** a hit faster than `crashSpeedMs` normal to the ground (6 m/s for the racer,
  3 m/s for the camera and survey drones).
  The motors stop, and the vehicle must be reset.
- **Water:** landing in water is a crash.

## 9. Reference profiles (`assets/vehicles/aerial/`)

Generic drones, not modelled on any commercial product. Their figures are typical published
values for the class. The Vehicle tab shows the computed figures; `flight_test.cpp` checks
that they agree with the references to within NFR-REAL-01 tolerances.

| Profile | Mass | Battery | Prop | Hover throttle (model / ref.) | T/W | Hover time | Top speed | Max wind level |
|---|---|---|---|---|---|---|---|---|
| `fpv-5in-racer` | 650 g | 6S 1100 mAh LiPo | 5.1″ tri-blade, 1900 Kv | 0.237 / 0.24 | 8.45 / 8.5 | 8.3 / 8.0 min | 30.8 / 30 m/s | 8 |
| `camera-250g` | 249 g | 2S 2000 mAh Li-ion | 4.7″, 2700 Kv | 0.477 / 0.47 | 2.94 / 2.9 | 24.8 / 24 min | 16.6 / 16 m/s | 5 |
| `survey-1500g` | 1.5 kg | 4S 5000 mAh LiPo | 10″, 740 Kv | 0.527 / 0.52 | 2.60 / 2.6 | 20.2 / 20 min | 18.4 / 18 m/s | 5 |

- **Drag:** rotor drag `k = 1.5` on all three. Body drag areas (front / side): racer
  0.011 / 0.0035 m², camera 0.005 / 0.0035 m², survey 0.02 / 0.011 m².
- **Top speed:** the level speed at the profile's tilt limit, computed in steady state. In
  simulated flight (altitude hold, with a GPS fix) the drones reach 31.1, 17.2 and 18.7 m/s.
- **Maximum wind:** the strongest steady wind each drone holds position in, nose into the
  wind (tilt limit and 85 % of full command): 30.8, 15.1 and 16.1 m/s (the Vehicle tab shows
  the tilt it takes). The side drag areas are smaller, so side-on the drones need less tilt.
- **Max wind level:** the highest level whose gust peak the drone can still hold position
  against within its tilt limit and 85 % of full command (FR-WIND-07).

## 10. Validation

The checks run in `ctest`. The most relevant tests:

| Requirement | Test |
|---|---|
| Free fall and drag (NFR-REAL-02) | `physics_test.cpp` |
| ISA atmosphere (NFR-REAL-03) | `physics_test.cpp` |
| Reference figures (NFR-REAL-01) | `flight_test.cpp`: static and dynamic hover throttle and current, test-stand thrust-to-weight, top speed in flight |
| Holding against wind (NFR-REAL-08) | `flight_test.cpp`: forces balance on a pinned test stand at the analytical tilt and command; in closed-loop Position hold in a steady wind, the mean tilt and power match `windHoldState()` within ±10 % (table below) |
| Wind statistics | `wind_test.cpp`: 60 s mean speed per level, Dryden σ, gust occurrence, determinism |
| Battery in the cold (NFR-REAL-11) | `physics_test.cpp`: delivered capacity and mid-discharge voltage vs. temperature |
| Fog and visibility (NFR-REAL-11) | `graphics_test.cpp`: near the ground, the renderer's fog, aerosol and Rayleigh extinction bring a dark object to 2 % contrast at the weather's visibility, within ±10 % (Koschmieder), for every weather preset |
| Rain (FR-WX-06) | `flight_test.cpp`: water film mass, drop momentum |
| Controller (NFR-REAL-04) | `flight_test.cpp`: rate curves, mixer, pre-arm checks, failsafe, Acro step response (overshoot < 30 %, no sustained oscillation); Angle mode holds its tilt limit and self-levels without a GPS fix: at most 10° while braking, within 1.5° of level and below 1.5 m/s after 25 s |
| Failure injection (FR-PHY-15) | `flight_test.cpp`, `ground_test.cpp`: injector timing, sensor faults in the readings, GPS loss and drift in position hold, degraded and dead motors, cell fault, compass and baro errors, RC loss; `mission_flight_test.cpp`: scheduled failures reach the vehicle |
| Position hold and RTH (FR-PHY-13, FR-WIND-06) | `flight_test.cpp`: hold within 2 m (estimate) up to each profile's wind limit and blown away (more than 20 m from the take-off point) at the first level whose mean wind is 20 % beyond the limit, stick speed and braking, RTH climb → return → land within the GPS error of home, failsafe RTH, GPS pre-arm checks |

### Holding position in a steady wind (NFR-REAL-08)

Position hold for 25 s in a steady wind, nose into the wind, then averaged over 10 s.
"Model" is `windHoldState()`, the analytical drag balance the Vehicle tab shows.

| Profile | Wind | Tilt (sim / model) | Battery power (sim / model) |
|---|---|---|---|
| FPV | 5.0 m/s | 7.9° / 8.0° | 188 / 188 W |
| FPV | 24.7 m/s | 45.2° / 45.4° | 944 / 965 W |
| camera | 5.0 m/s | 9.6° / 9.7° | 36.2 / 36.4 W |
| camera | 12.1 m/s | 24.0° / 24.2° | 58.7 / 59.4 W |
| survey | 5.0 m/s | 7.8° / 7.8° | 239 / 239 W |
| survey | 12.9 m/s | 20.7° / 20.8° | 384 / 386 W |

### Validation status (M7)

| Requirement | Status |
|---|---|
| NFR-REAL-01 reference figures | Checked: hover throttle, T/W, hover time and top speed within ±10 % (§9) |
| NFR-REAL-02 free fall | Checked |
| NFR-REAL-03 thrust at altitude | Checked |
| NFR-REAL-04 attitude control | Checked: Acro step table below; Angle-mode self-levelling |
| NFR-REAL-05 online terrain heights | Deferred to M4 (needs the GDAL elevation pipeline) |
| NFR-REAL-06 / 07 geo and UTM | Checked (`geo_test.cpp`) |
| NFR-REAL-08 wind balance | Checked: open- and closed-loop, table above |
| NFR-REAL-09 ground drone | Checked (`docs/GROUND_MODEL.md` §8) |
| NFR-REAL-10 astronomy | Checked (`astro_test.cpp`) |
| NFR-REAL-11 battery and fog | Checked. Open item: the battery temperature curves are representative, not taken from a cell datasheet |
| NFR-REAL-12 lidar DTM | Deferred to M4 (needs PDAL) |

### Acro step response

Measured with 50 % stick from hover. Rise is to 90 % of the commanded rate, overshoot is
the peak above the command, settle is the time to stay within ±5 %.

| Profile | Axis | Rise | Overshoot | Settle |
|---|---|---|---|---|
| FPV | roll | 0.096 s | 21 % | 0.46 s |
| FPV | pitch | 0.096 s | 17 % | 0.46 s |
| FPV | yaw | 0.17 s | 8 % | 0.90 s |
| camera | roll | 0.17 s | 22 % | 0.95 s |
| camera | pitch | — | 11 % | 0.71 s |
| camera | yaw | — | 11 % | 1.26 s |
| survey | roll | 0.31 s | 17 % | 0.80 s |
| survey | pitch | — | 6 % | 0.86 s |
| survey | yaw | — | 12 % | > 1.5 s |

"—" means the rate peaked below 90 % of the command before settling, which is typical of
the softer camera and survey tunes. The survey quad's yaw is slow because of its large yaw
inertia against weak propeller torque.
