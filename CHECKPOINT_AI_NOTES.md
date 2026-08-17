# BUG: Checkpoint front-slot departure is incoherent (#18 blocker)

**Status:** OPEN — sole blocker to completing Stage 2 (#18) and moving to Stage 3 (#19).
**Build:** baseline restored at commit `7a93b25` (reverted to known-good `3c930df`).
**Branch:** `claude/issue-16-epic-tp3hy5`
**File:** `EnfusionExtendedFramework/Scripts/Game/EEF_CheckpointComponent.c`

---

## 1. Summary
The whole Stage 2 traffic loop works — spawn → seat → approach → enter zone → queue →
advance → hold at the front — **except the final step**. When the front (HELD) vehicle is
released, it does **not** pull away cleanly toward the exit. It veers off the road and/or
does multi-point turns before eventually recovering. Everything up to release is solid.

## 2. What works (confirmed, do not touch)
- Spawn, crew seating, dispatch.
- **Approach + zone entry**: smooth. Zone entry was fixed by *retargeting* the live
  waypoint (moving its origin) instead of clear-and-re-add, so the car never stops on entry.
- **Queue**: slot assignment, promotion/re-pack, hold timing — all correct.
- Debug auto-release timer (stands in for the #19 player gate).

## 3. The bug (reproduction)
1. Checkpoint trigger + spawn/despawn markers + queue-slot markers placed on a road that
   has **any curve** near the front stop.
2. Let a vehicle spawn, queue, reach the front, and get auto-released.
3. **Observe:** instead of driving straight out to the exit, the car turns off the road /
   3-point-turns, then eventually heads to the exit. On a **dead-straight** road it looks
   fine (see §5).

## 4. Established facts (what we KNOW)
- On release the car is **fully stopped** at the front slot (its prior move request
  completed — it braked to a halt on its own).
- Re-commanding a stopped car with a fresh, distant waypoint makes it **simple-steer**
  (drive a straight line at the target) for the first stretch before it settles. On a
  curve that initial straight line leaves the road → veer → correct → multi-point turn.
- The **same vehicle follows the same curved road fine while it is already MOVING**
  (approach and queue-advance are smooth, both done via waypoint *retargeting*).
- **No props/barriers were present in any failing test** — only a slight road curve. So it
  is NOT obstacle avoidance / being boxed in.
- `AICarMovementComponent.GetCurrentPath()` reported **0 nodes / requestCompleted=1** on the
  departing car. This is now believed to be the **wrong component to inspect** — the navmesh
  path lives on the driver *agent's* pathfinding, not the car movement component — so that
  reading is unreliable and is NOT proof that "no path exists."

## 5. Why a straight road hid it
Simple-steering drives a straight line to the target. On a straight road that line lies on
the road, so departure looks perfect. On a curve the same line leaves the road. The
underlying defect (cold re-command → simple-steer → cut the curve) is identical in both;
the road shape only changes whether it's visible.

## 6. Hypotheses TESTED and RULED OUT (do not re-test)
1. Leftover / multiple waypoints (clear all + single fresh) — same jank.
2. Clear + delayed re-add (settle 250 ms) — same.
3. Retarget the completed slot waypoint — no-op: request already completed, no new order.
4. Stuck-recovery (hold > Max Stuck Time) — tested 2 s hold, same.
5. Prime straight-ahead 40 m then retarget — same.
6. Fresh clear+add order — `requestCompleted` stayed 1, 0 nodes.
7. Jagged zone navmesh — path node count was 0, not "many".
8. Simple-steering distance cutoff (target < 50 m) — exit is > 80 m and still simple-steered.
9. Props / navmesh obstacle at the stop — **no props present** in any failing test.
10. **Nose-projected roll-out point** (drive 8 m ahead of the vehicle's nose first) —
    REGRESSION: a car at the front faces the way it *drove in* (toward the checkpoint), not
    the exit, so the point landed off-road/behind it → it drove off and 3-point-turned,
    jamming the lane and making the queue behind it look broken. Reverted in `7a93b25`.

## 7. Leading theory (current)
The AI driver, re-tasked **from a dead stop**, does not immediately follow the road navmesh;
it simple-steers toward the target first, cutting curves. The only mechanism proven smooth
is updating a waypoint while the car is **already moving** (retarget). So a fix must either
(a) never let the car cold-start toward a distant target, or (b) feed it look-ahead points
that are always **on the road**, so even simple-steering traces the road.

## 8. Proposed next approach (after reset — deliberate, one idea at a time)
**Road-network "pure pursuit."** Use `RoadNetworkManager.GetClosestRoad()` to snap a short
look-ahead point onto the road ahead of the car, and *retarget* the (moving) waypoint to it
every few ticks while in-zone/departing. The road network becomes the path — **no authored
exit markers**. Confirmed to exist in the API; exact signatures still needed (see §10).

## 9. Decision needed
Is coherent departure a **hard requirement to close #18**, or is it acceptable to ship #18
with the clean queue + a documented departure TODO and address it as a follow-up (e.g. its
own issue) so Stage 3 (#19 interaction) can proceed? #19 only needs the existing
`GetFrontVehicle()` / `ReleaseFrontVehicle()` / `OnVehicleStateChanged` seams, which are done.

## 10. Blockers / environment
- Dev sandbox has **BI wiki, arexplorer, and BI forums egress-blocked** — API docs cannot be
  read here. Real signatures must come from **Workbench autocomplete** (as done for
  `AICarMovementComponent`). Needed next: `RoadNetworkManager` methods + how to obtain it.
- Test loop: push to `claude/issue-16-epic-tp3hy5`; user tests in their own Workbench.

## Component seams already in place (for #19)
- `AICarMovementComponent`: `SetCruiseSpeed(float kmh)`, `ResetCruiseSpeed()`.
- Waypoints: `AssignMoveWaypoint()` (clear+add), `RetargetWaypoint()` (move live waypoint —
  the smooth path), `ClearWaypoints()`, `HasWaypoints()`.
- Public: `GetFrontVehicle()`, `ReleaseFrontVehicle()`, `GetOnVehicleStateChanged()`,
  `GetOnVehicleDespawned()`, `GetVehicleStates()`.
