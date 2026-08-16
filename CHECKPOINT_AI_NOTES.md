# Checkpoint AI Movement — Debugging Notes (#18)

Working notes for the AI vehicle **departure** problem so we stop re-testing dead ends.

## Symptom
When the front (HELD) vehicle is released, it drives forward ~5 m then "reassesses"
its pathing and behaves incoherently (veer, correct, sometimes multi-point turn).
We just want it to continue on its way to the exit.

## What WORKS (do not blame these)
- **Spawn → approach drive**: smooth. Vehicle spawns upstream, gets one waypoint at
  the checkpoint origin, drives in fine. **Paths fine (navmesh) at spawn.**
- **Zone entry / queue advance**: smooth, after we switched to *retargeting* the
  active waypoint (moving its origin) instead of clear+add. Works because the vehicle
  is still executing a live (incomplete) request, so moving the target updates it.
- **Queue itself**: slots, promotion, hold, release timing all correct.

## What does NOT work: departure
Vehicle is stopped/HELD at the front slot, then released → incoherent.

## Hypotheses TESTED and RULED OUT
1. **Leftover/multiple waypoints** → clear all + single fresh waypoint. RULED OUT (same jank).
2. **Clear + delayed re-add (let AI settle 250ms)**. RULED OUT.
3. **Retarget the still-active slot waypoint to the exit** (worked for zone entry).
   RULED OUT — at the front the request has already completed, so moving the origin
   issues no new order.
4. **Stuck-recovery** (hold > Max Stuck Time 4s). RULED OUT — tested 2s hold, same jank.
5. **Prime straight ahead 40 m, then retarget to exit.** RULED OUT (still jank).
6. **Fresh clear+add order at release.** RULED OUT — log shows `requestCompleted=1`,
   `0` path nodes on all samples; a fresh waypoint did NOT put the car under an active
   drive order.
7. **Jagged zone navmesh.** Effectively ruled out — path node count is **0**, not "many".

## Key diagnostic facts (from `GetCurrentPath` / `HasCompletedRequest`)
- On departure: **0 path nodes**, **requestCompleted=1**, consistently across time samples.
- 0 nodes = **no navmesh path** → the car is using **simple steering** (drive straight
  at the target), which the component gates by **`Max Simple Steering Distance = 50 m`**.
- At **spawn**, the approach target (checkpoint origin) is far (> 50 m) → a **navmesh
  path** is built → smooth. At **departure**, the exit marker is **< 50 m** away →
  **simple steering** → the wobble. This is the one theory that explains BOTH
  "smooth at spawn" AND "janky on departure" AND "prime 40 m didn't help" (40 < 50).

## LEADING HYPOTHESIS (current)
The departure target is within the 50 m simple-steering radius, so the AI abandons
navmesh pathfinding and drives straight at the point, wobbling near the checkpoint
geometry. Force navmesh pathfinding by aiming the departure waypoint **> 50 m down
the road** (a lead point well past the exit). Despawn still triggers at the real exit
marker via the arrival poll, so the far lead point is never actually reached.

### Test for this hypothesis
After aiming the depart waypoint far down the road, the log should flip to
**node count > 0** (navmesh path built) and the drive-off should be smooth.
If node count stays 0 → simple steering is forced by something else and we escalate
to on-rails direct control.

## Component API we have
- `AICarMovementComponent : AIBaseMovementComponent`
  - `SetCruiseSpeed(float kmh)`, `ResetCruiseSpeed()`, `GetLastNavlinkEntity()`
- `AIBaseMovementComponent`
  - `HasCompletedRequest(bool)`, `RequestFollowPathOfEntity(IEntity)`,
    `GetCurrentPath(array<vector>)`, `GetPathfindingComponent()`, event `OnPathSet()`
- Relevant prefab config (NOT script-settable): `Max Simple Steering Distance 50`,
  `Min Prediction Distance 4`, `Max Distance to Path 1`, `Stop Distance Coefficient 17`,
  `Steering PID`, `Cruise Vehicle Speed Kmh 60`, `Min Speed 6`.

## Escape hatch if the leading hypothesis fails
On-rails scripted control: drive the vehicle directly via `CarControllerComponent`
inputs along the authored lane, bypassing the AI driver entirely.
