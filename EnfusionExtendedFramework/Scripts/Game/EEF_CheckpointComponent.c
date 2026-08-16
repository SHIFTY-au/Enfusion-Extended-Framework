// ============================================================
// EEF_CheckpointComponent.c
// Enfusion Extended Framework
//
// Single orchestrator component for the whole checkpoint traffic
// system. Attach to a SCR_BaseTriggerEntity placed in a World
// Editor layer - that trigger entity's ORIGIN is the checkpoint
// location itself and its SPHERE RADIUS defines the checkpoint
// zone: a vehicle that drives inside the radius (Stage 2) joins
// the queue.
//
// One component instance spawns and tracks MANY vehicles via an
// internal array of EEF_CheckpointVehicleState (a plain data
// class, not a per-vehicle component). This mirrors
// EEF_HunterSpawnerComponent and EEF_PatrolComponent, which both
// use the "one manager component + array of plain state objects"
// pattern rather than attaching a dedicated component to each
// spawned entity - that would require a custom variant of every
// vehicle prefab and defeats picking freely from a prefab pool.
//
// STAGE 1 (#17): the simplest possible traffic loop -
//   spawn -> populate -> drive straight through -> despawn.
//
// STAGE 2 (#18): real checkpoint behaviour -
//   spawn -> populate -> approach -> enter zone -> queue behind
//   any vehicles already waiting -> advance to the front -> hold
//   at the front until released -> depart -> despawn.
// A debug auto-release timer stands in for the player-driven
// permit/deny gate that Stage 3 (#19) will provide, so this stage
// is fully testable standalone. This stage still has NO opinion on
// *why* a vehicle is released - GetFrontVehicle() and the
// vehicle-state-changed event are exposed so #19 can drive it.
//
// Runs on SERVER (authority) only.
// ============================================================

// ============================================================
// Per-vehicle lifecycle state.
//
//   SPAWNING     - vehicle + occupant group spawned, waiting for
//                  the group's members to finish delayed spawning
//                  before they can be seated.
//   APPROACHING  - driver seated, driving toward the checkpoint zone
//                  (has not yet entered the trigger sphere).
//   QUEUED       - inside the zone, assigned a queue slot behind other
//                  vehicles, driving to / waiting at that slot marker.
//   AT_FRONT     - promoted to the front slot (slot 0), driving up to
//                  the front stop position.
//   HELD         - stopped at the front, waiting for a release decision
//                  (Stage 2: the debug auto-release timer; Stage 3: the
//                  player's permit/deny).
//   DEPARTING    - released, driving from the front toward the despawn point.
//   DESPAWNED    - marked for removal from the active array.
//
// Stage 3 (#19) adds the real permit/deny result on top of HELD/DEPARTING;
// keep any PERMITTED/DENIED distinction out of this file.
// ============================================================
enum EEF_ECheckpointVehicleState
{
	SPAWNING,
	APPROACHING,
	QUEUED,
	AT_FRONT,
	HELD,
	DEPARTING,
	DESPAWNED
}

// ============================================================
// Plain data class tracking one spawned vehicle. Held in an
// array on the component - same role as EEF_HunterGroupData /
// EEF_PatrolGroupState. NOT a component.
// ============================================================
class EEF_CheckpointVehicleState
{
	IEntity m_Vehicle;								//! The spawned vehicle entity
	SCR_AIGroup m_OccupantGroup;					//! The spawned occupant group (driver + passengers)
	EEF_ECheckpointVehicleState m_eState;			//! Current lifecycle state
	float m_fSpawnTime;								//! World time (s) the vehicle was spawned - failsafe lifetime
	float m_fStateEnterTime;						//! World time (s) the current state was entered
	bool m_bSeated;									//! True once the crew is aboard and the vehicle has been dispatched
	bool m_bHasContraband;							//! Contraband roll result - population data for #20 (placement) / interaction
	int m_iLastAgentTotal;							//! Agent count seen on the previous seat poll - detects the roster still growing
	int m_iStableSeatPolls;							//! Consecutive polls with a full, stable, fully-seated roster
	int m_iQueueSlot;								//! Queue position: 0 = front, -1 = not in the queue (Stage 2 #18)
	bool m_bReleasePending;							//! True once a release has been scheduled/requested for this vehicle, so it fires once (Stage 2 #18)

	void EEF_CheckpointVehicleState(IEntity vehicle, SCR_AIGroup group, float spawnTime, bool hasContraband)
	{
		m_Vehicle = vehicle;
		m_OccupantGroup = group;
		m_eState = EEF_ECheckpointVehicleState.SPAWNING;
		m_fSpawnTime = spawnTime;
		m_fStateEnterTime = spawnTime;
		m_bSeated = false;
		m_bHasContraband = hasContraband;
		m_iLastAgentTotal = -1;
		m_iStableSeatPolls = 0;
		m_iQueueSlot = -1;
		m_bReleasePending = false;
	}
}

// ============================================================
// A single prefab pool entry. Reused for both the vehicle pool
// and the occupant-group pool. Mirrors EEF_PatrolGroupSlot - a
// BaseContainerProps object array is the proven way to expose an
// editable prefab list in the Workbench attribute panel.
// ============================================================
[BaseContainerProps()]
class EEF_CheckpointPrefabEntry
{
	[Attribute("", UIWidgets.ResourcePickerThumbnail, "Prefab (.et)", "et")]
	ResourceName m_sPrefab;
}

// ============================================================
// A single ordered queue-slot marker (Stage 2 #18). One entry per
// physical stopping position in the queue lane. ORDER MATTERS: the
// first entry is the front of the queue (the stop line closest to
// the checkpoint), the last entry is the back. Place at least as
// many markers as "Max concurrent vehicles" so the queue never
// overflows. A plain marker/waypoint entity works - only its origin
// is used.
// ============================================================
[BaseContainerProps()]
class EEF_CheckpointQueueSlotEntry
{
	[Attribute("", UIWidgets.EditBox, "Name of a queue slot marker entity. First entry = front (checkpoint stop line), last = back of the queue.")]
	string m_sMarkerName;
}

//------------------------------------------------------------------------------------------------
[ComponentEditorProps(category: "EEF/Checkpoint", description: "Checkpoint traffic orchestrator - attach to a SCR_BaseTriggerEntity in a layer. Spawns vehicles that approach, queue in an ordered lane, hold at the front, then depart and despawn. Stage 2: a debug timer auto-releases the front vehicle (player-driven release arrives in #19).")]
class EEF_CheckpointComponentClass : ScriptComponentClass {}

//------------------------------------------------------------------------------------------------
class EEF_CheckpointComponent : ScriptComponent
{
	// Seat-poll tuning. The occupant group spawns its members staggered across many frames (leader
	// first, then the rest), so we poll fast to teleport each member the instant it appears - before
	// group cohesion makes it walk to the vehicle on foot - and only dispatch once the roster has
	// stopped growing and everyone is seated.
	protected const int CHECKPOINT_MAX_SEAT_POLLS = 80;			//! ~12s ceiling at the interval below
	protected const int CHECKPOINT_SEAT_POLL_MS = 150;
	protected const int CHECKPOINT_STABLE_POLLS_REQUIRED = 10;	//! ~1.5s of a complete, seated, unchanged roster before dispatch
	protected const int CHECKPOINT_MOPUP_STABLE_POLLS = 20;		//! ~3s settled after dispatch before we stop mopping up late members

	// Extra metres beyond the tight queue-slot completion radius within which the front vehicle is
	// treated as "arrived and stopped" at its slot. Small so HELD only fires once it has actually
	// pulled up, not while still rolling in.
	protected const float CHECKPOINT_SLOT_ARRIVAL_SLACK = 3.0;

	// Departure priming. Tasking a stopped front vehicle straight at the far exit makes the AI solve a
	// path whose first segment isn't dead ahead, so it lurches forward a few metres then re-paths. We
	// instead first send it this far straight DOWN THE ROAD (a dead-ahead target = trivial path = the
	// clean pull-away it does at spawn), then - once it is moving - retarget that same waypoint to the
	// real exit (retargeting a moving vehicle is already smooth, as at zone entry).
	protected const float CHECKPOINT_DEPART_PRIME_DIST = 40.0;	//! metres straight down the road
	protected const int CHECKPOINT_DEPART_PRIME_MS = 700;		//! let it get rolling before retargeting to the exit

	// --------------------------------------------------------
	// Route markers (referenced by entity name in the World Editor)
	// --------------------------------------------------------

	[Attribute("", UIWidgets.EditBox, "Name of the spawn point marker entity (upstream). Vehicles spawn here.")]
	protected string m_sSpawnPointName;

	[Attribute("", UIWidgets.EditBox, "Name of the despawn/exit point marker entity (downstream). Vehicles are deleted on arrival here.")]
	protected string m_sDespawnPointName;

	// --------------------------------------------------------
	// Queue lane (Stage 2 #18) - ordered stopping positions between
	// the approach and the checkpoint. First entry = front.
	// --------------------------------------------------------

	[Attribute("", UIWidgets.Object, "Ordered queue slot markers. First entry = front of the queue (checkpoint stop line), last = back. Place at least 'Max concurrent vehicles' markers so the lane never overflows.")]
	protected ref array<ref EEF_CheckpointQueueSlotEntry> m_aQueueSlotMarkers;

	// --------------------------------------------------------
	// Prefab pools
	// --------------------------------------------------------

	[Attribute("", UIWidgets.Object, "Vehicle prefab pool. One entry is picked at random per spawn.")]
	protected ref array<ref EEF_CheckpointPrefabEntry> m_aVehiclePrefabs;

	[Attribute("", UIWidgets.Object, "Occupant group prefab pool (SCR_AIGroup prefabs). One entry is picked at random per spawn - first member drives, the rest ride as cargo.")]
	protected ref array<ref EEF_CheckpointPrefabEntry> m_aOccupantGroupPrefabs;

	[Attribute("", UIWidgets.ResourcePickerThumbnail, "Move waypoint prefab used to drive vehicles to the exit. Select AIWaypoint_Move from Prefabs/AI/Waypoints/ (the same one used for Patrol/Hunter).", "et")]
	protected ResourceName m_sWaypointPrefab;

	[Attribute("1", UIWidgets.ComboBox, "AI movement effort tier applied to drive waypoints. Coarse and barely governs cars - the real speed control is the AICarMovementComponent cruise governor below. Prefer a higher tier (JOG/SPRINT) and let the cruise speeds do the capping.", "", ParamEnumArray.FromEnum(EMovementType))]
	protected EMovementType m_eMaxSpeed;

	// --------------------------------------------------------
	// Vehicle speed governor (Stage 2 #18) - the real km/h control.
	// Drives AICarMovementComponent.SetCruiseSpeed() on the vehicle,
	// which caps actual driving speed regardless of the effort tier.
	// --------------------------------------------------------

	[Attribute("35.0", UIWidgets.EditBox, "Cruise speed cap (km/h) while a vehicle is approaching or departing the checkpoint. Set <= 0 to leave the vehicle prefab's own configured cruise speed untouched.")]
	protected float m_fApproachSpeedKmh;

	[Attribute("12.0", UIWidgets.EditBox, "Cruise speed cap (km/h) once a vehicle is inside the checkpoint zone - the hard slow-down applied the instant it crosses the trigger so it eases up to the queue instead of braking hard behind it. Set <= 0 to not slow down in the zone.")]
	protected float m_fZoneSpeedKmh;

	// --------------------------------------------------------
	// Spawn cadence
	// --------------------------------------------------------

	[Attribute("15.0", UIWidgets.EditBox, "Minimum seconds between vehicle spawn attempts. The actual gap is randomised between min and max so traffic arrives at varying intervals.")]
	protected float m_fSpawnIntervalMin;

	[Attribute("30.0", UIWidgets.EditBox, "Maximum seconds between vehicle spawn attempts. The actual gap is randomised between min and max so traffic arrives at varying intervals.")]
	protected float m_fSpawnIntervalMax;

	[Attribute("3", UIWidgets.EditBox, "Maximum number of vehicles alive at any one time.")]
	protected int m_iMaxConcurrent;

	// --------------------------------------------------------
	// Population (rolled at spawn time, independent of queueing)
	// --------------------------------------------------------

	[Attribute("0.25", UIWidgets.EditBox, "Chance [0..1] a spawned vehicle is flagged as a contraband carrier. Item placement itself is Stage 4 (#20) - this stage only rolls and stores the flag.")]
	protected float m_fContrabandChance;

	// --------------------------------------------------------
	// Arrival polling (no native 'arrived' event - poll distance)
	// --------------------------------------------------------

	[Attribute("8.0", UIWidgets.EditBox, "Distance in metres from a route point at which a vehicle is considered arrived (checkpoint passage / exit despawn).")]
	protected float m_fArrivalRadius;

	[Attribute("15.0", UIWidgets.EditBox, "Completion radius in metres applied to drive waypoints. Keep this generous - a tight radius makes the AI overshoot then reverse to nail the exact point.")]
	protected float m_fWaypointCompletionRadius;

	[Attribute("2.0", UIWidgets.EditBox, "Completion radius in metres applied specifically to queue-slot drive waypoints. Keep this tight (1-2m) so queued vehicles line up neatly on their markers instead of stopping several metres short.")]
	protected float m_fQueueSlotCompletionRadius;

	[Attribute("1.0", UIWidgets.EditBox, "How often in seconds to poll vehicle positions for arrival at their current route point.")]
	protected float m_fArrivalPollInterval;

	[Attribute("300.0", UIWidgets.EditBox, "Failsafe: force-despawn a vehicle that has been alive this many seconds without completing its route (e.g. stuck or piled up).")]
	protected float m_fMaxVehicleLifetime;

	// --------------------------------------------------------
	// Release gate (Stage 2 #18 debug stand-in for #19)
	// --------------------------------------------------------

	[Attribute("8.0", UIWidgets.EditBox, "DEBUG stand-in for Stage 3 (#19): seconds a vehicle waits HELD at the front before it is auto-released. Once the real player-driven permit/deny gate exists this is bypassed. Set <= 0 to never auto-release (front vehicle waits forever - only useful with an external release caller).")]
	protected float m_fDebugAutoReleaseSeconds;

	// --------------------------------------------------------
	// Startup / debug
	// --------------------------------------------------------

	[Attribute("1", UIWidgets.CheckBox, "Automatically start the checkpoint shortly after world init. Disable to drive it purely from EEF_ScenarioFrameworkActionStartCheckpoint.")]
	protected bool m_bAutoStart;

	[Attribute("1", UIWidgets.CheckBox, "Print debug info to console. Disable in final missions.")]
	protected bool m_bDebugLog;

	// --------------------------------------------------------
	// Internal state
	// --------------------------------------------------------
	protected ref array<ref EEF_CheckpointVehicleState> m_aVehicles = new array<ref EEF_CheckpointVehicleState>();
	protected bool m_bActive = false;
	protected bool m_bTickersStarted = false;
	protected IEntity m_SpawnPoint;
	protected IEntity m_DespawnPoint;

	//! Resolved, ordered queue-slot marker entities (index 0 = front). Lazily populated from
	//! m_aQueueSlotMarkers by ResolveQueueSlots(). (Stage 2 #18)
	protected ref array<IEntity> m_aQueueSlots = new array<IEntity>();
	protected bool m_bQueueSlotsResolved = false;

	//! Cached checkpoint-zone radius (the trigger's sphere radius). A vehicle within this
	//! distance of the checkpoint origin has entered the zone. (Stage 2 #18)
	protected float m_fZoneRadius = -1;

	//! Fired with the EEF_CheckpointVehicleState just before it is removed. Later stages
	//! (e.g. #21 "stop spawning during a firefight") observe this. Lazily created.
	protected ref ScriptInvoker m_OnVehicleDespawned;

	//! Fired with the EEF_CheckpointVehicleState whenever its lifecycle state changes. Stage 3
	//! (#19 interaction) and Stage 5 (#21 hostile) observe queue-state transitions here. Lazily
	//! created. (Stage 2 #18)
	protected ref ScriptInvoker m_OnVehicleStateChanged;

	//------------------------------------------------------------------------------------------------
	// INITIALISATION
	//------------------------------------------------------------------------------------------------

	override void OnPostInit(IEntity owner)
	{
		super.OnPostInit(owner);

		if (!Replication.IsServer())
			return;

		if (!SCR_BaseTriggerEntity.Cast(owner))
			Print("[EEF Checkpoint] WARNING: Owner is not a SCR_BaseTriggerEntity. Attach this component to a trigger entity placed in a layer.", LogLevel.WARNING);

		// Defer until the world is fully initialised so marker entities and the
		// prefab pools can be resolved. Mirrors EEF_PatrolComponent's deferred start.
		GetGame().GetCallqueue().CallLater(DeferredInit, 1000, false);
	}

	//------------------------------------------------------------------------------------------------
	//! Deferred one second after world init - starts tickers and, if configured, activates.
	protected void DeferredInit()
	{
		StartTickers();

		if (m_bAutoStart)
			StartCheckpoint();
	}

	//------------------------------------------------------------------------------------------------
	//! Spin up the spawn + arrival poll tickers exactly once. The m_bActive flag gates whether
	//! they actually do work, so they can be started before activation.
	protected void StartTickers()
	{
		if (m_bTickersStarted)
			return;

		m_bTickersStarted = true;

		// Spawn timer reschedules itself with a fresh random delay each cycle (see ScheduleNextSpawn),
		// so the gap between vehicles varies. The arrival poll is a plain fixed-interval repeat.
		ScheduleNextSpawn();
		GetGame().GetCallqueue().CallLater(ArrivalTick, m_fArrivalPollInterval * 1000, true);

		DebugLog("Tickers started.");
	}

	//------------------------------------------------------------------------------------------------
	//! Queue the next SpawnTick after a random delay in [min, max] seconds. Called once at startup
	//! and again at the end of every SpawnTick, forming a self-perpetuating chain with a varying gap.
	protected void ScheduleNextSpawn()
	{
		float minSec = m_fSpawnIntervalMin;
		float maxSec = m_fSpawnIntervalMax;

		if (minSec < 0)
			minSec = 0;
		if (maxSec < minSec)
			maxSec = minSec;

		float delay = Math.RandomFloat(minSec, maxSec);
		GetGame().GetCallqueue().CallLater(SpawnTick, delay * 1000, false);
	}

	//------------------------------------------------------------------------------------------------
	// PUBLIC ACTIVATION API
	// Wired from EEF_ScenarioFrameworkActionStartCheckpoint /
	// EEF_ScenarioFrameworkActionStopCheckpoint. Mirrors the
	// EEF_HunterSpawnerComponent Start/StopHunter API.
	//------------------------------------------------------------------------------------------------

	void StartCheckpoint()
	{
		if (m_bActive)
			return;

		// If activation is driven purely by an action (auto-start disabled) the deferred
		// init may not have run yet - make sure the tickers exist.
		StartTickers();

		m_bActive = true;
		DebugLog("Checkpoint activated.");
	}

	void StopCheckpoint(bool cleanup = true)
	{
		if (!m_bActive)
			return;

		m_bActive = false;

		if (cleanup)
		{
			for (int i = m_aVehicles.Count() - 1; i >= 0; i--)
				DespawnVehicle(i, false);
		}

		DebugLog("Checkpoint deactivated.");
	}

	//------------------------------------------------------------------------------------------------
	// SPAWN TICK
	//------------------------------------------------------------------------------------------------

	protected void SpawnTick()
	{
		// Always queue the next attempt first so the varying-interval chain keeps running even when
		// this cycle skips spawning (inactive, at max, or markers unresolved).
		ScheduleNextSpawn();

		if (!m_bActive)
			return;

		CleanupDeadVehicles();

		if (GetActiveCount() >= m_iMaxConcurrent)
		{
			DebugLog(string.Format("At max concurrent vehicles (%1). Skipping spawn.", m_iMaxConcurrent));
			return;
		}

		if (!EnsurePointsResolved())
			return;

		SpawnVehicle();
	}

	//------------------------------------------------------------------------------------------------
	//! Spawn one vehicle at the spawn point, spawn its occupant group, roll contraband, and start
	//! the seat poll. Members spawn staggered, so SeatPoll keeps seating them as they appear and
	//! defers the drive waypoint until the whole crew is aboard.
	protected void SpawnVehicle()
	{
		ResourceName vehiclePrefab = PickRandomPrefab(m_aVehiclePrefabs);
		if (vehiclePrefab.IsEmpty())
		{
			DebugLog("No vehicle prefab available in the pool. Skipping spawn.");
			return;
		}

		ResourceName occupantPrefab = PickRandomPrefab(m_aOccupantGroupPrefabs);
		if (occupantPrefab.IsEmpty())
		{
			DebugLog("No occupant group prefab available in the pool. Skipping spawn.");
			return;
		}

		vector spawnPos = m_SpawnPoint.GetOrigin();
		vector checkpointPos = GetOwner().GetOrigin();

		// Spawn the vehicle facing the checkpoint so it does not immediately three-point turn.
		IEntity vehicle = SpawnPrefabFacing(vehiclePrefab, spawnPos, checkpointPos);
		if (!vehicle)
		{
			DebugLog("Failed to spawn vehicle prefab.");
			return;
		}

		SCR_AIGroup group = SCR_AIGroup.Cast(SpawnPrefabFacing(occupantPrefab, spawnPos, checkpointPos));
		if (!group)
		{
			DebugLog("Occupant prefab did not resolve to an SCR_AIGroup. Deleting orphaned vehicle.");
			SCR_EntityHelper.DeleteEntityAndChildren(vehicle);
			return;
		}

		bool hasContraband = Math.RandomFloat(0, 1) < m_fContrabandChance;

		float now = GetWorldTimeSeconds();
		EEF_CheckpointVehicleState state = new EEF_CheckpointVehicleState(vehicle, group, now, hasContraband);
		m_aVehicles.Insert(state);

		DebugLog(string.Format("Spawned vehicle at %1 (contraband: %2). Active: %3", spawnPos, hasContraband, m_aVehicles.Count()));

		// Log the vehicle's compartment layout once so a "passenger won't seat" problem is
		// diagnosable from the console (e.g. no free CARGO slots -> passenger seats are TURRET/FFV).
		LogCompartmentLayout(vehicle);

		// The occupant group spawns its members staggered - leader first, then the rest over the
		// following frames/seconds. Start a fast seat poll immediately: it teleports each member the
		// instant it appears (before group cohesion walks it to the vehicle) and only dispatches once
		// the roster has stopped growing and everyone is aboard.
		SeatPoll(state, 0);
	}

	//------------------------------------------------------------------------------------------------
	// SEATING (fast poll)
	//------------------------------------------------------------------------------------------------

	//! Repeatedly seat any un-seated group member, then dispatch once the crew is complete and stable.
	//!
	//! Members arrive staggered, so a single seating pass always misses the late ones and they walk
	//! in on foot. Instead we re-run every CHECKPOINT_SEAT_POLL_MS: seat whoever is newly present,
	//! and require CHECKPOINT_STABLE_POLLS_REQUIRED consecutive polls where the agent count has
	//! stopped growing AND everyone is in a seat before handing over the drive waypoint. This both
	//! catches late members quickly and guarantees we never dispatch mid-spawn.
	protected void SeatPoll(EEF_CheckpointVehicleState state, int attempt)
	{
		if (!state || !state.m_Vehicle || !state.m_OccupantGroup)
			return;

		// Bail if the vehicle was cleaned up (e.g. StopCheckpoint) while this poll was pending.
		if (m_aVehicles.Find(state) == -1)
			return;

		array<AIAgent> agents = {};
		state.m_OccupantGroup.GetAgents(agents);
		int total = agents.Count();

		// Teleport any member not yet in a seat. Runs on every poll - including after dispatch - so a
		// member that spawns late (the roster grows slowly and unpredictably) is snapped into a seat
		// the instant it appears instead of walking to (or chasing) the vehicle on foot.
		SeatAllAgents(state, agents);

		int seated = CountSeatedAgents(agents);
		bool rosterStable = (total > 0 && total == state.m_iLastAgentTotal);
		bool everyoneSeated = (total > 0 && seated == total);

		if (rosterStable && everyoneSeated)
			state.m_iStableSeatPolls = state.m_iStableSeatPolls + 1;
		else
			state.m_iStableSeatPolls = 0;

		state.m_iLastAgentTotal = total;

		// Dispatch once, only after the roster has been complete, seated and UNCHANGED long enough
		// that late members have almost certainly all arrived. IsInitializing() and the delayed-spawn
		// event both proved unreliable for these group prefabs (they report "done" with the roster
		// still growing), so completion is inferred from the count going quiet, not from an event.
		if (!state.m_bSeated && state.m_iStableSeatPolls >= CHECKPOINT_STABLE_POLLS_REQUIRED)
			Dispatch(state, seated, total);

		// Once dispatched AND settled for a good while, stop - no more stragglers are coming.
		if (state.m_bSeated && state.m_iStableSeatPolls >= CHECKPOINT_MOPUP_STABLE_POLLS)
			return;

		// Otherwise keep polling (and mopping up late members) until the ceiling.
		if (attempt < CHECKPOINT_MAX_SEAT_POLLS)
		{
			GetGame().GetCallqueue().CallLater(SeatPoll, CHECKPOINT_SEAT_POLL_MS, false, state, attempt + 1);
			return;
		}

		// Ceiling reached without ever dispatching - make a best-effort final call.
		if (!state.m_bSeated)
		{
			if (total == 0)
			{
				DebugLog(string.Format("Occupant group still has no agents after %1 polls - despawning vehicle. Check the group prefab has members with Spawn Immediately enabled.", attempt + 1));
				DespawnVehicleState(state, true);
				return;
			}

			if (!IsDriverSeated(state.m_Vehicle))
			{
				DebugLog("No driver could be seated (no free PILOT compartment?) - despawning vehicle.");
				DespawnVehicleState(state, true);
				return;
			}

			Dispatch(state, seated, total);
		}
	}

	//------------------------------------------------------------------------------------------------
	//! Seat every un-seated agent: the first free member into the driver seat (PILOT), the rest into
	//! any free passenger compartment (CARGO first, then TURRET as a fallback for vehicles whose
	//! passenger seats are gunner positions). Idempotent - already-seated members are skipped.
	protected void SeatAllAgents(EEF_CheckpointVehicleState state, array<AIAgent> agents)
	{
		bool driverSeated = IsDriverSeated(state.m_Vehicle);

		foreach (AIAgent agent : agents)
		{
			if (!agent)
				continue;

			IEntity character = agent.GetControlledEntity();
			if (!character)
				continue;

			// Already seated - leave them be (keeps this pass idempotent across polls).
			if (IsInAnyCompartment(character))
				continue;

			SCR_CompartmentAccessComponent access = SCR_CompartmentAccessComponent.Cast(
				character.FindComponent(SCR_CompartmentAccessComponent)
			);
			if (!access)
				continue;

			if (!driverSeated)
			{
				if (access.MoveInVehicle(state.m_Vehicle, ECompartmentType.PILOT))
					driverSeated = true;
			}
			else
			{
				// Try cargo, then gunner/turret seats if the prefab has no cargo slots.
				if (!access.MoveInVehicle(state.m_Vehicle, ECompartmentType.CARGO))
					access.MoveInVehicle(state.m_Vehicle, ECompartmentType.TURRET);
			}
		}
	}

	//------------------------------------------------------------------------------------------------
	//! Mark the crew aboard and give the group its single drive waypoint to the exit.
	protected void Dispatch(EEF_CheckpointVehicleState state, int seated, int total)
	{
		state.m_bSeated = true;

		if (seated < total)
			DebugLog(string.Format("Dispatching with %1/%2 member(s) seated - check the vehicle prefab has enough passenger seats.", seated, total));
		else
			DebugLog(string.Format("All %1 member(s) seated - dispatching.", total));

		SetState(state, EEF_ECheckpointVehicleState.APPROACHING);

		// Stage 2: drive toward the checkpoint ZONE, not straight to the exit. The vehicle keeps
		// APPROACHING until ArrivalTick sees it cross into the trigger sphere, at which point it is
		// assigned a queue slot and re-tasked to that slot marker. We aim the approach waypoint at
		// the checkpoint origin (with the generous general completion radius so it flows in smoothly)
		// - the zone-enter detection fires well before it would ever complete that waypoint.
		AssignMoveWaypoint(state.m_OccupantGroup, GetOwner().GetOrigin(), m_fWaypointCompletionRadius);

		// Govern the approach speed so vehicles don't come in hot toward the queue.
		ApplyCruiseSpeed(state, m_fApproachSpeedKmh);

		DebugLog("Vehicle dispatched - approaching the checkpoint zone.");
	}

	//------------------------------------------------------------------------------------------------
	//! True if a driver currently occupies a PILOT compartment on the vehicle.
	protected bool IsDriverSeated(IEntity vehicle)
	{
		SCR_BaseCompartmentManagerComponent compMgr = SCR_BaseCompartmentManagerComponent.Cast(
			vehicle.FindComponent(SCR_BaseCompartmentManagerComponent)
		);
		if (!compMgr)
			return false;

		array<BaseCompartmentSlot> slots = {};
		compMgr.GetCompartmentsOfType(slots, ECompartmentType.PILOT);
		foreach (BaseCompartmentSlot slot : slots)
		{
			if (slot && slot.GetOccupant())
				return true;
		}
		return false;
	}

	//------------------------------------------------------------------------------------------------
	//! True if the character currently occupies any compartment (is seated in a vehicle).
	protected bool IsInAnyCompartment(IEntity character)
	{
		SCR_CompartmentAccessComponent access = SCR_CompartmentAccessComponent.Cast(
			character.FindComponent(SCR_CompartmentAccessComponent)
		);
		return access && access.GetCompartment() != null;
	}

	//------------------------------------------------------------------------------------------------
	//! Count how many of the group's agents are currently seated in a compartment.
	protected int CountSeatedAgents(array<AIAgent> agents)
	{
		int seated = 0;
		foreach (AIAgent agent : agents)
		{
			if (!agent)
				continue;

			IEntity character = agent.GetControlledEntity();
			if (character && IsInAnyCompartment(character))
				seated++;
		}
		return seated;
	}

	//------------------------------------------------------------------------------------------------
	//! Log the vehicle's total/free compartment counts by type - diagnostics for seating issues.
	protected void LogCompartmentLayout(IEntity vehicle)
	{
		if (!m_bDebugLog)
			return;

		SCR_BaseCompartmentManagerComponent compMgr = SCR_BaseCompartmentManagerComponent.Cast(
			vehicle.FindComponent(SCR_BaseCompartmentManagerComponent)
		);
		if (!compMgr)
		{
			DebugLog("Vehicle has no SCR_BaseCompartmentManagerComponent - cannot seat occupants.");
			return;
		}

		DebugLog(string.Format("Vehicle compartments (free/total) - PILOT %1/%2, CARGO %3/%4, TURRET %5/%6",
			CountFreeCompartments(compMgr, ECompartmentType.PILOT), CountCompartments(compMgr, ECompartmentType.PILOT),
			CountFreeCompartments(compMgr, ECompartmentType.CARGO), CountCompartments(compMgr, ECompartmentType.CARGO),
			CountFreeCompartments(compMgr, ECompartmentType.TURRET), CountCompartments(compMgr, ECompartmentType.TURRET)));
	}

	protected int CountCompartments(SCR_BaseCompartmentManagerComponent compMgr, ECompartmentType type)
	{
		array<BaseCompartmentSlot> slots = {};
		compMgr.GetCompartmentsOfType(slots, type);
		return slots.Count();
	}

	protected int CountFreeCompartments(SCR_BaseCompartmentManagerComponent compMgr, ECompartmentType type)
	{
		array<BaseCompartmentSlot> slots = {};
		compMgr.GetFreeCompartmentsOfType(slots, type);
		return slots.Count();
	}

	//------------------------------------------------------------------------------------------------
	// ARRIVAL TICK
	//------------------------------------------------------------------------------------------------

	//! Poll every vehicle for arrival at its current route point (no native 'arrived' event) and
	//! advance it through the queue state machine:
	//!   APPROACHING -> (enter zone) QUEUED/AT_FRONT -> (reach front) HELD
	//!               -> (released) DEPARTING -> (reach exit) despawn.
	protected void ArrivalTick()
	{
		if (m_aVehicles.IsEmpty())
			return;

		float now = GetWorldTimeSeconds();

		for (int i = m_aVehicles.Count() - 1; i >= 0; i--)
		{
			EEF_CheckpointVehicleState state = m_aVehicles[i];

			if (!state || !state.m_Vehicle)
			{
				m_aVehicles.Remove(i);
				continue;
			}

			// Failsafe: cull vehicles that never complete their route (stuck / piled up).
			if (m_fMaxVehicleLifetime > 0 && now - state.m_fSpawnTime > m_fMaxVehicleLifetime)
			{
				DebugLog("Vehicle exceeded max lifetime - force despawning.");
				DespawnVehicle(i, true);
				continue;
			}

			vector vehiclePos = state.m_Vehicle.GetOrigin();

			switch (state.m_eState)
			{
				case EEF_ECheckpointVehicleState.APPROACHING:
				{
					// Wait until the vehicle crosses into the trigger sphere, then slot it.
					if (IsInZone(vehiclePos))
						EnterQueue(state);
					break;
				}

				case EEF_ECheckpointVehicleState.AT_FRONT:
				{
					// Once it has driven up to the front stop line, hold it for the release decision.
					if (state.m_iQueueSlot == 0 && HasArrivedAtSlot(vehiclePos, 0))
					{
						SetState(state, EEF_ECheckpointVehicleState.HELD);
						DebugLog("Front vehicle arrived at the stop line - HELD, awaiting release.");
						BeginHold(state);
					}
					break;
				}

				case EEF_ECheckpointVehicleState.DEPARTING:
				{
					// The vehicle counts its drive waypoint as complete - and therefore STOPS - up to a
					// full completion radius short of the exit marker, so the despawn radius must span
					// that gap or the vehicle halts just outside it and never despawns.
					if (HasArrivedWithin(vehiclePos, m_DespawnPoint.GetOrigin(), GetExitDespawnRadius()))
					{
						DebugLog("Vehicle reached exit point - despawning.");
						DespawnVehicle(i, true);
					}
					break;
				}

				// QUEUED / HELD: parked at a slot, waiting to be promoted or released - nothing to poll.
				// SPAWNING / DESPAWNED: handled elsewhere (seat poll / removal).
			}
		}
	}

	//------------------------------------------------------------------------------------------------
	// QUEUE (Stage 2 #18)
	//
	// This component's own m_aVehicles array is the source of truth for queue order - there is no
	// native "trigger contains N vehicles, ordered" query. Each queued vehicle carries m_iQueueSlot
	// (0 = front). When the front vehicle departs we PromoteQueue(): every remaining vehicle's slot
	// decrements and it is re-tasked to drive up to its new marker, so the whole line advances.
	//------------------------------------------------------------------------------------------------

	//! Transition an APPROACHING vehicle into the queue: claim the lowest free slot, drive it to that
	//! slot's marker, and set QUEUED (or AT_FRONT if it took slot 0). If the lane is full it stays
	//! APPROACHING and retries on the next tick (a slot frees when the front vehicle departs).
	protected void EnterQueue(EEF_CheckpointVehicleState state)
	{
		int slot = AssignQueueSlot();
		if (slot < 0)
		{
			// Lane full - hold position here and retry once a slot frees. Clearing the waypoints
			// stops the vehicle where it is instead of letting it plough into the checkpoint. Only
			// do this once (when it still has an approach waypoint) to avoid re-clearing every tick.
			if (HasWaypoints(state.m_OccupantGroup))
			{
				DebugLog("Checkpoint zone entered but the queue is full - holding position (add more queue slot markers than max concurrent vehicles).");
				ClearWaypoints(state.m_OccupantGroup);
			}
			return;
		}

		state.m_iQueueSlot = slot;
		DriveToSlot(state, slot);

		// Hard slow-down the instant it enters the zone so it eases up to its slot instead of
		// braking hard behind the queue. Persists (SetCruiseSpeed is sticky) through any promotion
		// until the vehicle is released.
		ApplyCruiseSpeed(state, m_fZoneSpeedKmh);

		if (slot == 0)
		{
			SetState(state, EEF_ECheckpointVehicleState.AT_FRONT);
			DebugLog("Vehicle entered the zone into the FRONT slot - advancing to the stop line.");
		}
		else
		{
			SetState(state, EEF_ECheckpointVehicleState.QUEUED);
			DebugLog(string.Format("Vehicle entered the zone into queue slot %1.", slot));
		}
	}

	//! Lowest queue slot index not currently claimed by another vehicle, capped at the number of
	//! authored markers. Returns -1 if the lane is full or no markers are configured.
	protected int AssignQueueSlot()
	{
		ResolveQueueSlots();

		int capacity = m_aQueueSlots.Count();
		if (capacity == 0)
		{
			DebugLog("No queue slot markers configured - cannot queue. Add queue slot markers in the component attributes.");
			return -1;
		}

		for (int slot = 0; slot < capacity; slot++)
		{
			if (!IsSlotOccupied(slot))
				return slot;
		}

		return -1;
	}

	//! True if any queued/held vehicle currently holds this slot index.
	protected bool IsSlotOccupied(int slot)
	{
		foreach (EEF_CheckpointVehicleState state : m_aVehicles)
		{
			if (state && state.m_iQueueSlot == slot)
				return true;
		}
		return false;
	}

	//! Re-pack the queue after a vehicle leaves it (released or force-despawned): collect every
	//! vehicle still waiting in the lane, order them by their current slot, and reassign compact
	//! slots 0..n-1 - re-tasking each to drive to its new (closer or unchanged) marker. Whoever ends
	//! up in slot 0 becomes AT_FRONT and heads for the stop line. Re-packing (rather than a blind
	//! decrement) closes any gap left by a middle vehicle despawning, so the line never stalls.
	protected void PromoteQueue()
	{
		// Gather the vehicles still queued, in current slot order.
		array<EEF_CheckpointVehicleState> queued = {};
		foreach (EEF_CheckpointVehicleState state : m_aVehicles)
		{
			if (!state || state.m_iQueueSlot < 0)
				continue;

			// A vehicle already DEPARTING keeps its (stale) slot until despawn but is no longer in the
			// lane - never re-task it back to a marker.
			if (state.m_eState != EEF_ECheckpointVehicleState.QUEUED
				&& state.m_eState != EEF_ECheckpointVehicleState.AT_FRONT
				&& state.m_eState != EEF_ECheckpointVehicleState.HELD)
				continue;

			InsertBySlot(queued, state);
		}

		for (int newSlot = 0; newSlot < queued.Count(); newSlot++)
		{
			EEF_CheckpointVehicleState state = queued[newSlot];

			bool slotChanged = (state.m_iQueueSlot != newSlot);
			state.m_iQueueSlot = newSlot;

			if (newSlot == 0)
			{
				// Already at the front (HELD at the stop line, or AT_FRONT driving up to it) and not
				// actually moved - leave it be rather than re-issuing the same waypoint / resetting a hold.
				bool alreadyFront = (state.m_eState == EEF_ECheckpointVehicleState.HELD
					|| state.m_eState == EEF_ECheckpointVehicleState.AT_FRONT);
				if (alreadyFront && !slotChanged)
					continue;

				DriveToSlot(state, newSlot);
				SetState(state, EEF_ECheckpointVehicleState.AT_FRONT);
				DebugLog("Queue advanced - next vehicle promoted to the front.");
			}
			else if (slotChanged)
			{
				DriveToSlot(state, newSlot);
				SetState(state, EEF_ECheckpointVehicleState.QUEUED);
				DebugLog(string.Format("Queue advanced - vehicle moved up to slot %1.", newSlot));
			}
		}
	}

	//! Insert a state into a list kept sorted ascending by m_iQueueSlot.
	protected void InsertBySlot(array<EEF_CheckpointVehicleState> list, EEF_CheckpointVehicleState state)
	{
		for (int i = 0; i < list.Count(); i++)
		{
			if (state.m_iQueueSlot < list[i].m_iQueueSlot)
			{
				list.InsertAt(state, i);
				return;
			}
		}
		list.Insert(state);
	}

	//! Re-task a vehicle's group to drive to the given slot marker, using the tight queue-slot
	//! completion radius so it stops neatly on the mark. Uses RetargetWaypoint (moves the existing
	//! waypoint) rather than clear-and-re-add, so a vehicle rolling in from the approach - or moving
	//! up as the queue advances - never loses its target and brakes to a halt in the process.
	protected void DriveToSlot(EEF_CheckpointVehicleState state, int slot)
	{
		vector slotPos;
		if (!GetSlotPosition(slot, slotPos))
			return;

		RetargetWaypoint(state.m_OccupantGroup, slotPos, m_fQueueSlotCompletionRadius);
	}

	//------------------------------------------------------------------------------------------------
	// RELEASE GATE (Stage 2 #18 debug stand-in for #19)
	//------------------------------------------------------------------------------------------------

	//! Start the debug hold timer for a vehicle that just became HELD at the front. Stage 3 (#19)
	//! replaces this with the real player permit/deny; until then a timer auto-releases the vehicle.
	protected void BeginHold(EEF_CheckpointVehicleState state)
	{
		if (m_fDebugAutoReleaseSeconds <= 0)
		{
			DebugLog("Debug auto-release disabled - front vehicle will wait for an external release call.");
			return;
		}

		state.m_bReleasePending = true;
		GetGame().GetCallqueue().CallLater(ReleaseHeldVehicle, m_fDebugAutoReleaseSeconds * 1000, false, state);
	}

	//! Debug timer callback: release the vehicle if it is still the HELD front vehicle we scheduled.
	protected void ReleaseHeldVehicle(EEF_CheckpointVehicleState state)
	{
		if (!state || m_aVehicles.Find(state) == -1)
			return;

		if (state.m_eState != EEF_ECheckpointVehicleState.HELD)
			return;

		DebugLog("Debug auto-release timer elapsed - releasing front vehicle.");
		ReleaseVehicle(state);
	}

	//! Public permit signal: release the vehicle currently held at the front, if any. This is the
	//! seam Stage 3 (#19) drives instead of the debug timer.
	void ReleaseFrontVehicle()
	{
		EEF_CheckpointVehicleState front = GetFrontVehicle();
		if (front && front.m_eState == EEF_ECheckpointVehicleState.HELD)
			ReleaseVehicle(front);
	}

	//! Send a held vehicle on its way. Departing from a standstill straight at the far exit makes the
	//! AI lurch and re-path, so we PRIME it: first aim a waypoint a good distance straight down the
	//! road (dead-ahead target = clean pull-away, like at spawn), then FinishDepart() retargets it to
	//! the real exit once it is moving.
	protected void ReleaseVehicle(EEF_CheckpointVehicleState state)
	{
		if (!state || !state.m_Vehicle)
			return;

		state.m_iQueueSlot = -1;
		state.m_bReleasePending = false;

		SetState(state, EEF_ECheckpointVehicleState.DEPARTING);

		// Lift the in-zone slow-down - depart at the (controlled) approach speed rather than flooring
		// it away from the checkpoint.
		ApplyCruiseSpeed(state, m_fApproachSpeedKmh);

		// Prime: drive straight down the road (checkpoint -> exit direction) first.
		vector primePos = state.m_Vehicle.GetOrigin() + GetDepartDirection(state) * CHECKPOINT_DEPART_PRIME_DIST;
		RetargetWaypoint(state.m_OccupantGroup, primePos, m_fWaypointCompletionRadius);
		GetGame().GetCallqueue().CallLater(FinishDepart, CHECKPOINT_DEPART_PRIME_MS, false, state);
		DebugLog("Vehicle released - priming straight ahead before turning out to the exit.");

		// Advance everyone behind it now that the front slot is free.
		PromoteQueue();
	}

	//! Second half of a departure: once the vehicle is rolling, retarget its (now moving) waypoint to
	//! the real exit. Guards against the vehicle having been despawned during the prime window.
	protected void FinishDepart(EEF_CheckpointVehicleState state)
	{
		if (!state || m_aVehicles.Find(state) == -1)
			return;

		if (state.m_eState != EEF_ECheckpointVehicleState.DEPARTING)
			return;

		RetargetWaypoint(state.m_OccupantGroup, m_DespawnPoint.GetOrigin(), m_fWaypointCompletionRadius);
		DebugLog("Departing vehicle now heading to the exit.");

		// Diagnostic: sample the AI's computed path several times across the departure. Consistently 0
		// nodes = no navmesh path at all (simple steering straight at the target); a few nodes = a
		// clean navmesh path; many tightly-spaced nodes = a jagged mesh causing constant re-steering.
		// Sampling over time rules out "path not solved yet" at any single instant.
		if (m_bDebugLog)
		{
			GetGame().GetCallqueue().CallLater(DumpDeparturePath, 300, false, state);
			GetGame().GetCallqueue().CallLater(DumpDeparturePath, 1200, false, state);
			GetGame().GetCallqueue().CallLater(DumpDeparturePath, 2500, false, state);
		}
	}

	//! Log the AI's current navmesh path for a departing vehicle (debug only) - node count is the tell:
	//! 0 = simple steering (no path), a handful = clean, many tightly-spaced = a rough mesh the vehicle
	//! keeps re-steering along. Also reports whether the movement request has completed.
	protected void DumpDeparturePath(EEF_CheckpointVehicleState state)
	{
		if (!state || m_aVehicles.Find(state) == -1 || !state.m_Vehicle)
			return;

		AICarMovementComponent movement = AICarMovementComponent.Cast(
			state.m_Vehicle.FindComponent(AICarMovementComponent)
		);
		if (!movement)
		{
			DebugLog("Departure path sample: vehicle has no AICarMovementComponent.");
			return;
		}

		array<vector> pts = {};
		movement.GetCurrentPath(pts);

		bool done = movement.HasCompletedRequest(false);
		DebugLog(string.Format("Departure path sample: %1 node(s), requestCompleted=%2 (0 nodes = simple steering / no navmesh path).", pts.Count(), done));
		foreach (int i, vector p : pts)
			DebugLog(string.Format("  path[%1] = %2", i, p));
	}

	//! Direction to pull away in on release: along the road, i.e. from the checkpoint origin toward
	//! the exit (horizontal). Falls back to the vehicle's own forward if the checkpoint and exit are
	//! effectively coincident.
	protected vector GetDepartDirection(EEF_CheckpointVehicleState state)
	{
		vector dir = m_DespawnPoint.GetOrigin() - GetOwner().GetOrigin();
		dir[1] = 0;

		float len = dir.Length();
		if (len > 0.001)
			return dir * (1.0 / len);

		// Fallback: the vehicle's forward axis (Z) flattened to horizontal.
		vector mat[4];
		state.m_Vehicle.GetTransform(mat);
		vector forward = mat[2];
		forward[1] = 0;

		len = forward.Length();
		if (len > 0.001)
			return forward * (1.0 / len);

		return "0 0 1";
	}

	//------------------------------------------------------------------------------------------------
	// ZONE / SLOT HELPERS (Stage 2 #18)
	//------------------------------------------------------------------------------------------------

	//! Resolve the ordered queue-slot marker names into entities exactly once. Missing markers are
	//! skipped with a warning, so an author typo drops one slot rather than breaking the whole lane.
	protected void ResolveQueueSlots()
	{
		if (m_bQueueSlotsResolved)
			return;

		m_bQueueSlotsResolved = true;
		m_aQueueSlots.Clear();

		if (!m_aQueueSlotMarkers)
			return;

		foreach (EEF_CheckpointQueueSlotEntry entry : m_aQueueSlotMarkers)
		{
			if (!entry || entry.m_sMarkerName.IsEmpty())
				continue;

			IEntity marker = GetGame().GetWorld().FindEntityByName(entry.m_sMarkerName);
			if (!marker)
			{
				DebugLog(string.Format("Queue slot marker '%1' not found in world - skipping.", entry.m_sMarkerName));
				continue;
			}

			m_aQueueSlots.Insert(marker);
		}

		DebugLog(string.Format("Resolved %1 queue slot marker(s).", m_aQueueSlots.Count()));
	}

	//! World position of a queue slot marker by index. Returns false if the index is out of range.
	protected bool GetSlotPosition(int slot, out vector outPos)
	{
		ResolveQueueSlots();

		if (slot < 0 || slot >= m_aQueueSlots.Count() || !m_aQueueSlots[slot])
			return false;

		outPos = m_aQueueSlots[slot].GetOrigin();
		return true;
	}

	//! True if the vehicle has reached its slot marker (stopped on the mark). Detection spans the
	//! tight completion radius plus a small fixed slack - NOT the loose general arrival radius, which
	//! would flag "at front" while the vehicle is still several metres out and rolling in.
	protected bool HasArrivedAtSlot(vector vehiclePos, int slot)
	{
		vector slotPos;
		if (!GetSlotPosition(slot, slotPos))
			return false;

		float radius = m_fQueueSlotCompletionRadius + CHECKPOINT_SLOT_ARRIVAL_SLACK;
		return HasArrivedWithin(vehiclePos, slotPos, radius);
	}

	//! True if the vehicle position is inside the checkpoint zone (trigger sphere).
	protected bool IsInZone(vector vehiclePos)
	{
		return HasArrivedWithin(vehiclePos, GetOwner().GetOrigin(), GetZoneRadius());
	}

	//! Checkpoint zone radius, cached from the owning trigger's sphere radius. Falls back to the
	//! arrival radius (treating the checkpoint as a point) with a one-time warning if the trigger
	//! has no radius set, so queueing still functions rather than silently never triggering.
	protected float GetZoneRadius()
	{
		if (m_fZoneRadius >= 0)
			return m_fZoneRadius;

		SCR_BaseTriggerEntity trigger = SCR_BaseTriggerEntity.Cast(GetOwner());
		if (trigger && trigger.GetSphereRadius() > 0)
		{
			m_fZoneRadius = trigger.GetSphereRadius();
		}
		else
		{
			m_fZoneRadius = m_fArrivalRadius;
			DebugLog(string.Format("Trigger has no sphere radius set - falling back to the arrival radius (%1m) as the checkpoint zone. Set a radius on the trigger entity for a proper queue zone.", m_fArrivalRadius));
		}

		return m_fZoneRadius;
	}

	//! True if the group still has any waypoint queued.
	protected bool HasWaypoints(SCR_AIGroup group)
	{
		if (!group)
			return false;

		array<AIWaypoint> queue = {};
		group.GetWaypoints(queue);
		return !queue.IsEmpty();
	}

	//! Remove all of a group's waypoints (stops the vehicle where it is).
	protected void ClearWaypoints(SCR_AIGroup group)
	{
		if (!group)
			return;

		array<AIWaypoint> queue = {};
		group.GetWaypoints(queue);
		foreach (AIWaypoint wp : queue)
			group.RemoveWaypoint(wp);
	}

	//! Set the vehicle's AI cruise-speed governor (km/h). kmh <= 0 restores the vehicle prefab's own
	//! configured cruise speed via ResetCruiseSpeed(). This is the real speed control - the coarse
	//! EMovementType effort tier is left on SPRINT so this governor is the limiting factor.
	protected void ApplyCruiseSpeed(EEF_CheckpointVehicleState state, float kmh)
	{
		if (!state || !state.m_Vehicle)
			return;

		AICarMovementComponent movement = AICarMovementComponent.Cast(
			state.m_Vehicle.FindComponent(AICarMovementComponent)
		);
		if (!movement)
		{
			DebugLog("Vehicle has no AICarMovementComponent - cannot govern cruise speed.");
			return;
		}

		if (kmh > 0)
			movement.SetCruiseSpeed(kmh);
		else
			movement.ResetCruiseSpeed();
	}

	//------------------------------------------------------------------------------------------------
	// WAYPOINTS
	//------------------------------------------------------------------------------------------------

	//! Clear the group's current waypoints and give it a single move waypoint at targetPos.
	//! A member in the driver seat makes the group drive the vehicle there. completionRadius < 0
	//! falls back to the general m_fWaypointCompletionRadius.
	protected void AssignMoveWaypoint(SCR_AIGroup group, vector targetPos, float completionRadius = -1)
	{
		if (!group)
			return;

		array<AIWaypoint> queue = {};
		group.GetWaypoints(queue);
		foreach (AIWaypoint wp : queue)
			group.RemoveWaypoint(wp);

		if (m_sWaypointPrefab.IsEmpty())
		{
			DebugLog("No waypoint prefab set - vehicle will not drive.");
			return;
		}

		AIWaypoint waypoint = AIWaypoint.Cast(
			GetGame().SpawnEntityPrefab(Resource.Load(m_sWaypointPrefab), GetGame().GetWorld())
		);
		if (!waypoint)
		{
			DebugLog("Failed to spawn move waypoint.");
			return;
		}

		if (completionRadius < 0)
			completionRadius = m_fWaypointCompletionRadius;

		// A generous completion radius stops the vehicle counting the waypoint as "reached" only
		// at a pinpoint - which causes overshoot-and-reverse. It arrives smoothly instead. Queue
		// slots pass a smaller radius so vehicles line up tightly at their markers.
		if (completionRadius > 0)
			waypoint.SetCompletionRadius(completionRadius);

		// Apply the speed limit as a waypoint movement-speed setting. Settings MUST be added before
		// AddWaypoint() (API requirement) - same mechanism EEF_PatrolComponent uses.
		SCR_AIWaypoint scrWaypoint = SCR_AIWaypoint.Cast(waypoint);
		if (scrWaypoint)
		{
			SCR_AIGroupCharactersMovementSpeedSetting speedSetting = SCR_AIGroupCharactersMovementSpeedSetting.Create(
				SCR_EAISettingOrigin.WAYPOINT,
				m_eMaxSpeed
			);
			if (speedSetting)
				scrWaypoint.AddSetting(speedSetting);
		}

		waypoint.SetOrigin(targetPos);
		group.AddWaypoint(waypoint);
	}

	//! Move the group's current move waypoint to targetPos instead of clearing and re-adding one.
	//! Repositioning an active waypoint keeps the vehicle driving toward it without the momentary
	//! brake-to-a-halt that removing all waypoints causes - so a vehicle entering the zone, or moving
	//! up as the queue advances, flows straight into its new slot. Falls back to AssignMoveWaypoint if
	//! the group has no waypoint to move (e.g. it already completed one and is sitting idle).
	protected void RetargetWaypoint(SCR_AIGroup group, vector targetPos, float completionRadius = -1)
	{
		if (!group)
			return;

		array<AIWaypoint> queue = {};
		group.GetWaypoints(queue);

		// Reposition the first live waypoint and drop any extras so exactly one remains.
		AIWaypoint keep = null;
		foreach (AIWaypoint wp : queue)
		{
			if (!wp)
				continue;

			if (!keep)
				keep = wp;
			else
				group.RemoveWaypoint(wp);
		}

		if (!keep)
		{
			// Nothing to move - assign a fresh waypoint (this path can briefly stop an idle vehicle,
			// but an idle vehicle is already stopped, so there is no motion to preserve).
			AssignMoveWaypoint(group, targetPos, completionRadius);
			return;
		}

		if (completionRadius < 0)
			completionRadius = m_fWaypointCompletionRadius;
		if (completionRadius > 0)
			keep.SetCompletionRadius(completionRadius);

		keep.SetOrigin(targetPos);
	}

	//------------------------------------------------------------------------------------------------
	// DESPAWN
	//------------------------------------------------------------------------------------------------

	//! Despawn the vehicle at array index i. Deletes the vehicle (and seated occupants as its
	//! children) plus the group entity, fires the despawn invoker, and removes the state.
	protected void DespawnVehicle(int index, bool fireEvent)
	{
		if (index < 0 || index >= m_aVehicles.Count())
			return;

		EEF_CheckpointVehicleState state = m_aVehicles[index];
		bool wasInLane = WasInLane(state);
		m_aVehicles.Remove(index);
		DestroyVehicleState(state, fireEvent);

		// If a queued/front vehicle vanished (e.g. failsafe cull), close the gap so the line advances.
		if (wasInLane && m_bActive)
			PromoteQueue();
	}

	//! Same as DespawnVehicle but located by state reference (used before we have an index).
	protected void DespawnVehicleState(EEF_CheckpointVehicleState state, bool fireEvent)
	{
		bool wasInLane = WasInLane(state);

		int index = m_aVehicles.Find(state);
		if (index != -1)
			m_aVehicles.Remove(index);

		DestroyVehicleState(state, fireEvent);

		if (wasInLane && m_bActive)
			PromoteQueue();
	}

	//! True if the state currently holds a queue slot in the waiting lane (not yet departing).
	protected bool WasInLane(EEF_CheckpointVehicleState state)
	{
		return state && state.m_iQueueSlot >= 0
			&& (state.m_eState == EEF_ECheckpointVehicleState.QUEUED
				|| state.m_eState == EEF_ECheckpointVehicleState.AT_FRONT
				|| state.m_eState == EEF_ECheckpointVehicleState.HELD);
	}

	//! Delete the entities backing a state and fire the despawn event. Does NOT touch the array.
	protected void DestroyVehicleState(EEF_CheckpointVehicleState state, bool fireEvent)
	{
		if (!state)
			return;

		state.m_eState = EEF_ECheckpointVehicleState.DESPAWNED;

		if (fireEvent && m_OnVehicleDespawned)
			m_OnVehicleDespawned.Invoke(state);

		// Delete the vehicle first (seated occupants go with it as children), then the group.
		if (state.m_Vehicle)
			SCR_EntityHelper.DeleteEntityAndChildren(state.m_Vehicle);

		if (state.m_OccupantGroup)
			SCR_EntityHelper.DeleteEntityAndChildren(state.m_OccupantGroup);

		state.m_Vehicle = null;
		state.m_OccupantGroup = null;
	}

	//! Drop states whose vehicle entity has been destroyed out from under us.
	protected void CleanupDeadVehicles()
	{
		for (int i = m_aVehicles.Count() - 1; i >= 0; i--)
		{
			EEF_CheckpointVehicleState state = m_aVehicles[i];
			if (!state || !state.m_Vehicle)
			{
				if (state && state.m_OccupantGroup)
					SCR_EntityHelper.DeleteEntityAndChildren(state.m_OccupantGroup);
				m_aVehicles.Remove(i);
			}
		}
	}

	//------------------------------------------------------------------------------------------------
	// HELPERS
	//------------------------------------------------------------------------------------------------

	//! Resolve and cache the spawn / despawn marker entities. Returns false (and logs once per
	//! call) if either is missing - callers skip their cycle.
	protected bool EnsurePointsResolved()
	{
		if (!m_SpawnPoint)
		{
			if (m_sSpawnPointName.IsEmpty())
			{
				DebugLog("Spawn point name is empty - set it in the component attributes.");
				return false;
			}
			m_SpawnPoint = GetGame().GetWorld().FindEntityByName(m_sSpawnPointName);
		}

		if (!m_DespawnPoint)
		{
			if (m_sDespawnPointName.IsEmpty())
			{
				DebugLog("Despawn point name is empty - set it in the component attributes.");
				return false;
			}
			m_DespawnPoint = GetGame().GetWorld().FindEntityByName(m_sDespawnPointName);
		}

		if (!m_SpawnPoint)
		{
			DebugLog(string.Format("Spawn point entity '%1' not found in world.", m_sSpawnPointName));
			return false;
		}

		if (!m_DespawnPoint)
		{
			DebugLog(string.Format("Despawn point entity '%1' not found in world.", m_sDespawnPointName));
			return false;
		}

		return true;
	}

	//! Spawn a prefab at pos, yawed to face targetPos (level - pitch/roll zeroed).
	protected IEntity SpawnPrefabFacing(ResourceName prefab, vector pos, vector targetPos)
	{
		Resource resource = Resource.Load(prefab);
		if (!resource || !resource.IsValid())
		{
			DebugLog(string.Format("Could not load prefab: %1", prefab));
			return null;
		}

		vector toTarget = targetPos - pos;
		toTarget[1] = 0;

		EntitySpawnParams spawnParams = new EntitySpawnParams();
		spawnParams.TransformMode = ETransformMode.WORLD;

		if (toTarget.LengthSq() > 0.001)
		{
			vector angles = toTarget.VectorToAngles();
			Math3D.AnglesToMatrix(angles, spawnParams.Transform);
		}
		else
		{
			Math3D.MatrixIdentity4(spawnParams.Transform);
		}

		spawnParams.Transform[3] = pos;

		return GetGame().SpawnEntityPrefab(resource, GetGame().GetWorld(), spawnParams);
	}

	//! Pick a random non-empty prefab from a pool, or ResourceName.Empty if none are usable.
	protected ResourceName PickRandomPrefab(array<ref EEF_CheckpointPrefabEntry> pool)
	{
		if (!pool || pool.IsEmpty())
			return ResourceName.Empty;

		array<ResourceName> valid = {};
		foreach (EEF_CheckpointPrefabEntry entry : pool)
		{
			if (entry && !entry.m_sPrefab.IsEmpty())
				valid.Insert(entry.m_sPrefab);
		}

		if (valid.IsEmpty())
			return ResourceName.Empty;

		return valid[Math.RandomInt(0, valid.Count())];
	}

	//! Horizontal-only arrival test against an explicit radius.
	protected bool HasArrivedWithin(vector fromPos, vector targetPos, float radius)
	{
		float dx = fromPos[0] - targetPos[0];
		float dz = fromPos[2] - targetPos[2];
		return (dx * dx + dz * dz) <= (radius * radius);
	}

	//! Radius at which a vehicle counts as arrived at the exit. The vehicle can stop up to a full
	//! completion radius short of the marker, so span both plus the arrival tolerance.
	protected float GetExitDespawnRadius()
	{
		float radius = m_fArrivalRadius;
		if (m_fWaypointCompletionRadius > radius)
			radius = m_fWaypointCompletionRadius;

		return radius + m_fArrivalRadius;
	}

	//! Number of vehicles still occupying a concurrency slot (everything not yet despawned).
	protected int GetActiveCount()
	{
		int count = 0;
		foreach (EEF_CheckpointVehicleState state : m_aVehicles)
		{
			if (state && state.m_eState != EEF_ECheckpointVehicleState.DESPAWNED)
				count++;
		}
		return count;
	}

	protected void SetState(EEF_CheckpointVehicleState state, EEF_ECheckpointVehicleState newState)
	{
		if (state.m_eState == newState)
			return;

		state.m_eState = newState;
		state.m_fStateEnterTime = GetWorldTimeSeconds();

		// Notify observers (#19 interaction, #21 hostile) of the queue-state transition.
		if (m_OnVehicleStateChanged)
			m_OnVehicleStateChanged.Invoke(state);
	}

	protected float GetWorldTimeSeconds()
	{
		return GetGame().GetWorld().GetWorldTime() / 1000.0;
	}

	//------------------------------------------------------------------------------------------------
	// ACCESSORS (read by later stages)
	//------------------------------------------------------------------------------------------------

	//! Live view of the tracked vehicle states - read by #18 (queueing), #20 (contraband), #21 (hostile).
	array<ref EEF_CheckpointVehicleState> GetVehicleStates()
	{
		return m_aVehicles;
	}

	//! Despawn event - Invoke passes the EEF_CheckpointVehicleState being removed. Lazily created.
	ScriptInvoker GetOnVehicleDespawned()
	{
		if (!m_OnVehicleDespawned)
			m_OnVehicleDespawned = new ScriptInvoker();
		return m_OnVehicleDespawned;
	}

	//! State-changed event - Invoke passes the EEF_CheckpointVehicleState whose state just changed.
	//! Read by #19 (interaction) and #21 (hostile) to react to queue transitions. Lazily created.
	ScriptInvoker GetOnVehicleStateChanged()
	{
		if (!m_OnVehicleStateChanged)
			m_OnVehicleStateChanged = new ScriptInvoker();
		return m_OnVehicleStateChanged;
	}

	//! The vehicle currently at the front of the queue (slot 0) - the one AT_FRONT driving up to the
	//! stop line or HELD waiting for release. Returns null if no vehicle is at the front. Read by #19
	//! to know which vehicle a player interaction applies to.
	EEF_CheckpointVehicleState GetFrontVehicle()
	{
		foreach (EEF_CheckpointVehicleState state : m_aVehicles)
		{
			if (state && state.m_iQueueSlot == 0
				&& (state.m_eState == EEF_ECheckpointVehicleState.AT_FRONT
					|| state.m_eState == EEF_ECheckpointVehicleState.HELD))
				return state;
		}
		return null;
	}

	bool IsCheckpointActive()
	{
		return m_bActive;
	}

	//------------------------------------------------------------------------------------------------
	protected void DebugLog(string message)
	{
		if (m_bDebugLog)
			Print("[EEF Checkpoint] " + message);
	}
}
