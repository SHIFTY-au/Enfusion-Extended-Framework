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
- **The exit/despawn marker is > 80 m from the checkpoint** (confirmed by user).
- So 0 nodes is NOT "target too close for navmesh". The AI simply found **no path at
  all from the vehicle's stopped position** to a far target.

## Hypotheses TESTED and RULED OUT (cont.)
8. **Fresh clear+add waypoint re-commands the driver.** RULED OUT — requestCompleted
   stays 1, 0 nodes; the group did not put the car under a new drive order.
9. **Simple steering because target < Max Simple Steering Distance (50 m).** RULED OUT —
   the exit is > 80 m away and it still returned 0 nodes. Aiming even farther (a lead
   point past the exit) reverted; premise was wrong and it risked targeting off-mesh.

## RESOLVED — root cause confirmed (open-road test)
Moved the queue-slot markers to a clean straight stretch of open road and released:
**departure became smooth.** Crucially the log STILL read `0 nodes / requestCompleted=1`
even while driving off perfectly. So:

- **`0 nodes` is NOT the bug.** It means *simple steering* — the AI drives straight at
  the target with no navmesh path, and that is completely normal and works fine on a
  clear straight line (even to a target >80 m away). Every earlier reading of "0 nodes =
  broken path" was a misread. Struck.
- The ONLY differentiator is the **stop location**. Open road → clear straight line to
  the target → simple steering works. Checkpoint → the straight line to the exit is
  **obstructed by the checkpoint props/barriers** (and there's no navmesh corridor
  through them to route around), so simple steering flails.
- ⇒ It IS the navmesh/geometry at the checkpoint, exactly as the leading hypothesis said,
  but specifically: **no clear straight line AND no navmesh detour out of the gate.**

### FIX IMPLEMENTED — authored exit-path markers
The queue-slot markers already prove the AI drives to authored on-road points flawlessly.
So on release we no longer aim it straight at the distant exit; we feed it an ordered
chain of **exit-path markers** (author-placed on the road out of the gate) then the exit.
Each leg is a short, clear, straight shot it can simple-steer cleanly — same mechanism as
the inbound queue slots. Optional; empty = drive straight at exit (open road only).
- New attribute: `m_aExitPathMarkers` (array of `EEF_CheckpointExitPathEntry`), resolved
  by `ResolveExitPath()`; `GetExitRoute()` = markers + despawn point.
- New: `CreateMoveWaypoint()` (extracted), `AssignRouteWaypoints()` (drives a chain).
- `ReleaseVehicle()` now calls `AssignRouteWaypoints(group, GetExitRoute(...))`.
- New radius attribute `m_fExitPathCompletionRadius` (default 5m) for the intermediate
  points — loose enough to flow through, not stop/reverse.
- **DEPARTING** still only watches distance to the despawn point, so the chain doesn't
  change arrival/despawn. PromoteQueue ignores DEPARTING, so the route isn't disturbed.

**Map-side alternative / complement:** clear the navmesh corridor through the checkpoint
(props' navmesh generation mode, continuous road mesh, slot markers on-mesh) so a real
path generates. The exit-path markers work regardless, so they're the robust default.

**NOT the lever:** `Max Distance to Path` — only acts when a path exists; departure is
simple-steering (no path), so tuning path-follow params does nothing here.

---
## LEADING HYPOTHESIS (superseded — see RESOLVED above): can't path OUT of the stop position
Pathfinding returns **0 nodes to a far target** ⇒ no path exists **from where the
vehicle is standing**. The vehicle drives *in* fine but, once stopped at the front
slot, it's resting somewhere the navmesh can't originate a path from — most likely
**on/against checkpoint props/barriers, or a slot marker placed off the road mesh**.
This is the "local navmesh for the zone" instinct, but specifically about the
**start** position, not a jagged path.

Note: a **spawned** vehicle (open road, clean mesh) drives from a standstill fine —
so "stopped vehicles can't be commanded" is NOT the cause. The differentiator is the
**stop location** (checkpoint) vs spawn location (open road).

### DECISIVE test (authoring, ~2 min) — distinguishes navmesh/position (B) from a
### deeper re-command problem (A):
Temporarily place the queue-slot markers on a **clean straight stretch of open road,
well away from any checkpoint props/barriers**, and release.
- Departure now smooth + **node count > 0** ⇒ (B) confirmed: navmesh/obstacle at the
  stop position. Fix = navmesh cutters on the props / continuous road corridor /
  marker placement. (Authoring, map-specific.)
- Still 0 nodes / wobbles on clean open road ⇒ (A): the fully-stopped re-command is
  the problem after all → escalate to on-rails direct control.

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
