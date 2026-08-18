# BUG: AI vehicle driver janks throughout the checkpoint zone (#18 blocker)

**Status:** OPEN — sole blocker to completing Stage 2 (#18).
**Branch:** `claude/issue-16-epic-tp3hy5`

---

## 1. Symptom
Inside the low-speed checkpoint area the AI vehicle driver moves incoherently:
it stops mid-road, veers onto the shoulder, and does 3-point turns before
recovering. This is **not tied to one phase** — it has been observed at spawn
dispatch, zone entry, queue advance, and release. The queue *logic* (slots,
promotion, hold, release timing, cruise-speed governor) is correct; the defect
is purely the vehicle's physical movement.

## 2. What testing established (cannot be re-derived from code)
- The jank is **location/road-shape dependent**. On a dead-straight road stretch
  it looks acceptable; any curve near the action makes it visible. Same code,
  same orders — only the road shape changes whether it shows.
- The **same car follows the same curve fine once it is already moving.** The
  jank appears when the driver is (re)tasked toward a target, most severely
  from a **dead stop**.
- **No props, barriers, or authored obstacles were present in any failing
  test** — a slight road curve alone reproduces it. It is not obstacle
  avoidance / being boxed in.

## 3. Ruled out by testing (do not re-attempt)
Every approach below tried to make the *engine AI* drive better by issuing
better waypoint orders. All failed with the same jank:
- Clearing + re-adding a single fresh waypoint.
- Delayed re-add (let the car settle first).
- Retargeting a live waypoint's origin instead of re-adding.
- Priming a straight-ahead point before the real target.
- A nose-projected "roll-out" point ahead of the stopped car — **regression:**
  a car at the front faces the way it drove *in*, so the point landed
  behind/off-road and it 3-point-turned, jamming the lane. Reverted.

Conclusion: issuing better orders to the engine's AI driver does not fix this.
The engine vehicle brain is unreliable at low speed in tight/curved space.

## 4. Known API (obtained from Workbench; not fetchable in this sandbox)
```
class RoadNetworkManager {
    proto int  GetClosestRoad(vector pos, out BaseRoad foundRoad, out float distance, bool skipNavlinks = false);
    proto int  GetRoadsInAABB(vector aabbMin, vector aabbMax, out array<BaseRoad> outRoads);
    proto bool GetReachableWaypointInRoad(vector agentPos, vector goalPos, float range, out vector outPos);
}
```
Still needed from Workbench: how to obtain the `RoadNetworkManager` instance,
the `BaseRoad` surface (to place a point on the road / know which side), and the
vehicle input/drive component (throttle/brake/steering) if going physics-based.

## 5. Environment constraint
BI wiki, arexplorer, BI forums, the Reforger Workshop, Steam, and YouTube are
all egress-blocked in this sandbox (confirmed again during the research pass
below — direct WebFetch to every one of those domains was refused by the
proxy). API signatures must come from Workbench autocomplete or from search
snippets, not full page fetches. User tests each build by pulling
`claude/issue-16-epic-tp3hy5` into their own Workbench.

## 6. Research: how the community works around this (2026-08-17)
Web search (snippet-level only, no full page fetch — see §5) confirms this is
a widely-acknowledged, longstanding limitation of vanilla Reforger's vehicle
AI, not something specific to our checkpoint setup. That matches our own
testing conclusion in §3. Two independent public confirmations:

- **"Competent AI driving"** (Reforger Workshop, id `68FCF11534562F2E`) exists
  specifically to force AI vehicles to take wider turns "in order to not get
  stuck in a forward-reverse loop" — i.e. the exact 3-point-turn symptom in
  §1, reproduced widely enough to have a dedicated fix mod.
- **CRX Enfusion A.I.** (Workshop id `5F268647F8A1A1F4`), the largest
  community AI overhaul mod, has a whole separate, still-WIP "Vehicle Column"
  feature for convoy/column driving (see their Aug-2025 WIP video, YouTube id
  `81pmHWfVnJ8`) — built from custom behaviour-tree logic, not by issuing
  better waypoints. A well-resourced dedicated AI mod treating close-order
  vehicle-follow as a hard problem requiring bespoke BT work independently
  corroborates §3's conclusion: better waypoint orders alone don't fix this.

### New angle not yet tried: navmesh/road geometry, not just orders
Bohemia's own 1.2 patch notes ("Changes and Improvement to AI features in
1.2") describe several driving-relevant engine changes that are geometry/data
levers, not order-issuing logic:
- Vehicle navmesh **agent radius was enlarged from 2m to 2.6m**.
- Road-network rebuilding was changed to modify road **width** to avoid
  splitting roads, and later made more targeted (only certain areas, not the
  whole road).
- Vehicle **obstacle avoidance moved from the character to the car**
  component (i.e. it's now handled at the vehicle level).

This lines up with our own §2 finding — *"road-shape dependent... a slight
curve alone reproduces it... the same car follows the same curve fine once
already moving"* — better than anything order-related does: a moving vehicle
is already inside a valid navmesh corridor and just follows it, but a
(re)planned path from a dead stop has to fit a start pose through a corridor
sized for a 2.6 m-radius agent. If the authored road/navmesh width at our
checkpoint's curve is tight, a stopped-and-replanning vehicle can find no
clean forward arc and falls back to reverse-adjust (3-point turn), while a
vehicle already moving through the same curve never has to solve that from a
standing start. **Untried and worth checking before going physics-based:**
measure/widen the navmesh corridor at the specific curve in our checkpoint
zone, independent of any code change.

### New API surface for the "direct in-zone script control" candidate
On top of the `RoadNetworkManager` methods already in §4, search turned up:
- `SCR_AIVehicleUsageComponent` (`scripts/Game/AI/Components/SCR_AIVehicleUsageComponent.c`)
  — attached to the root entity of every AI-usable vehicle; likely the
  integration point for reading/overriding vehicle AI state rather than
  fighting it through waypoints.
- `AIPathfindingComponent` — each vehicle carries pathfinding filter flags on
  this component, changeable via behaviour-tree nodes
  `AITaskSetPathfindingFilters` / `AITaskResetPathfindingFilters`. This could
  let us bias a vehicle toward road-graph pathing over general navmesh
  inside the zone without hand-rolling throttle/steer — a middle ground
  between "better waypoints" (ruled out) and full physics-based driving
  (§4's original candidate). Exact flag values weren't retrievable through
  search snippets — needs Workbench autocomplete/decompiled source.
- `NavmeshWorldComponent` on `AIWorld` exposes the `ChimeraNavmeshWorld` used
  for vehicle pathing (the 2m→2.6m agent radius above applies here).

### Sources (all on egress-blocked domains — open these normally, not via fetch)
- Competent AI driving — https://reforger.armaplatform.com/workshop/68FCF11534562F2E
- CRX Enfusion A.I. changelog — https://reforger.armaplatform.com/workshop/5F268647F8A1A1F4/changelog
- CRX Vehicle Column WIP video — https://www.youtube.com/watch?v=81pmHWfVnJ8
- Bohemia: Changes and Improvement to AI features in 1.2 — https://reforger.armaplatform.com/news/modding-update-june-7-2024
- Official Modding Boot Camp #12 – Artificial Intelligence — https://www.youtube.com/watch?v=SsL8arV1lMA
  (this is the specific bootcamp episode covering the AI system; worth a
  human watch-through since it can't be fetched/transcribed from here)
- SCR_AIVehicleUsageComponent source (Arma Reforger Explorer) — https://arexplorer.zeroy.com/_s_c_r___a_i_vehicle_usage_component_8c_source.html
- Arma Reforger Script API (AI group) — https://community.bistudio.com/wikidata/external-data/arma-reforger/ArmaReforgerScriptAPIPublic/group__AI.html

## 7. Recommended next step
Cheapest-first: have someone with Workbench/browser access (a) skim Boot Camp
#12 and the two workshop mods above for concrete parameter names, and
(b) check the checkpoint zone's authored road width/navmesh corridor at the
problem curve against the 2.6 m agent radius. Only fall back to the
`AIPathfindingComponent` filter or full `RoadNetworkManager`-driven
in-zone-only physics control if the geometry angle doesn't resolve it —
in that order, since each is progressively more code to own long-term.

## 8. Implementation: road-network target-point correction (2026-08-18)
Landed the "calculated queue location" fix in `EEF_CheckpointComponent.c`,
gated behind a new `m_bSnapToRoadNetwork` attribute so it can be A/B tested
against the pre-#23 raw-marker behaviour without a code revert. It's one
mechanism, not two separate ones ("road-graph pathing" and "calculated queue
location" were the same idea described at two altitudes):

- New helper `ResolveDrivePoint(fromPos, targetPos)` snaps `targetPos` onto
  the road corridor reachable from `fromPos` via
  `RoadNetworkManager.GetReachableWaypointInRoad()`, falling back to the raw
  point untouched if the manager/query is unavailable - purely additive,
  can't behave worse than before.
- Wired into all three places the component issues a drive-to-point order
  (matches "not tied to one phase" in §1): the approach target in
  `Dispatch()`, the queue-slot target in `DriveToSlot()`, and the exit
  target in `ReleaseVehicle()`. The AI's own driving (throttle/steer/
  obstacle-avoidance) is untouched - only the target point changes.
- New attributes: `m_bSnapToRoadNetwork` (toggle, default on) and
  `m_fRoadSnapRange` (search range in metres for the reachability query).

**BLOCKING - not yet testable as-is:** the accessor to obtain a live
`RoadNetworkManager` instance was never confirmed (see §5 - BI wiki/forums/
Workshop/YouTube are all egress-blocked from this sandbox). `ResolveDrivePoint()`
currently hardcodes `RoadNetworkManager roadMgr = null;` with a `TODO(#23)`
marker at that exact line, so the feature compiles and is a documented no-op
until that one line is fixed. Deliberately not guessed at - a wrong guess
there would just fail to compile and burn a Workbench test cycle for
nothing, the same trap that made §3's testing pass slow.

**Needed from whoever has Workbench next:** type `GetGame().GetWorld().` and
separately check `AIWorld` in autocomplete for a method returning
`RoadNetworkManager` (or however an `AIWorld`/`BaseWorld` instance is itself
obtained, if that's an extra hop). Report back or fix the one line in
`ResolveDrivePoint()` directly.

## 9. Live-tested finding (2026-08-18): heading, not position, is the dominant
## variable — new design direction supersedes §8's position-snap approach

**Test result:** §8's position-snap code was never actually live when tested
(`roadMgr` was still hardcoded `null` per §8's blocker), so what got tested
was baseline behaviour — described as "the AI was super bad." Separately,
manually re-orienting a stopped vehicle at a curve queue point (in Workbench,
before release) so it faced more directly down the exit road produced an
**acceptable departure with only a slight S-shaped path**, instead of the
usual 3-point-turn jank. Same position, only heading changed.

**Conclusion:** vehicle heading at the stop point is the dominant variable,
not point position. This fits the mechanism theory in §6/§8: the AI's
replan-from-a-stop has to solve a forward arc from the vehicle's *current
heading* to the target; a badly misaligned heading means no forward arc
fits the corridor, so it reverses to reorient. Align the heading first and
the arc shrinks to something solvable. §8's position-only snap does nothing
for a vehicle sitting at the *right point* facing the *wrong way* - this
likely explains why it read as useless even setting aside that it was inert.

**NOT the same as the already-ruled-out "nose-projected roll-out point" in
§3.** That attempt derived a lead-in point from the vehicle's *live* facing
when it happened to stop (unreliable, caused a regression). This is an
*authored/intentional* target heading, independent of how the vehicle
settled - a different, better-grounded lever. Do not conflate the two or
re-dismiss this based on §3's entry.

**Ruled out outright: directly snapping a stopped vehicle's transform to a
target heading in code.** Explicitly rejected as "100% immersion breaking."
Do not revisit.

### New design direction (replaces §8's approach; supersedes it, not additive)
Rather than authoring per-slot heading (which would push authoring burden
onto the mission maker) or snapping a stopped vehicle's facing, let the AI
drive one continuous route through the zone - as if passing straight
through at the already-governed low speed - and never re-task it while it's
in the lane. Concretely:

1. Queue hold points are **calculated**, not authored, from the checkpoint
   (or a mission-maker-defined checkpoint zone): starting at the checkpoint
   and walking back along the road at even spacing, find the road's local
   heading at each spacing interval and drop a gate line perpendicular to
   it there.
2. The vehicle is dispatched with a single continuous route through the
   zone - never re-tasked to a series of disconnected stop markers the way
   `DriveToSlot`/`RetargetWaypoint` currently work.
3. Each poll tick tests whether the vehicle has *crossed* its currently
   assigned gate line (a line-crossing test between consecutive polls, not
   a proximity/radius test like today's `HasArrivedWithin`).
4. On crossing its assigned gate: halt **in place, mid-route**, without
   cancelling or replacing the order.
5. On release: resume the *same* order/route - no new waypoint, no replan -
   so the AI is never asked to solve anything from a dead stop, which is
   exactly the case we already know works cleanly (§2: "the same car
   follows the same curve fine once it is already moving").

This explains the test result directly: a vehicle mid-route is tangent to
the road by construction, at every point along it, with zero authoring
burden per checkpoint.

### Blocking - four things need Workbench confirmation before this is coded
Learned from §8: don't guess API and burn another test cycle. In priority
order:

1. **Road tangent/heading at an arbitrary point.** Needed to place gates.
   `RoadNetworkManager.GetClosestRoad()` returns a `BaseRoad` (see §4) -
   does `BaseRoad` expose a curve/point-list or direction query? Check
   `BaseRoad`'s members in autocomplete.
2. **Obtaining a live `RoadNetworkManager` instance at all.** Same
   unresolved blocker as §8 - needed regardless of design.
3. **Walking distance along the road to space gates evenly**, including
   across a curve built from multiple connected road segments. Look for a
   points/nodes array or a distance-along-road query on `BaseRoad`, and
   whether `RoadNetworkManager.GetRoadsInAABB` is how segments get stitched
   together across the queue lane's span.
4. **The linchpin: halting a moving AI vehicle in place without cancelling
   its order, then resuming the same order later.** Determines whether
   "gate-crossing + freeze" is achievable at all. `AICarMovementComponent`
   is already in use in this file (`SetCruiseSpeed`/`ResetCruiseSpeed`/
   `GetCurrentPath`) - check whether it has a real stop/halt call, or
   whether `SetCruiseSpeed(0)` actually holds at zero rather than being
   treated as "unset" (today's code treats `kmh <= 0` as "reset to prefab
   default," which is not a true stop - see `ApplyCruiseSpeed`). Also check
   whether `AIWaypoint`/`SCR_AIWaypoint` has any pause/dwell state as an
   alternative mechanism.

Line-crossing detection (step 3 above) is plain vector math on data already
available - no new engine API needed there.

### Debug visualisation (nice-to-have, asked for but not blocking)
Enfusion-family engines conventionally expose a debug-draw primitive -
commonly a `Shape` class (`Shape.CreateSphere`, `CreateArrow`, lines, etc.)
for exactly this kind of gizmo. Unconfirmed for this title specifically -
check Workbench autocomplete on `Shape`. If present, it slots onto code
that already exists: `DumpDeparturePath()` already pulls
`movement.GetCurrentPath()` and currently only prints it to console -
extending that to draw spheres/lines would visualise the live AI path, the
computed gate lines, and the hold points at once in the Workbench viewport.
If nothing like that exists, the console dump already in place is the
fallback.

### Status of §8's code
Removed. `m_bSnapToRoadNetwork` / `ResolveDrivePoint()` and the whole
marker-based queue system (`EEF_CheckpointQueueSlotEntry`, `m_aQueueSlotMarkers`,
`DriveToSlot`, `ResolveQueueSlots`, `GetSlotPosition`, `HasArrivedAtSlot`,
`RetargetWaypoint`, `m_fQueueSlotCompletionRadius`, `CHECKPOINT_SLOT_ARRIVAL_SLACK`)
are gone, replaced by the design below - implemented 2026-08-18.

## 10. Implemented: calculated gate-crossing queue (2026-08-18)

Landed in `EEF_CheckpointComponent.c`. Two corrections to the section 9 plan
from live feedback:
- **Item 1 (road tangent) resolved, not blocked.** `BaseRoad` only exposes
  `GetWidth()` and `GetPoints(out array<vector>)` - no dedicated tangent
  query. Tangent is computed from consecutive points in that polyline
  instead (`ComputeQueueGates()`'s segment-direction math), so this needed
  no new engine capability.
- **Item 4 (halt-in-place/resume) dropped as a requirement.** "Stop and
  cancel order" was confirmed acceptable, so gate-stop reuses the existing
  `ClearWaypoints()`/`AssignMoveWaypoint()` primitives instead of needing any
  new pause/dwell/freeze API. This works specifically *because* the vehicle
  was already driving a continuous, on-road route when it stopped - its
  heading is correct by construction, so resuming with a fresh order toward
  the next gate is a trivial straight-ahead correction, not a re-plan from a
  bad pose (the case that was actually broken).
- Item 3 (walking distance along the road) needed no separate Workbench
  answer either - it's the same `GetPoints()` polyline, walked and
  accumulated in script. The requested spacing input (longest vehicle +
  buffer) became the `m_fQueueSlotSpacing` attribute default (12m, per a
  ~7.5m truck + ~4m clearance).

**What's live:**
- `ComputeQueueGates()` - lazily computes `m_iMaxConcurrent` gates by
  finding the road nearest the checkpoint (`RoadNetworkManager.GetClosestRoad`),
  projecting the checkpoint onto it, and walking the point list back toward
  the spawn point, accumulating distance to place a gate every
  `m_fQueueSlotSpacing` metres with a heading from the local segment
  direction.
- `EnterQueue()` no longer retargets anything - a vehicle keeps driving the
  same waypoint it was dispatched with the whole time it's in the lane.
- `ArrivalTick()` halts a QUEUED/AT_FRONT vehicle (`ClearWaypoints`) the
  instant `HasCrossedAssignedGate()` detects it has passed its gate's
  perpendicular line (a signed-distance sign flip between polls, horizontal
  only) - self-limiting, fires exactly once per gate.
- `PromoteQueue()` calls the new `ResumeTowardFront()` (reissues
  `AssignMoveWaypoint` toward the checkpoint origin) instead of retargeting
  to a marker - the vehicle resumes and the next, closer gate catches it.

**Resolved: the `RoadNetworkManager` instance accessor.** `GetGame().GetAIWorld()`
is typed to return the base `AIWorld` class, so Workbench autocomplete on
that chain (and on `GetGame().`/`GetGame().GetWorld().`) never showed
anything road-related - three separate autocomplete checks came up empty
because `GetRoadNetworkManager()` is declared on the `SCR_AIWorld` subclass,
invisible to autocomplete until cast down to that type. Found by searching
all shipped scripts (not just this project) for the literal string
`RoadNetworkManager`, which turned up the base game's own
`SCR_resupplyTaskSolver.c` using exactly this pattern:

```
SCR_AIWorld aiWorld = SCR_AIWorld.Cast(GetGame().GetAIWorld());
RoadNetworkManager roadNetworkManager = aiWorld.GetRoadNetworkManager();
```

`ComputeQueueGates()` now uses the same two-step cast-then-call instead of
the hardcoded `null` placeholder. This was the last unconfirmed piece -
everything downstream (gate math, crossing detection, resume-on-promotion)
was already fully wired. The whole feature is implemented and ready for a
live Workbench test.

**Minor unflagged assumption:** uses `Math.Sqrt()` (not previously used
elsewhere in this file, though `Math.RandomFloat`/`Math.RandomInt` already
are). Standard enough on a Math class across Bohemia engines that this
wasn't treated as a fourth blocker, unlike the `RoadNetworkManager` accessor.

## §11. First live test: shared-target bug found and fixed (2026-08-18)

First in-Workbench test of the §10 implementation, after the `RoadNetworkManager`
accessor fix. Observed (no debug/hardcoding, pure observation):
- Spawn, dispatch, approach, zone entry, and slow-down all worked as expected.
- Vehicle 1 drove the curve smoothly and stopped roughly at the front stop
  point, as intended.
- As each following vehicle (2, 3, 4) approached the one stopped ahead of it,
  the stopped vehicle would move forward and pull off-road before stopping
  again. This repeated down the line.
- The four vehicles did not visibly end up spaced ~12m apart along the road.
- No move/resume was observed following release.

**Log evidence that pinpointed it:** `ComputeQueueGates()` logged "Computed 4
of 4 requested queue gate(s)" - gate math itself was fine. But the log line
for the front vehicle reaching HELD ("Front vehicle reached the stop line -
HELD, awaiting release") never appeared, even though it visibly stopped. No
front vehicle ever transitions to HELD, so `BeginHold()`'s auto-release timer
never even starts - there was never a real release/resume to observe.

**Root cause:** §10's implementation kept every queued vehicle's AI waypoint
aimed at the single, shared, far-away checkpoint origin the whole time (a
deliberate design choice at the time: "no retargeting - it keeps driving this
exact waypoint the whole way through the queue"), relying purely on a
perpendicular-line crossing test (`HasCrossedAssignedGate`) to notice when a
vehicle passed its calculated gate. Two things broke this:
1. `AssignMoveWaypoint()`'s completion radius on that shared approach
   waypoint is `m_fWaypointCompletionRadius` (15m default, deliberately
   generous per its own doc comment) - *larger* than the 12m default gate
   spacing. The AI's own arrival logic could therefore call the drive request
   "complete" and stop the vehicle without it ever crossing the exact gate
   line the crossing test was watching for. This is exactly what happened to
   vehicle 1: it stopped (satisfying the AI's own generous radius) short of
   / without crossing gate 0's line, so `HasCrossedAssignedGate` never fired.
2. Every vehicle's real AI destination was the *same* far-away point. A
   vehicle behind one that had already stopped had no reason (from the AI's
   perspective) to stop before reaching that shared target - it treated the
   stopped vehicle ahead as an obstacle blocking its route to that target and
   steered around it, off-road, exactly matching "moved forward and pulled
   off road" repeating down the line.

**Fix:** retarget each vehicle directly to its own assigned gate's exact
on-road point (fresh `AssignMoveWaypoint`, same mechanism already proven safe
in `ReleaseVehicle()`), using a new, deliberately tight
`m_fQueueGateCompletionRadius` (3m default) instead of the generous general
radius. This is still "the same road, in the direction of travel" - not an
off-road marker and not a heading-snap - so the property that actually fixed
#23 (heading comes from driving the real road) is unaffected. It just makes
each gate a real, distinct, near destination instead of an invisible
tripwire on a route aimed somewhere else, which is what:
- Makes the AI's own arrival and our arrival check agree (they now test the
  same point), so HELD/promotion/release actually fire.
- Gives each queued vehicle a nearer stopping point than the vehicle ahead
  of it, so it has no reason to try to drive past/around it.

`EnterQueue()` and `ResumeTowardFront()` now both retarget to
`GetQueueGate(slot).m_Point`. `ArrivalTick()`'s QUEUED/AT_FRONT case now
checks `HasArrivedWithin(vehiclePos, gate.m_Point, m_fQueueGateCompletionRadius)`
instead of the old signed-crossing test. `HasCrossedAssignedGate()`,
`SignedDistanceAlongGate()`, and the `m_LastPolledPos` poll-pairing field
they depended on are removed as dead code - a plain arrival-radius check
against the vehicle's actual destination replaces them entirely. `m_Forward`
on the gate class is kept: `ComputeQueueGates()` now logs every computed
gate's point + heading, so gate placement can be sanity-checked in the
Workbench console against where vehicles actually stop - a low-effort stand-in
for the debug visualisation asked about in section 9 (no in-world debug-draw
API was found/needed).

**Not yet re-tested** - this fix has not had a live Workbench pass yet.

**Rejected explicitly - do not revisit:** direct transform heading-snap
("100% immersion breaking") and per-slot authored headings (mission-maker
authoring burden). See section 9.
