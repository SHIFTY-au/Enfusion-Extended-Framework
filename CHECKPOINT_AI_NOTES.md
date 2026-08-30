# BUG: AI vehicle driver janks throughout the checkpoint zone (#18 blocker)

**Status:** Driving jank FIXED (section 13, physics-hold + AI-deactivate). Prop
failure ROOT-CAUSED (section 15) as runtime **navmesh carving** by placed props,
not a driving bug — fix is asset/composition-side (author the lane, or dress with
non-carving props), no EEF_CheckpointComponent.c change required. Sole remaining
blocker to Stage 2 (#18) is authoring the checkpoint composition's drive lane.
**Branch:** `claude/arma-ai-driving-workaround-imfeex`

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

**Re-tested (2026-08-18) - large improvement, one issue remains open:**

The retarget fix works: the front vehicle now visibly drives to and stops at
the correct near-checkpoint stop line (gate 0, which sits right at the
checkpoint's own projection onto the road - so "looks like it's driving to
the checkpoint location" is the *correct*, expected result, not a bug),
"Front vehicle reached the stop line - HELD, awaiting release." now fires,
and the debug auto-release / promote / depart / despawn cycle completed
correctly at least once, confirmed end-to-end in the log (release -> "Queue
advanced" x3 -> departure path samples -> despawn -> new spawn backfilling
the freed slot).

Two things observed this round:

1. **The HELD front vehicle nudges forward a few metres when the next
   vehicle approaches from behind**, even though it has zero waypoints
   (`ClearWaypoints` already ran) and reduced cruise speed. This happens
   with no active drive order at all, so it isn't something our waypoint
   logic is issuing - it looks like Reforger's own low-level AI vehicle
   collision/obstacle avoidance reacting to a nearby dynamic obstacle,
   independent of the scripted waypoint system. No API for suppressing this
   was found. Not necessarily harmful (a car easing forward a couple of
   metres when another queues up behind it is not unrealistic), but worth
   watching in case it ever pushes a HELD/QUEUED vehicle far enough to
   destabilise a later arrival check.

2. **After the first release/promotion cycle, most subsequent front
   vehicles failed to ever reach HELD** - they sat until
   `m_fMaxVehicleLifetime` (300s) force-despawned them, and the queue only
   ever advanced via those forced despawns, not real releases. One promoted
   vehicle *did* eventually reach HELD near the end of the log, so
   `ResumeTowardFront()`'s retarget-to-own-gate isn't fundamentally broken -
   it stalls, most likely on single-lane pathing contention with the
   vehicle(s) still parked ahead of it on the same narrow road (the AI
   struggling to route past/behind a stationary vehicle directly in its
   lane, rather than smoothly queuing behind it). This is the same family of
   problem as the section 11 bug, just surfacing differently now that
   destinations are distinct instead of shared.

   Since this is very plausibly an engine-level AI pathing limitation rather
   than a logic bug in this file, and there wasn't enough log evidence to
   confirm the mechanism, no behavioural fix was attempted blind. Instead:
   `ArrivalTick()`'s QUEUED/AT_FRONT arrival handling was made idempotent
   (guarded by `HasWaypoints()`, same idiom as the "lane full" branch in
   `EnterQueue()`) so a holding non-front vehicle no longer re-logs "reached
   queue slot N - holding" and re-clears an already-empty waypoint list every
   poll forever - a real (if minor) bug, now fixed. And a new throttled
   diagnostic, `LogQueueProgress()`, samples every ~10s per vehicle while it
   is still short of its gate: distance remaining, AI path node count, and
   `HasCompletedRequest()`. This is the same instrumentation pattern as
   `DumpDeparturePath()` for departures, aimed at the QUEUED/AT_FRONT side
   instead. Next test's console log should show, for a stalled vehicle,
   whether it's making slow real progress (distance shrinking, 0 path
   nodes), stuck re-routing (path node count churns without distance
   shrinking), or never got a real order at all
   (`requestCompleted` stuck at `1`) - which determines what the actual fix
   needs to be.

**Third live test (2026-08-18) - the stall diagnostic gave a conclusive
answer.** `LogQueueProgress()` did exactly its job: every one of the four
vehicles' distance-to-gate readings converged and then held rock-steady at a
fixed, vehicle-specific, non-zero value - 4.30954m, 3.48013m, 3.49269m,
4.28298m - with `requestCompleted=1` and `0 path node(s)` every single poll
after that. These are not vehicles stuck mid-route or endlessly re-routing;
they are parked, with the AI itself considering the drive request finished,
sitting consistently 3.5m-4.3m short of the exact point `HasArrivedWithin`
was checking against. The AI's real stopping precision for a car has a floor
noticeably above the 3m `m_fQueueGateCompletionRadius` we were testing -
`SetCompletionRadius(3.0)` does not make the vehicle stop within 3m, so our
arrival check was structurally impossible to satisfy. That's the actual
cause of the "never reaches HELD" stall from the second test - not pathing
contention as guessed there.

This also explains the "collision" the user flagged this round: a stopped
vehicle drives forward a few metres, unprompted, when the next vehicle
approaches - which the user was explicit is not acceptable for a queue of
cars, in any form. One log line makes the mechanism visible: slot 2's
distance-to-gate jumped from 3.49269m to 8.75089m between two 10s samples
while still reading `0 path node(s), requestCompleted=1` (i.e. no new drive
order was ever issued to it) - it moved ~5.3m on its own while the script
had given it nothing to do. The only thing that can move a parked vehicle
with no active waypoint is a physical shove from something else - almost
certainly the vehicle behind it arriving too close, given the AI's ~4m
stopping imprecision leaves less real clearance between vehicles than the
12m nominal gate spacing implies. (The specific instance logged was a
backward shove on a mid-queue vehicle rather than the forward creep on the
HELD front vehicle the user described visually, but it is the same
insufficient-clearance mechanism - contact between vehicles that shouldn't
be able to reach each other.)

**Fix (recalibrated from the measured numbers, not guessed):**
- `m_fQueueSlotSpacing` default raised 12.0 -> 18.0m - real clearance
  between vehicles has to absorb the ~4-5m stopping imprecision on *both*
  the vehicle ahead and the vehicle behind, on top of vehicle length, or
  they end up close enough to make contact exactly as observed.
- `m_fQueueGateCompletionRadius` default raised 3.0 -> 6.0m - comfortably
  above the observed 3.48m-4.31m genuine stopping range (with margin for
  larger vehicles / other prefabs), while still well under half of the new
  18m spacing, so gates can't overlap.
- `m_fZoneSpeedKmh` default lowered 12.0 -> 8.0 km/h - more reaction
  distance for a vehicle to brake cleanly behind the one ahead instead of
  nudging/contacting it, as a second, independent lever on the same
  clearance problem.

None of this touches the retarget-to-own-gate mechanism itself (still
correct per the second test) or the heading source (still the real road) -
it only recalibrates two distance constants and one speed constant to match
measured AI behaviour instead of assumed behaviour. `LogQueueProgress()` and
the `HasWaypoints()` idempotency guard from this same entry's earlier fix
both stay in place - the diagnostic in particular is what made this
diagnosis possible and should keep running on every future test.

**Not yet re-tested** - the spacing/radius/speed recalibration above has not
had a live Workbench pass yet.

## §12. Fourth live test: recalibration disproven, reverted to gate-crossing detection (2026-08-18)

The recalibrated numbers from the entry above made things *worse*: at 18m
spacing / 6m completion radius / 8 km/h zone speed, all vehicles stopped
7.19m-7.22m short of their gate (worse than the previous test's 3.48m-4.31m,
at a *tighter* effective ratio too), still outside the new 6m radius, so
HELD still never fired and the run was stopped manually before a 4th vehicle
even entered. The user's screenshot showed the stopped vehicles' spacing was
inconsistent with what the gate math intended.

This is the conclusive data point across three tuning rounds: the AI's own
stopping precision for a car, measured against a requested waypoint, is not
a fixed constant we can converge on by raising the completion radius -
3.5m, 4.3m, 7.2m across three tests, moving in the *wrong* direction after
the spacing/speed changes that should have helped. Treating "how close can
the AI get to a point" as a tunable number was the wrong model of the
problem from the start.

**Correction (from the user, restating the original pre-section-11
design):** vehicles should path continuously toward the real END POINT
(the despawn/exit marker) - not the checkpoint's own origin, which is what
the original section 9/10 implementation actually aimed at - and a queued
vehicle is halted by our OWN geometric test: the instant its position
crosses the perpendicular line through its assigned gate (point + forward,
already computed by `ComputeQueueGates()`), independent of whatever the
AI's own waypoint-arrival/completion-radius logic thinks. This decouples
"did the vehicle reach its slot" from "does the AI consider its drive
request complete" entirely - the second thing has now been shown three
times over to be unpredictable, so nothing should depend on it for
correctness.

**Why this isn't just re-trying something already proven broken:**
`HasCrossedAssignedGate` / `SignedDistanceAlongGate` existed in the original
section 9/10 implementation and were removed in section 11 - but section
11's own diagnosis of *why* that design failed its first live test says
plainly: the drive target back then was `GetOwner().GetOrigin()` (the
checkpoint itself) with the generous `m_fWaypointCompletionRadius` (15m at
the time) - larger than the 12m gate spacing. That let the AI silently
"arrive" and stop somewhere inside the queue zone on its own, before ever
crossing a gate line, which desynced the crossing test from what the
vehicle was actually doing (it never saw a crossing because the vehicle
had already stopped short of one) - not a flaw in the crossing test itself.
The fix this time is aiming the shared route at the actual despawn/exit
point instead: far enough past every gate that the AI's own completion
radius can never be satisfied anywhere near the queue zone, so it keeps
truly driving (real forward momentum, real path) all the way through, and
the crossing test - which only fires on an actual position crossing - is
the only thing that ever halts it. This should also structurally prevent
the original section 11 symptom (a trailing vehicle swerving off-road
around the one ahead): each vehicle's own gate sits further back than the
one ahead of it, so the crossing test halts it well before it gets
physically close enough to treat the parked vehicle ahead as an obstacle.

**What changed in code:**
- `EEF_CheckpointVehicleState.m_LastPolledPos` restored (needed by the
  crossing test's prev/curr sign-flip comparison), set at the end of every
  `ArrivalTick()` poll for every vehicle.
- `Dispatch()` and `ResumeTowardFront()` now aim `AssignMoveWaypoint` at
  `m_DespawnPoint.GetOrigin()` (the real end point) instead of
  `GetOwner().GetOrigin()` (section 9/10's original target) or a per-gate
  point (section 11's target).
- `EnterQueue()` no longer retargets at all - matches section 9/10's "no
  retargeting" behaviour, now safe because the shared target is genuinely
  far away.
- `ArrivalTick()`'s QUEUED/AT_FRONT case checks `HasCrossedAssignedGate`
  instead of an arrival-radius test. The crossing test is naturally
  idempotent (both sides read "past the gate" forever after it fires), so
  the `HasWaypoints()` re-fire guard added earlier in this same round is no
  longer needed there and was dropped.
- `m_fQueueGateCompletionRadius` attribute removed entirely (no longer
  meaningful - nothing checks a radius against a gate point anymore).
- `m_fQueueSlotSpacing` (18m) and `m_fZoneSpeedKmh` (8 km/h) from the
  disproven recalibration are left as-is for now (they still provide some
  margin for real braking overshoot after a crossing, which is a much
  smaller and more bounded error source than the AI's own arrival
  imprecision was) - not re-tuned again in the same round as a mechanism
  change, to keep this test isolated to one variable.
- `LogQueueProgress()` (the stall diagnostic) is retained unchanged and
  still fires on "not yet crossed" - still useful for confirming a queued
  vehicle is making real progress toward its gate.

**Not yet re-tested** - this is a mechanism change back to gate-crossing
detection, with the specific defect from its first attempt (checkpoint
origin + radius large enough to mask a crossing) corrected. Needs a live
Workbench pass.

**Rejected explicitly - do not revisit:** direct transform heading-snap
("100% immersion breaking") and per-slot authored headings (mission-maker
authoring burden). See section 9.

## §13. Root cause pinned, fixed by holding in place instead of re-tasking (2026-08-28)

The final diagnostic trace posted to issue #23 (2026-08-18 09:47) is the
breakthrough this whole thread was missing. Instrumenting the release path
(250ms samples of position/yaw/gear/navlink/path-node-count) established, from
live data rather than theory:

- The stalling vehicle is **on the navmesh** (correction < ~0.1m throughout) and
  **has a real computed path** (39-87 nodes) once actually driving.
- `SCR_AIGroupUtilityComponent.m_OnMoveFailed` **never fires**, even on a run
  where a released vehicle physically rammed the one behind it.
- **The concrete failure:** for ~3.75s immediately after `ReleaseVehicle()`,
  the vehicle sits at an *unchanged* position with `0 path node(s)` and
  `requestCompleted=1`, then swings its heading ~130°+, **drops into reverse**
  (gear 0, confirmed by engine audio), and ends up 1-2m from where it started -
  i.e. it turns around near the stop line before driving off. This happened
  **identically on two vehicles at very different separations** (10m vs 34m
  apart), and with the despawn marker legitimately straight ahead down a road
  that curves only ~6° over the gate spacing. So it is **not** collision
  avoidance, **not** a misplaced target, **not** a hairpin, and **not**
  off-navmesh.

The one thing all of that leaves is the mechanism sections 2/3/9 already named
and section 10 then talked itself out of: **the engine AI replanning a path
from a dead stop.** `ReleaseVehicle()` (and, on the current HEAD, every
gate-stop and every promotion) did `ClearWaypoints()` then a fresh
`AssignMoveWaypoint()` on a stationary car. The AI treats "re-enter the start
of my freshly-planned path" from a standstill as needing a turn-around, and
reverses to do it. This is the exact same "fresh order from a dead stop" that
§3 ruled out for approach and §2 contrasted against "the same car follows the
same curve fine once it is already moving" - it was simply never removed from
the *stop/resume* path, only from the *approach* path.

**§9 item 4 already had the right fix ("the linchpin"): halt the vehicle in
place WITHOUT cancelling its order, and resume the SAME order.** §10 dropped
that requirement ("stop-and-cancel acceptable"), which is precisely what put
the jank back. §13 restores it, using the lever §9 item 4 flagged to check:

**What changed in `EEF_CheckpointComponent.c`:**
- New `HoldVehicle(state)` - a true halt via `AICarMovementComponent.SetCruiseSpeed(0)`
  that **does not touch the group's waypoint**. The drive order toward the far
  despawn point, and the road-tangent path already computed for it while
  moving, stay live; the car is just speed-capped to a stop. New `m_bHeld` flag
  on the state tracks this (idempotent hold, and lets `ApplyCruiseSpeed()` clear
  it on resume).
- The whole component now issues **exactly one** `AssignMoveWaypoint()` per
  vehicle, in `Dispatch()`, aimed at the despawn/exit point. Nothing clears or
  re-adds it ever again:
  - Gate crossing (`ArrivalTick` QUEUED/AT_FRONT) → `HoldVehicle()` instead of
    `ClearWaypoints()`.
  - Lane-full on zone entry (`EnterQueue`) → `HoldVehicle()` instead of
    `ClearWaypoints()`.
  - Promotion (`ResumeTowardFront`) → `ApplyCruiseSpeed(zone)` (lift the cap)
    instead of a fresh `AssignMoveWaypoint()`.
  - Release (`ReleaseVehicle`) → the `ApplyCruiseSpeed(approach)` it already did
    now *is* the resume; the fresh `AssignMoveWaypoint()` is deleted.
- Gate math, `HasCrossedAssignedGate`/`SignedDistanceAlongGate`, the far-target
  routing from section 12, spacing/speed constants, and all diagnostics are
  unchanged. This is purely "stop by governing speed, not by cancelling the
  order" - the minimal change that removes the dead-stop replan from every
  phase.

Why this should hold where three rounds of radius/spacing tuning failed:
resuming is now a pure speed change from a pose that is correct by construction
(the car was mid-route on the real road when it was capped), which is the one
case testing has *always* shown works cleanly. The AI is never handed a fresh
order from a standstill, so it has nothing to turn around for.

**The one remaining unknown - directly testable, not a compile blocker:**
whether `SetCruiseSpeed(0)` genuinely *holds* a car at zero, versus being
clamped to a minimum or treated as "unset." The pre-existing `ApplyCruiseSpeed`
mapped `kmh <= 0` to `ResetCruiseSpeed()` (prefab default) purely for attribute
semantics, so `SetCruiseSpeed(0)` itself was never actually exercised. If a live
test shows held vehicles creep forward instead of stopping, the fix is a real
halt call on `AICarMovementComponent` (check autocomplete for a `Stop`/`Halt`/
`SetWantedSpeed(0)`-style method) swapped into `HoldVehicle()` - the surrounding
"never clear the order" structure stays exactly as-is. Everything compiles and
is a no-op-safe change until that's confirmed.

**Not yet re-tested** - needs a live Workbench pass focused on: (a) do queued
vehicles actually stop and stay put at their gates, and (b) does a released
vehicle now drive straight off from the stop line with no reverse/turn-around.

### §13a. Live result (2026-08-28): `SetCruiseSpeed(0)` is NOT a stop — confirmed

The flagged unknown resolved the bad way: **`AICarMovementComponent.SetCruiseSpeed(0)`
does not stop the car.** The vehicle keeps driving, so with the current build
cars blow straight through the gates and never queue. The hold-in-place
*architecture* is unaffected and stays (one `AssignMoveWaypoint` in `Dispatch`;
gate-stop/promotion/release go through hold/resume, never clear+re-add) — only
the halt *primitive* inside `HoldVehicle()` is wrong and needs replacing.

**Do not re-attempt as a stop:** `SetCruiseSpeed(0)` / any `<= 0` cruise value
(the engine ignores it or treats it as unset).

**Candidate real-halt levers to confirm in Workbench autocomplete before coding
(don't guess-compile — one bad name burns a build):**
1. `AICarMovementComponent` members — a dedicated `Stop()` / `Halt()` /
   `SetWantedSpeed(0)` / brake-request call that pauses motion WITHOUT clearing
   the group's waypoint (the whole point is to keep the order live).
2. `SCR_CarControllerComponent` (already sampled in the diagnostic trace for
   gear) — a **persistent handbrake** setter (Reforger parks empty/AI vehicles
   with the handbrake on; likely something like `SetPersistentHandBrake(bool)`
   on the controller or the vehicle's wheeled-simulation component). A handbrake
   hold is ideal: it physically stops the car while the AI driver keeps its
   order and path, so release is just releasing the brake — exactly the
   "resume the same order, no replan" property we need.
3. A hold/wait waypoint type (e.g. an `AIWaypoint_Defend`/wait prefab) inserted
   *ahead of* the move waypoint rather than replacing it, if the group processes
   waypoints in order and resumes the move waypoint when the wait one is removed.

Preferred order: (2) handbrake, then (1), then (3). Whichever exists, it drops
straight into `HoldVehicle()` (halt) and its inverse into the resume path
(`ApplyCruiseSpeed`/`ReleaseVehicle`/`ResumeTowardFront`) — no other structural
change.

### §13b. New symptom to diagnose (2026-08-28): periodic Workbench freeze

Every few minutes the play test freezes and Workbench opens the script editor at
`CleanupDeadVehicles()` (the reverse loop that deletes an orphaned
`m_OccupantGroup` and removes the state when `!state.m_Vehicle`). Reads as a
runtime script break, not a plain hang.

**Leading theory (unconfirmed - needs the console error text):** when a vehicle
is destroyed externally (stuck/piled-up/blown), its seated driver dies with it,
the now-empty `SCR_AIGroup` auto-deletes itself, and the next `SpawnTick`'s
`CleanupDeadVehicles` then calls `SCR_EntityHelper.DeleteEntityAndChildren(state.m_OccupantGroup)`
on that group. If the reference isn't auto-nulled, that's a delete on a freed
entity. The §13a bug makes this MORE frequent: cars that don't queue drive around
chaotically and get destroyed abnormally more often, so the latent cleanup path
runs more often. **Need from Workbench:** the exact console error line at the
freeze, and whether the `for` line actually carries a user breakpoint. (§13c's
real halt should also cut the pile-ups that trigger this, so it may become rare
on its own - but still get the error line so we can fix the actual crash.)

### §13c. Halt implemented via physics simulation state (2026-08-28)

User pulled the engine's own generated script headers from `addons/core/data.pak`
(`Scripts/Core/Physics/{ActiveState,SimulationState,Physics}.c`) and confirmed the
exact API. Decisive facts:
- `IEntity.GetPhysics()` → `Physics` with `SetActive(ActiveState)`,
  `ChangeSimulationState(SimulationState)`, `SetVelocity/SetAngularVelocity`,
  `SetLinearFactor`, etc.
- `enum ActiveState { INACTIVE (sleeps), ACTIVE, ALWAYS_ACTIVE }`.
- `enum SimulationState { NONE (not in collision world), COLLISION (in collision
  world but NOT simulated), SIMULATION (dynamic, simulated) }`.
- Vanilla hierarchy wrapper: `SCR_PhysicsHelper.ChangeSimulationState(IEntity ent,
  SimulationState simState, bool recursively = false)`.

**Chosen mechanism: `SimulationState.COLLISION` as the hold.** It is exactly
right for a queue stop - the body stays a solid obstacle (the car behind stops
against it) but is not dynamically simulated, so it can neither creep from AI
throttle (the §13a `SetCruiseSpeed(0)` failure) nor be shoved by a contact (the
"nudge forward when the next car arrives" from §11). `SetActive(INACTIVE)` was
rejected: it only sleeps the body, and a contact wakes it → shove. The waypoint
and computed path live in the AI components, not physics, so they survive the
freeze; release is `ChangeSimulationState(SIMULATION)` and the AI drives the SAME
order onward from a correct pose - never the fresh-waypoint-from-a-stop that the
final trace pinned as the ~130° reverse jank.

**Code (`EEF_CheckpointComponent.c`):**
- `HoldVehicle()` - zero linear+angular velocity, then
  `SCR_PhysicsHelper.ChangeSimulationState(vehicle, SimulationState.COLLISION)`.
  Idempotent via `m_bHeld`. Non-recursive (chassis only, not occupants).
- `ResumeVehicle()` - `ChangeSimulationState(vehicle, SimulationState.SIMULATION)`,
  clears `m_bHeld`. Idempotent.
- Every hold site (gate crossing, lane-full) now calls `HoldVehicle`; every resume
  site (promotion `ResumeTowardFront`, release `ReleaseVehicle`, and a
  lane-full-then-slotted vehicle in `EnterQueue`) calls `ResumeVehicle` then
  `ApplyCruiseSpeed`. `ApplyCruiseSpeed` no longer touches `m_bHeld` - Hold/Resume
  own it, since they own the physics state.
- Still exactly one `AssignMoveWaypoint` per vehicle (in `Dispatch`); nothing
  clears or re-adds it. The architecture is unchanged - only the halt primitive
  went from the no-op `SetCruiseSpeed(0)` to the physics-state freeze.

**One knob if a live test still shows movement while held:** switch the two
`ChangeSimulationState` calls to `recursively = true` (freezes child bodies too,
e.g. the wheeled sim) - noted inline on `HoldVehicle`.

**Live result (2026-08-28): the freeze works** - queued cars stop dead at their
gates and stay put, no creep, no shove. ONE issue left (see §13d).

### §13d. Occupant AI deactivated while held (2026-08-28)

The freeze holds the car, but the driver's AI is still running an unsatisfiable
move order, so it decides it's stuck and revs / tries to reverse out - visible and
audible even though the frozen body can't move. Needed to quiet the driver.

User pulled the shipped headers again (`data007.pak`). Decisive facts:
- `AIControlComponent` / `AIAgent` both expose `ActivateAI(bool forced = true)` /
  `DeactivateAI()` / `IsAIActivated()`. (The MCP/BIKI `void ActivateAI()` is stale;
  the header wins.)
- **Reactivation RESTARTS the behaviour, it does not resume.** Movement requests
  are issued in BT task `OnEnter` (`SCR_AIFollowEntityPath` etc.), not held as a
  durable command, so when the tree runs again the request is re-issued and the
  path recomputed. Vanilla (`SCR_ChimeraAIAgent.OnLifeStateChanged`) even calls
  `comms.ClearOrders()` on reactivate and rebuilds intent from GROUP messages -
  "crucial to resume to group orders." So the **order survives** (it's a group
  waypoint) but the **path is replanned**.
- `AICarMovementComponent` has NO enable/disable/stop/output-mute - only
  `SetCruiseSpeed`/`ResetCruiseSpeed` (and `SetCruiseSpeed(0)` is already confirmed
  §13a not to stop the car). So there is no way to keep the AI active AND quiet;
  quieting it requires deactivation, which implies a replan on release.
- Handbrake (`CarControllerComponent.SetPersistentHandBrake` +
  `VehicleWheeledSimulation.SetBreak`) holds the car but the AI keeps commanding
  throttle into it (still revs) and can trip `SCR_AIRemoveStuckVehicle`. Rejected.

**Chosen: keep the physics freeze AND `DeactivateAI()` the whole crew while held;
on release unfreeze THEN `ActivateAI()`.** New `SetGroupAIActive(state, active)`
iterates the occupant group's agents. Wired into `HoldVehicle` (deactivate after
freezing) and `ResumeVehicle` (reactivate after unfreezing). This accepts the
replan on release that we spent §13 avoiding - but bets it is now harmless
*because the freeze guarantees a clean, road-tangent, mid-route pose* to replan
from, which is exactly the "same car follows the same curve fine once moving" case
(§2) rather than the bad arrived-imprecise/retasked pose the original jank hit.

**Watch on the next test:** (a) the held driver now sits quietly (no rev/reverse);
(b) on release it pulls straight away with no turn-around. If (b) still hitches,
the next lever is to also re-issue the move waypoint explicitly in `ResumeVehicle`
(reactivation replans anyway, so it costs nothing) or a short forward-point prime.

**Not yet re-tested.**

## §14. Dressing props (cones/guardrails) and AI avoidance (2026-08-29)

Placing a Game Master traffic cone in the lane made the AI drive erratically
around it. Full investigation and the resolution:

**Dead ends (confirmed, do not re-attempt):**
- **No script lever to relax AI obstacle avoidance.** `AIPathfindingComponent`
  exposes only `RayTrace`, `SetAreaCosts(ResourceName)`, `GetNavmeshComponent`,
  `GetClosestPositionOnNavmesh` - no filter/flag setter. The BT nodes
  `AITaskSetPathfindingFilters`/`AITaskResetPathfindingFilters` are native `Node`
  subclasses; the filter work is engine-side, not bound to Enforce. So the
  `RelaxVehicleObstacleAvoidance()` scaffold (a pathfinding-filter relax) could
  never work and was removed.
- `SetCruiseSpeed(0)` does not stop a car (§13a); handbrake is fought by the AI
  (§13d) - both already ruled out.

**Root cause (confirmed via MCP attribute dump of AICarMovementComponent, 39
attributes):** the AI's obstacle avoidance is a **two-stage detection cone**, not
a constant-width one. Near the bumper it fans out much wider:
- `DetectionAngle` - far-field forward cone half-width.
- `MinRangeDetectionAngle` - a SECOND, WIDER angle used within close range.
- `MinDetectionRange` - the close range inside which `MinRangeDetectionAngle`
  replaces `DetectionAngle`.
- `ObstacleAvoidanceCheckDist` - how far ahead the avoidance check reaches.
- `ObstacleAvoidanceTimer` - reaction persistence / re-eval cadence.
A cone sitting ~1 m off the driving line is outside the far-field `DetectionAngle`
but inside the wide near-field `MinRangeDetectionAngle`, so the car reacts to
something visibly not on its path.

**Fix (config, not code): a checkpoint vehicle prefab variant.** None of the 39
attributes have runtime setters, so this cannot be done from script at spawn - it
is a prefab-side change. Tune, in order:
1. Reduce `MinRangeDetectionAngle` (drops off-axis close objects like the cone,
   while the vehicle ahead in the queue - dead ahead, low angle - stays detected).
2. Then `MinDetectionRange` if needed.
3. Do NOT shrink `ObstacleAvoidanceCheckDist` / `Min Prediction Distance` - those
   cut reaction to the car in front and would undo the queue spacing tuned via
   `m_fQueueSlotSpacing`.
`Collision detection layers` / `...preset` is the surgical alternative if the prop
sits on a droppable collision layer.
Point the checkpoint vehicle pool at the tuned variant(s). Curated variants
instead of arbitrary base prefabs is the accepted cost.

**Other useful attributes noted for later:**
- `CruiseVehicleSpeedKmh` - the prefab default that `SetCruiseSpeed` overrides and
  `ResetCruiseSpeed` restores.
- `StopDistanceCoefficient` - scales computed stopping distance; a cleaner lever
  for queue clearance than capping `m_fZoneSpeedKmh` at 8, worth trialling.
- `Max Simple Steering Distance` - below this the car steers directly instead of
  using a navmesh path (explains the occasional 0-node `GetCurrentPath`).

**If curated variants are unacceptable (fully arbitrary pool required):** the only
remaining route is script-driving the zone - deactivate the AI (already done for
holds) and steer via `VehicleWheeledSimulation.SetThrottle/SetSteering/SetBreak`,
bypassing engine avoidance entirely. Bigger build; not started.

### §14a. Concrete tuning target + gotchas (2026-08-29)

Cleanest single change that needs no current-value readout: **set
`MinRangeDetectionAngle` equal to `DetectionAngle`.** That collapses the two-stage
cone into one uniform narrow forward cone - the near-field flare that catches an
off-axis cone disappears, while the vehicle dead-ahead (low angle) stays detected
so queue-following is preserved. If following degrades, nudge `MinRangeDetectionAngle`
back up slightly; if the prop is still caught, then also shrink `MinDetectionRange`.

Gotchas (from the MCP attribute dump / reference artifact):
- Attribute strings mix conventions - spaced title case (`Min Prediction Distance`)
  and camelCase (`ObstacleAvoidanceCheckDist`). Use each string EXACTLY as listed;
  they are not interchangeable.
- Enfusion MCP `wb_entity_modify`: `listProperties` needs `propertyPath`;
  `setProperty` needs both `propertyPath` (e.g. "AICarMovementComponent") and
  `propertyKey` (e.g. "MinRangeDetectionAngle"). No runtime setters exist, so
  `setProperty` on a live entity is for TESTING only - the permanent change is an
  inherited prefab variant overriding the nested AICarMovementComponent values.

### §15. ROOT CAUSE of the prop failure: navmesh carving (2026-08-29)

Two Workbench debug screenshots (navmesh + AI-path overlay) settled what the
detection-cone theory could not:
- **Clean road, no prop:** the road nav-path runs straight down the road surface.
- **The instant ANY object is dropped on the road surface:** the navmesh boundary
  (blue) pulls inward around the object and the generated AI path bends OFF the
  road - out into the grass/verge - to route around the carved hole. A single
  traffic cone does this.
- **Authored roadblock compositions do NOT:** they ship **hand-defined pathing**
  (their own nav-links / carve baked so a drivable lane survives). AI threads them
  cleanly.

So the failure was never (only) the AICarMovementComponent detection cone. The
dominant mechanism is **runtime navmesh carving**: a dynamically placed prop that
carries a navmesh obstacle cuts the walkable/drivable navmesh, and if its carve
footprint spans the lane it *severs* the lane, leaving the pathfinder no on-road
route - it goes off-road or fails. This is why:
- narrowing the detection cone (§14/§14a) helps only at the margin - it changes
  how the car reacts to an obstacle it can still route past, but cannot restore a
  navmesh route the carve deleted.
- a cone "down the middle" broke pathing entirely: two carves from the sides plus
  one in the middle sever every on-road corridor.

**Consequence for any waypoint/corridor idea:** issuing our own chain of
road-centreline waypoints (sampled from the same `road.GetPoints()` polyline
ComputeQueueGates already builds) pins the car to the road line under *mild*
navmesh perturbation, but it CANNOT drive through a *severed* lane - the car still
pathfinds between our waypoints on the carved navmesh. Corridor waypoints are a
robustness upgrade, not a fix for lane-severing carves. Do not sell them as one.

**The two real fixes (both on the asset/composition side, matching how the base
game solves it):**
1. **Dress with non-carving props.** A prop only deforms the path if it carries a
   navmesh obstacle / cut. Decorative props with no navmesh cutout (or a cutout
   small enough to leave a drivable gap) can be placed freely. Full-obstacle cones
   sever lanes; that is an asset property of the cone, not a driving bug. Pick
   dressing whose navmesh footprint leaves the lane open, or place obstacle-props
   off the driving lane.
2. **Ship the checkpoint as a composition with authored nav** - hand-placed
   nav-links / a baked navmesh cut that defines the serpentine drive lane, exactly
   like the roadblock compositions in the screenshot. Then the player's cosmetic
   dressing sits on top and the AI follows the authored lane regardless. This is
   the durable, dressing-agnostic answer and is how BI ships driveable obstacle
   layouts.

Detection-cone tuning (§14/§14a) still stands as a *secondary* lever - it makes
the car less twitchy around props it CAN route past - but it is no longer the
headline fix. The headline is: **carving props sever navmesh lanes; author the
lane (composition nav-links) or dress with non-carving props.**

### §16. CHOSEN DIRECTION: non-navmesh-affecting dressing (2026-08-30)

Two further test results from the user closed off the other routes:
- Confirmed: placing an object DOES adjust the navmesh at runtime - the road path
  re-routes around it (not just avoidance). Carving is real and dynamic.
- The pre-authored roadblock **composition is NOT reliable either** - the engine
  vehicle AI still struggles to navigate it even with hand-defined pathing. So the
  "author nav-links in a composition" route (§15 fix 2) is downgraded: authored
  nav does not rescue the flaky low-speed vehicle brain in tight clutter.

That leaves two viable ends:
1. **Script-drive the zone** (deactivate AI in the corridor, pure-pursuit steer
   along the road-centreline polyline via VehicleWheeledSimulation
   SetThrottle/SetSteering/SetBrake). Fully immune to carving + the vehicle brain.
   Bigger build; needs a Phase-1 proof-of-life first (does sim input apply with AI
   off?). Documented, not built. Fallback if route 2 is insufficient.
2. **Dress with non-navmesh-affecting props (USER'S CHOSEN DIRECTION).** If the
   dressing doesn't carve, the road path stays intact and the AI drives the clean
   line straight through - no reliance on pathing-through-clutter at all.

**The design fork route 2 forces:** a prop cannot both "not affect navmesh" AND
"be an obstacle the AI weaves around" - opposites. So, per prop:
- Cosmetic (cones/signs/clutter) -> make it NOT affect navmesh. AI ignores it,
  drives the clean road line. Realistic (a car drives over a cone).
- Real barriers (guardrails/jersey walls) -> keep them OFF the drive lane; use them
  to define the lane edges, not sever it. They may carve as long as a gap remains.

**How to author a non-navmesh-affecting prop (confirm exact names in-editor;
egress blocked here):** navmesh contribution follows the object's COLLISION, so:
1. Strip the collider entirely -> pure-visual variant (best for cones; vehicles
   pass through, nothing to carve).
2. Move its collision to a physics/interaction layer that navmesh generation
   ignores (still bumps players, doesn't carve).
3. Remove an explicit navmesh-cut/obstacle component on the variant if present.
Test rig already in hand: the navmesh debug overlay from the screenshots - drop
the variant on the road, confirm the blue boundary doesn't move.
Fastest path: overlay-test existing collision-light Reforger decorative props,
keep the ones that don't shift the navmesh; only build stripped variants where
needed.

**EEF_CheckpointComponent.c is still not implicated** by route 2 - it remains a
map-authoring/asset matter. Script-drive (route 1) is the only path that would add
component code, and only if non-carving dressing proves insufficient in practice.

**Status header updated accordingly.** The EEF_CheckpointComponent.c driving/queue
code is not implicated by this finding - it is a map-authoring / asset matter. No
code change is required for the prop issue; the optional corridor-waypoint
robustness upgrade is available on request but will not be built speculatively.
