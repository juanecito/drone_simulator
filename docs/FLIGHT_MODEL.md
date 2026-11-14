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
- **H-force (rotor drag):** `H = −k·T·μ` in the rotor plane, with `μ = V∥/(ω·R)`.
- **Ground effect:** `T / (1 − (R/4z)²)` (Cheeseman–Bennett), capped.
- **Vortex ring state:** up to 25 % thrust loss when descending at about the hover induced
  velocity `v_h = √(T/(2ρA))`. There is no loss below `0.3·v_h` or above `2·v_h`.

### Battery (equivalent circuit)

- **Terminal voltage:** `V = N·(OCV(SoC, T) − V_RC) − I·R0(T)`. `R0` follows an Arrhenius law
  in temperature, and one RC branch models the slow polarisation.
- **Charge:** coulomb counting. The usable capacity falls in the cold.
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
  with gain 0.25 s⁻¹.
- **Accelerometer trust:** the accelerometer is trusted only while `|a + g·up|` stays within
  0.05 g of the expected value. With a GPS fix, the GPS-derived acceleration (smoothed,
  at most 0.5 s old) is removed from the reference first, so steady turns and acceleration
  do not tilt the horizon.
- **Gyro calibration:** while the vehicle is disarmed, still and on the ground, the gyro bias
  is learned (τ = 0.5 s).
- **Altitude and climb rate:** from the barometer, plus the GPS position and velocity once
  there is a fix.
- **Known limitation:** without a GPS fix, long accelerating manoeuvres slowly drift the
  attitude estimate, as on a real flight controller.

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
- **Position hold (mode 4):** behaves as altitude hold until M7.

### Arming and failsafe

- **Pre-arm checks:** throttle low, tilt under 25°, battery not critical, not crashed, RC link
  present, and the arm switch cycled since start-up.
- **Failsafe:** 1 s after the RC link is lost the craft lands at 1.5 m/s.

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
| `fpv-5in-racer` | 650 g | 6S 1100 mAh LiPo | 5.1″ tri-blade, 1900 Kv | 0.237 / 0.24 | 8.45 / 8.5 | 8.3 / 8.0 min | 30.7 / 30 m/s | 8 |
| `camera-250g` | 249 g | 2S 2000 mAh Li-ion | 4.7″, 2700 Kv | 0.477 / 0.47 | 2.94 / 2.9 | 24.8 / 24 min | 16.3 / 16 m/s | 5 |
| `survey-1500g` | 1.5 kg | 4S 5000 mAh LiPo | 10″, 740 Kv | 0.527 / 0.52 | 2.60 / 2.6 | 20.2 / 20 min | 18.1 / 18 m/s | 5 |

- **Top speed:** the level speed at the profile's tilt limit, computed in steady state. In
  simulated flight (altitude hold, with a GPS fix) the drones reach 31.9, 16.7 and 18.5 m/s.
- **Max wind level:** the highest level whose gust peak the drone can still hold position
  against within its tilt limit and 85 % of full command (FR-WIND-07).

## 10. Validation

The checks run in `ctest`. The most relevant tests:

| Requirement | Test |
|---|---|
| Free fall and drag (NFR-REAL-02) | `physics_test.cpp` |
| ISA atmosphere (NFR-REAL-03) | `physics_test.cpp` |
| Reference figures (NFR-REAL-01) | `flight_test.cpp`: static and dynamic hover throttle and current, test-stand thrust-to-weight, top speed in flight |
| Holding against wind (NFR-REAL-08) | `flight_test.cpp`: the drone leans into the wind and stays in place |
| Wind statistics | `wind_test.cpp`: 60 s mean speed per level, Dryden σ, gust occurrence, determinism |
| Controller | `flight_test.cpp`: rate curves, mixer, pre-arm checks, failsafe, Angle-mode limits, Acro step response |

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
