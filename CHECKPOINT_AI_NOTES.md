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
