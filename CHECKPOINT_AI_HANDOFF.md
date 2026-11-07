# Checkpoint AI — Handoff to the local (Forge-X MCP) agent

**You are the local agent.** You are running on the modder's machine with
**Forge-X-Reforger-MCP** connected and **Arma Reforger Workbench** live on
`127.0.0.1:5780`. The cloud agent that wrote this could not reach the engine
(isolated container, egress blocked, no game data), so every API and behaviour
below was established by the modder manually testing builds. Your job is to
finish it using the tools the cloud agent never had: inspect/modify prefabs,
query the navmesh, and test in play mode **without** a human round-trip.

- **Branch:** `claude/arma-ai-driving-workaround-imfeex`
- **Deep log:** `CHECKPOINT_AI_NOTES.md` (sections referenced below as §N). Read
  §13, §15, §16 first — they hold the confirmed root cause and chosen direction.
- **Code:** `EnfusionExtendedFramework/Scripts/Game/EEF_CheckpointComponent.c`
- **GitHub:** Issue #18 (checkpoint queueing) is blocked by Issue #23 (AI driving).

---

## 1. Goal

AI vehicles should queue at a checkpoint — approach → hold in a queue lane →
advance on release → depart — and **manoeuvre through a checkpoint the modder
dresses however they like** (cones, guardrails, props, placed at runtime via Game
Master). The dressing is the modder's to decide; the AI must cope with it.

## 2. System shape (so you don't re-derive it)

- One orchestrator component, `EEF_CheckpointComponent`, manages an array of plain
  `EEF_CheckpointVehicleState` objects (a pool of arbitrary vehicle prefabs — by
  design there is **no** per-vehicle component).
- Queue "gates" are computed from the road network in `ComputeQueueGates()`
  (~line 1152): `SCR_AIWorld → GetRoadNetworkManager() → GetClosestRoad() →
  BaseRoad.GetPoints()`, then a back-directed polyline with a gate every
  `m_fQueueSlotSpacing` metres. Arrival is detected by a signed-distance
  sign-flip across the gate in `ArrivalTick()` (~line 733).
- `Dispatch()` (~line 613) issues the **one and only** move waypoint to the
  despawn point (line 634) and never clears it. Vehicles are stopped **in place**,
  never by re-issuing a waypoint.

## 3. What is SOLVED — do not reopen

**The low-speed driving jank (the original #23) is fixed (§13).** Root cause: the
engine replans a path *from a dead stop* when handed a fresh waypoint → ~3.75 s
stall then a ~130° reverse. Fix: hold the car **in place without cancelling the
order**, and never re-issue a waypoint to a stopped car.

- `HoldVehicle(state)`: zero the velocity, then
  `SCR_PhysicsHelper.ChangeSimulationState(vehicle, SimulationState.COLLISION)`
  (still a collidable obstacle, not dynamically simulated — can't creep or be
  shoved) **plus** deactivate the group's AI (`SetGroupAIActive(state,false)` →
  `AIAgent.DeactivateAI()`), so the driver doesn't rev/reverse against the frozen
  car. Sets `m_bHeld = true`.
- `ResumeVehicle(state)`: back to `SimulationState.SIMULATION` + `ActivateAI()`,
  `m_bHeld = false`. No fresh waypoint — the original despawn waypoint is still
  live, so it just continues.

Confirmed working by the modder. **Leave this intact.** Queue logic (slots,
promotion `PromoteQueue()`, hold/resume call sites) is also verified correct.

## 4. What was tried for the PROP problem — and FAILED

The remaining blocker is **dressing the checkpoint with props**. History, so you
don't repeat it:

| # | Attempt | Result |
|---|---------|--------|
| 1 | Relax AI obstacle avoidance from script (`AIPathfindingComponent` filter setter) | **No such script setter exists.** The filters are set by native BT nodes (`AITaskSetPathfindingFilters`). Dead end; scaffold removed (commit 7dca96a). |
| 2 | Narrow the vehicle's near-field detection cone: set `AICarMovementComponent.MinRangeDetectionAngle = DetectionAngle` (§14/§14a) | Helps only at the margin. **Cannot restore a navmesh route a carve deleted.** Secondary lever at best. |
| 3 | Author the checkpoint as a composition with hand-defined nav-links (like the vanilla roadblock compositions) | **Modder tested: the engine vehicle AI still struggles** to navigate even an authored composition in tight clutter. Downgraded. |
| 4 | Drive on our own road-centreline waypoint chain | Rejected on analysis: the car still pathfinds between waypoints on the **carved** navmesh, so it can't drive a severed lane either. |

## 5. CONFIRMED ROOT CAUSE (§15, §16)

Two things, both verified by the modder with the navmesh debug overlay:

1. **Runtime navmesh carving is real.** The instant *any* object with a navmesh
   obstacle is placed on the road, the navmesh boundary pulls in around it and the
   AI path re-routes — **off the road** if the carve spans the lane. One traffic
   cone does it. A cone "down the middle" severs every on-road corridor.
2. **The engine's low-speed vehicle AI is unreliable in tight/cluttered space** —
   independently of carving (that's attempt #3's failure).

So: you **cannot** win by giving the engine better orders, better tuning, or
better navmesh in the cluttered zone. Every route through the engine's pathfinder
has failed.

## 6. CHOSEN DIRECTION — make the dressing not affect the navmesh (§16)

If the dressing doesn't carve, the road path stays intact and the already-working
driver sails the clean line straight through. No reliance on
pathing-through-clutter at all.

**The design fork (decide per prop):** a prop cannot both "not affect navmesh" AND
"be an obstacle the AI weaves around" — opposites.
- **Cosmetic props (cones, signs, light clutter)** → make them NOT affect the
  navmesh. The AI ignores them and drives the clean road line. Realistic — a car
  drives over a cone.
- **Real barriers (guardrails, jersey walls)** → keep them **off** the drive lane;
  use them to define the lane's edges, not sever it. They may carve as long as a
  drivable gap remains.

---

## 7. YOUR PLAYBOOK (use the Forge-X tools)

Work top to bottom. Verify each step with the engine before moving on.

### Step 0 — Connect and orient
- `wb_is_workbench_running` / `wb_connect` / `wb_state` — confirm the live link to
  `127.0.0.1:5780`.
- `wb_current_project` — confirm you're on this mod/project.
- Read `CHECKPOINT_AI_NOTES.md` §13, §15, §16 and skim
  `EEF_CheckpointComponent.c` around the line numbers in §2.

### Step 1 — Prove the carve mechanism and find the exact lever (the thing the cloud agent could not see)
- `asset_search "cone"` (and `"TrafficCone"`, `"roadblock"`) → get the base
  traffic-cone prefab path + GUID.
- `game_read` / `wb_entity_inspect` / `prefab` (inheritance chain) on that prefab →
  identify **which component/property makes it carve the navmesh**. Candidates:
  a physics collider on a navmesh-contributing layer, or an explicit navmesh-cut /
  obstacle component. **Report the exact component + property names you find** —
  that is the fact the whole fix turns on.
- Sanity-check against the engine: place the base cone on a road,
  `terrain_navmesh_status` before/after → confirm the boundary moves. Remove it.

### Step 2 — Author a non-carving cosmetic variant
- `game_duplicate` the base cone prefab into the mod (don't edit vanilla).
- On the variant, apply the minimal change from Step 1's finding, in priority
  order — stop at the first that works:
  1. **Strip the collider** entirely → pure-visual (best for cones; vehicles pass
     through, nothing to carve).
  2. **Move the collider to a physics/interaction layer navmesh generation
     ignores** (still bumps players, doesn't carve).
  3. **Remove the navmesh-cut/obstacle component** if the prefab carries one.
  Use `wb_entity_modify` / `wb_component` to make the change.

### Step 3 — Verify the variant does NOT carve
- Place the variant on the road.
- `terrain_navmesh_status` → confirm the navmesh boundary is **unchanged** vs the
  bare road. This is the pass/fail gate. If it still carves, go back to Step 2 and
  apply the next lever.

### Step 4 — Verify the AI drives through clean
- Set up (or reuse) a checkpoint with the variant props dressing the lane.
- `wb_play` → watch an AI vehicle drive the checkpoint. It should hold the clean
  road line and queue normally. `wb_stop`.
- If good: **this closes #23 for cosmetic dressing and unblocks #18.** Record the
  variant's resource path — the cloud agent offered to wire it into the checkpoint
  vehicle/dressing pool defaults in the repo; hand that path back.

### Step 5 — Barriers (only if the modder wants physical obstacles)
- Keep real barriers off the drive lane; author them as the lane's edges.
- If a chicane where the AI must physically weave is genuinely required, the
  engine won't do it reliably (attempt #3) → go to the fallback in §8.

### Optional — the secondary tuning lever, now that you can read live values
- `wb_entity_inspect` the vehicle's `AICarMovementComponent` → read the real
  `DetectionAngle` / `MinRangeDetectionAngle` (the cloud agent never had these).
- `wb_entity_modify` → set `MinRangeDetectionAngle = DetectionAngle` for less
  twitch around props the car *can* route past. Do not shrink
  `ObstacleAvoidanceCheckDist` / `Min Prediction Distance` — that cuts reaction to
  the car in front and would undo the queue spacing.
- **Attribute-name gotcha (§14a):** the strings mix conventions — spaced title
  case (`Min Prediction Distance`) vs camelCase (`ObstacleAvoidanceCheckDist`).
  Use each exactly as listed; they are not interchangeable.

---

## 8. FALLBACK (only if non-carving dressing is insufficient): script-drive the zone

Documented, **not built**. Take the AI out of the loop through the corridor and
steer the vehicle directly along the road-centreline polyline we already compute.
Immune to carving and to the flaky vehicle brain.

**Before building anything, run Phase 1 — proof-of-life** (one question only a live
engine answers): *with AI deactivated and a driver seated, does feeding
`VehicleWheeledSimulation` input actually move the car, or is sim input gated when
no controller is driving?*
- In play mode, on one vehicle in the zone: deactivate the group AI (as
  `HoldVehicle` does, but keep physics in `SIMULATION`, not `COLLISION`), ensure
  the engine is running, apply a small constant throttle + zero steering for ~1 s,
  log velocity. Does it creep?
- **If yes** → build Phase 2: a pure-pursuit controller ticking ~10–20 Hz that
  steers/throttles/brakes along the `ComputeQueueGates` road polyline between the
  zone-entry gate and despawn; hand control back to the engine AI outside the zone.
- **If no** → sim input is gated; this route is dead, report back.

**Confirm these API signatures first** (`api_search` / `component_search`) so Phase
1 compiles the first time — the cloud agent had to guess these:
1. Reaching the sim from the vehicle: `CarControllerComponent` →
   `GetSimulation()` return type (`VehicleWheeledSimulation`?).
2. `VehicleWheeledSimulation.SetThrottle(...)`, `SetSteering(...)` (1 arg or 2 +
   a `bool`?), `SetBrake(...)` / `SetBreak(...)` — exact params.
3. Engine control: `StartEngine()` / `IsEngineOn()` — which class, exact names.
4. Reading speed: `GetPhysics().GetVelocity()` vs a sim `GetSpeedKmh()`.

---

## 9. Hard constraints — do not violate

- **Keep the §13 hold/resume mechanism intact.** It is the hard-won fix for the
  original jank. Don't "simplify" it back into waypoint re-issuing.
- **Never re-issue a fresh waypoint to a stopped car** (§3, §13) — that is the
  exact bug that was fixed.
- **Don't edit vanilla prefabs in place** — `game_duplicate` into the mod and edit
  the variant. Don't save the world with a moved vanilla component.
- **`wb_cleanup`** before publishing — Forge-X installs temporary handler scripts
  into the mod for Workbench control; remove them.
- Prefer the **first** lever that passes the `terrain_navmesh_status` gate; don't
  over-engineer a prop variant.

## 10. What to report back to the modder / cloud agent

1. Step 1's finding: the exact component + property that controls navmesh carving.
2. The working non-carving variant's **resource path** (for wiring into the repo
   pool defaults).
3. Pass/fail of Step 3 (navmesh unchanged) and Step 4 (AI drives clean).
4. If you touched the fallback: the four confirmed API signatures and the Phase-1
   result.
