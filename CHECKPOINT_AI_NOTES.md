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

## 4. Agreed direction
Stop delegating in-zone movement to the engine AI. Take **direct script
control** of vehicles while they are inside the checkpoint zone, using the road
network as the path source (`RoadNetworkManager`, §5) so movement traces the
road instead of cutting curves. AI handles far approach only; scripted control
takes over inside the zone.

## 5. Known API (obtained from Workbench; not fetchable in this sandbox)
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

## 6. Environment constraint
BI wiki, arexplorer, and BI forums are egress-blocked in this sandbox, so API
signatures must come from Workbench autocomplete. User tests each build by
pulling `claude/issue-16-epic-tp3hy5` into their own Workbench.
