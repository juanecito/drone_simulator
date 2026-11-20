# Ground Model

How Drone Simulator drives a ground drone (milestone M6): the vehicle, the suspension,
tyre and drivetrain models, the drive controller, rollover, stuck and water detection, the
reference profile and how the model was checked. The code is in `src/core/physics`
(`ground_profile`, `ground_vehicle`, `drive_controller`, `surface`) and `src/core/sim`. It has
no GUI or OpenGL code and runs headless.

The ground drone is an RC-style rover (hobby scale, battery-electric, 2–6 kg). It shares the
`Vehicle` interface with the aerial drone, so the loop, missions, cameras, HUD and storage
treat both alike. Frames and conventions are those of the aerial drone
([Flight model](FLIGHT_MODEL.md), §1): world `x` = East, `y` = Up, `z` = South; body FRD.

Everything is implemented from vehicle-dynamics textbook material; no code from other
simulators or games is used.

## 1. Layouts (`ground_profile.hpp`)

| Layout | Wheels | Steering | Status |
|---|---|---|---|
| `skid-steer-4wd` | 4, one gear motor each | skid (left/right sides at different speeds) | done |
| `skid-steer-6wd` | 6, one gear motor each, middle axle at the centre | skid | done (no reference profile yet) |
| Ackermann RC crawler, tracked rover | — | — | later (FR-GND-01 S / C) |

Wheel hubs at rest sit at `x = ±wheelbase/2` (and 0 for the middle axle), `y = ±track/2`,
`z = cogHeight − r` below the centre of mass (body frame).

## 2. Chassis

The chassis is the same 6-DoF rigid body as the aerial drone (RK4, `double` positions). The
wheel spin rates are integrated with it in the same RK4 step: the wheel speeds advance through
the four RK4 stages together with the body.

**Sub-stepping.** Tyre slip makes the wheel spin and the body's sideways motion stiff. Each
tick is split into equal sub-steps short enough that `h·λ ≤ 1` for the stiffest mode (and at
most 1 ms), with `λ` the larger of

- wheel: `(r²·N·μ·B·C / v₀ + G²·(η/(Kv²·R) + I₀/(Kv·ω_f)) + T_brake/ω_b) / J`, and
- body: `1.5·g·μ·B·C / v₀`,

for the grippiest surface. The reference rover takes 5 sub-steps of a 500 Hz tick. Results
at 240 Hz, 500 Hz and 2 kHz agree within a few centimetres after 3 s of driving.

**Aerodynamic drag** per body axis, `½·ρ·C_d·A·v·|v|`, uses the velocity relative to the air
(wind), as for the aerial drone; for a rover it is small.

## 3. Suspension (FR-GND-03)

Each wheel hangs from a mount on the chassis and moves along the body's down axis `d` over
the travel `s_max`. Once per tick the terrain is sampled under every wheel (height, normal and
material: a local plane per wheel), plus under the four corners and the centre of the
underside.

- **Contact.** With `P` the mount, `n` the terrain normal and `X₀` a point of the plane, the
  hub extension at which the tyre (radius `r`) touches is
  `x = (r − n·(P − X₀)) / (n·d)`. No contact when `x ≥ s_max` or the strut is nearly parallel
  to the ground (`n·d > −0.25`).
- **Spring–damper.** Compression `c = s_max − x`, `F = k·c + c_d·ċ` with
  `ċ = n·v_P / (n·d)`; beyond full compression a bump stop 20 × stiffer adds to it.
- **Normal load.** `N = max(0, F) / (−n·d)`, so the strut's axial force balances it.
- **Mounts.** The springs have no preload: the static compression is `m·g / (n_wheels·k)`; the
  profile is rejected if it exceeds 90 % of the travel.

Forces act at the contact point `hub − r·n`, so lateral tyre forces roll the body and shift
load between the sides — the source of rollover in fast turns and on side slopes.

**Body contacts.** The corners of the body box and the middle of the underside collide with
the terrain like the aerial drone's points (penalty spring–damper, ω = 120 rad/s, ζ = 0.7)
with sliding friction `0.5·μ_surface`. A rover high-centred on a crest rests on its belly and
can get stuck. A body hit faster than `crashSpeedMs` is a crash.

**Ramps and bridges (FR-ITM-13).** The surface under a wheel or body point is the highest of
the terrain and the tops of nearby ramps and bridge decks that are not above the point (2 cm
tolerance at the joins): the world's `surfaceAt()`, passed to the vehicle as
`EnvironmentSample::surfaceQuery`. The hub at full extension is the wheel's query point, so a
wheel climbs onto a deck lower than its hub and a deck overhead (a bridge to drive under) is
ignored. Ramp and bridge tops are paved. Their sides, high ends, the underside of decks and
the kerbs are solid like other obstacles, so a rover driving into a ramp's high
end, off a deck's edge or under a deck too low for it hits them. Jumps follow from the same
contacts: driving slowly off a 25 cm ledge, this short rover pivots on its rear wheels for
about 0.2 s and nose-dives — as a real one does.

## 4. Tyres and surfaces (FR-GND-04, `surface.hpp`)

**Slip.** In the contact plane, with `f` the wheel's forward direction, `l = n × f`, `v_c`
the contact-point velocity and `u = ω·r` the tread speed:
`s_x = (u − v_c·f)/D`, `s_y = −(v_c·l)/D`, `D = max(|v_c·f|, |u|, v₀)`, `v₀ = 0.2 m/s`
(the floor stands in for the tyre's relaxation at a crawl). The combined slip is
`s = √(s_x² + s_y²)`.

**Force.** A simplified Magic Formula on the combined slip, along the slip direction:
`F = μ·N·sin(C·atan(B·s))`, with `μ = μ_surface·gripScale`. The reference tyre (B = 12,
C = 1.35) peaks at a slip of ≈ 0.2 and keeps ≈ 85 % of the peak when spinning or sliding
(loose surfaces lose less grip past the peak than asphalt).

**Rolling resistance** acts on the wheel as a torque `C_rr·N·r·tanh(ω/0.5)`.

| Surface | Peak μ | C_rr | From the terrain |
|---|---:|---:|---|
| `paved` (asphalt, concrete, bare rock) | 0.90 | 0.015 | rock texture |
| `grass` | 0.55 | 0.07 | grass texture |
| `dirt` (dry dirt, gravel) | 0.65 | 0.04 | — (missions) |
| `sand` | 0.50 | 0.16 | beach texture |
| `snow` (packed) | 0.30 | 0.08 | snow texture |
| `mud` | 0.35 | 0.14 | — (missions) |

Peak friction values are typical published figures for pneumatic tyres; rolling resistance is
raised for small wheels sinking into soft ground. The terrain lookup returns the material
with the largest weight in the terrain's texture blend (`terrain::dominantMaterial`), so the
grip matches what is drawn.

**Weather (FR-WX-07, `surfaceProperties(material, cover)`).** The weather generator keeps a
ground state, the same everywhere: a **wetness** (0 dry … 1 soaked; 1 mm of rain soaks the
ground, which dries with a 30 min time constant at 50 % humidity, slower in humid air, not at
all while it rains; it starts soaked when it is already raining and damp in fog) and a **snow
depth** (it settles at ≤ 1 °C at 10 mm of snow per mm of water, melts at 5 mm per °C and hour
and starts with three hours' worth when it is already snowing). Wetness blends each material
towards its wet values; snow turns any surface into snow from 2 cm and adds `2·depth` (at most
0.25) of rolling resistance for the wheels pushing through it:

| Surface | Wet μ | Wet C_rr | Note |
|---|---:|---:|---|
| `paved` | 0.60 | 0.018 | wet asphalt |
| `grass` | 0.35 | 0.08 | |
| `dirt` | 0.40 | 0.10 | turns to mud |
| `sand` | 0.55 | 0.10 | wet sand is firmer |
| `snow` | 0.20 | 0.10 | slush |
| `mud` | 0.25 | 0.18 | |

The HUD and the Simulation tab name the surface as the driver sees it (`wet grass`, `snow`).
The reference rover climbs 22° of dry grass but not of wet grass nor of snow-covered ground
(`GroundVehicle.RainAndSnowTakeTheGripAway`).

**Standing still.** A tyre model driven by slip velocity cannot hold a vehicle still on a
slope: it needs some creep to produce force. When the brake is on, both sides are at zero and
the rover has stopped (< 0.03 m/s), each tyre is held at its contact point by a tangential
spring–damper (3000 N/m, 60 N·s/m) — static friction. A tyre pushed past its grip (`μ·N`) or
its brake (`T_brake / r`) slides, its anchor following it, while the others keep holding. The
hold ends when the brake is released or the rover as a whole moves faster than 0.06 m/s. The
reference rover parks on 15° grass without moving and holds on a 30° paved side slope with the
load transfer of statics (§8).

## 5. Drivetrain (FR-GND-05)

One brushed DC gear motor per wheel (`Kv`, winding resistance `R`, no-load current `I₀`,
armature inertia, gear ratio `G`, gearbox efficiency `η`, ESC current limit `I_max`).

- **Current:** `I = clamp((d·V_batt − G·ω/Kv)/R, ±I_max)` for the side's command `d ∈ [−1, 1]`.
- **Torque at the wheel:** `T = G·(η·I/Kv − I₀/Kv·tanh(G·ω/10))` — the second term is the
  friction of motor and gearbox.
- **Wheel:** `J·ω̇ = T − F_x·r − T_rr − T_brake`, `J = J_wheel + J_motor·G²`.
- **Neutral** (|d| < 0.01, brake off): the ESC lets the motor coast (`I = 0`).
- **Brake** (pilot brake, Speed-control stop, disarmed, failsafe): the ESC shorts the windings
  (`V = 0`: dynamic braking, strong at speed, fading at a crawl) and the holding brake adds
  `T_brake·tanh(ω/1)`.
- **Battery:** the shared pack model of the aerial drone (FR-PHY-07: OCV curve, sag,
  temperature). The motors are fed with the pack voltage under the previous tick's load, and
  draw `Σ max(0, d·I)/η_ESC` plus the electronics. Regenerative braking (S) is not modelled:
  braking current is not returned to the pack.

## 6. Drive controller (FR-GND-11, `drive_controller.hpp`)

- **Arming** as for aerial drones: the arm switch must be seen off first; arming is refused
  with the drive stick out of neutral (|drive| > 0.1), on a critical battery, crashed, rolled
  over, drowned or without RC link. Disarmed, the ESCs hold the brake.
- **Manual:** throttle curve `(1 − e)·x + e·x³`, then the arcade mix for skid steering:
  `left = t + g·s`, `right = t − g·s`, both scaled back into [−1, 1] keeping their ratio
  (`g` = steering gain). Tank-style sticks (left and right side) are an input-mapping concern:
  `t = (L + R)/2`, `s = (L − R)/2` gives the same mix.
- **Speed control:** the drive stick sets a target `drive × cruiseSpeed`; a PI controller on
  the wheel-odometry speed (anti-windup by conditional integration) sets the common command;
  steering is mixed on top. A zero target near standstill brakes.
- **Brake** (keyboard `Space`): both sides at zero with the brake.
- **Failsafe:** after `failsafeDelayS` without RC link the rover stops and brakes.
- **Mode selection** from the transmitter's mode index: 0 and 1 (the default) Manual,
  2 and 3 Speed control. Heading hold (S), traction control (S) and the waypoint autopilot (C)
  come later.

## 7. Sensors, rollover, stuck, water

- **Sensors (FR-GND-10):** IMU, barometer, GPS and compass with the aerial noise models and
  the same estimator; vibration scales with the wheel speed. **Wheel encoders** count whole
  steps (`countsPerRev`); odometry distance and speed come from the mean of all wheels, so
  wheel spin fools it as on a real rover. Speed control uses the odometry speed.
- **Rollover (FR-GND-07):** tilted more than 70° from upright for 0.5 s. The rover disarms,
  cannot be armed until reset, and raises `Rollover` (missions fail). The HUD's rollover
  warning will use the **lateral load transfer** `(N_right − N_left)/ΣN` (±1 = one side's
  wheels lifting).
- **Stuck (FR-GND-08):** armed and driving (|mean side command| > 0.25, brake off) without
  getting 0.3 m from where the episode started. After `stuckDetectS` (5 s by default) one
  `VehicleStuck` event is raised; turning on the spot is not "driving". The time is reported
  continuously (`stuckS`), and missions fail after `rules.maxStuckS` (30 s by default).
- **Water (FR-GND-09):** water deeper than `wadingDepthM` at the vehicle drowns the
  electronics: the rover disarms, cannot be armed again and raises `WaterEntered`. Submerged
  parts of the body get heavy drag.
- **Obstacles:** the body box and the outline of each wheel collide with solid items (boxes,
  walls, trees) like the aerial drone: pushed out, normal speed removed, a hit above 1 m/s is a
  `Collision` (missions with a ground vehicle fail above `rules.maxCollisionSpeedMs`, 2 m/s by
  default), above `crashSpeedMs` a crash. Driving onto ramps and bridges as surfaces comes with
  the ground missions.

## 8. Reference profile and checks

**`rover-4wd-3kg` — 4WD skid-steer rover ~3 kg (generic).** Built from typical hobby-kit
components: aluminium chassis, four 37 mm 12 V brushed gear motors (30:1, 850 rpm/V, 2.4 Ω,
≈ 330 rpm with no load), 130 mm rubber wheels on 30 mm of spring–damper travel, 3S 3000 mAh
LiPo; centre of mass 0.11 m up, wheelbase 0.26 m, track 0.30 m. Not modelled on any specific
commercial product.

Derived figures (`computeGroundStats`, shown in the Vehicle tab, FR-GND-13) and what the
simulation does with the same profile:

| Figure | Derived | Simulated |
|---|---:|---:|
| Top speed, paved, full charge | 2.17 m/s | 2.23 m/s |
| Top speed, sand | — | 2.08 m/s |
| 0 → 90 % of top speed, paved | 0.42 s | within ±30 % (test) |
| Stall torque per wheel | 1.11 N·m | — |
| Steepest climb: paved / dirt / grass / sand / mud / snow | 42° / 33° / 29° / 27° / 19° / 17° (traction) | paved: climbs 35°, not 40°; grass: 25°, not 30°; snow: not 20° |
| Static rollover angle, sideways / front–back | 53.7° / 49.8° | stays up on a 30° side slope |
| Load transfer on a 30° side slope | 0.42 (rigid body) | 0.44 (body rolls on its springs) |
| Cruise (70 % of the grass top speed) | 1.48 m/s at 1.9 A | — |
| Range / driving time at that speed, grass | 7.3 km / 82 min | — |

The climbing limits are those of tyres at their peak grip (`tan θ = μ`). Driving up at full
throttle loses a few degrees to the load transfer onto the rear wheels; past the limit the
wheels spin, the grip drops towards its sliding value and the rover slides back. The motor
torque limit (`sin θ + C_rr·cos θ = n·T/(m·g·r)` at a crawl) is not reached by this profile on
any surface.

Unit tests (`tests/unit/ground_test.cpp`) check: profile loading, round trip and validation
messages; the derived figures; the arcade mix, arming checks and failsafe; resting on the
suspension (wheel loads sum to `m·g`, static compression); top speed and acceleration against
the derived figures; braking distance and holding; turning on the spot and arcing in the right
direction; speed control holding its set speed; climbing within the limit and sliding back
beyond it; parking on a slope with and without a holding brake; the side-slope load transfer;
rollover and its arming block; stuck detection against a wall; drowning in water; softer
ground being slower; failsafe; determinism and tick-rate independence; collisions with an
obstacle; and the world switching between vehicle types.
