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
//                  vehicles, driving toward the front on one continuous
//                  route until halted at its assigned gate (#23 redesign -
//                  never retasked to a separate stop marker).
//   AT_FRONT     - promoted to the front slot (slot 0), driving up to
//                  the front stop position (same continuous-route/gate
//                  mechanism as QUEUED, just slot 0's gate).
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
	float m_fLastStallLogTime;						//! World time (s) LogQueueProgress last fired for this vehicle - throttles the stall diagnostic (#23)
	vector m_LastPolledPos;							//! Vehicle position at the previous ArrivalTick poll - gate-crossing detection needs the pair (#23 redesign, restored - see notes section 12)
	bool m_bHeld;									//! True while halted in place by taking the physics body out of dynamic simulation (SimulationState.COLLISION) with the drive order still live - never cleared/re-issued, so release just re-enters simulation, not a replan from a dead stop (#23 fix, notes section 13c)

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
		m_fLastStallLogTime = spawnTime;
		m_LastPolledPos = vehicle.GetOrigin();
		m_bHeld = false;
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
// A single calculated queue gate (#23 redesign - see CHECKPOINT_AI_NOTES.md
// section 9, corrected in section 12). Replaces the old hand-authored marker
// list: computed by ComputeQueueGates() by walking the road network back
// from the checkpoint. Index 0 is the front (checkpoint stop line); higher
// indices step back toward the spawn point. m_Point + m_Forward together
// define a perpendicular line across the road at that point - every vehicle
// drives ONE continuous route toward the real end point (never retargeted
// per-gate) and is halted the instant its position crosses its own assigned
// gate's line (HasCrossedAssignedGate), independent of the AI's own
// waypoint-arrival precision, which measured data showed is unpredictable
// (3.5m-7.2m short of a requested point, worse with more distance/slower
// speed, not better - see section 12). m_Forward is the road's local
// direction of travel at m_Point (spawn side -> checkpoint).
// ============================================================
class EEF_CheckpointQueueGate
{
	vector m_Point;
	vector m_Forward;
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

	protected const float CHECKPOINT_STALL_LOG_INTERVAL = 10.0;	//! Throttle for the queue-progress stall diagnostic below

	// --------------------------------------------------------
	// Route markers (referenced by entity name in the World Editor)
	// --------------------------------------------------------

	[Attribute("", UIWidgets.EditBox, "Name of the spawn point marker entity (upstream). Vehicles spawn here.")]
	protected string m_sSpawnPointName;

	[Attribute("", UIWidgets.EditBox, "Name of the despawn/exit point marker entity (downstream). Vehicles are deleted on arrival here.")]
	protected string m_sDespawnPointName;

	// --------------------------------------------------------
	// Queue gates (#23 redesign, corrected in section 12) - queue stop points are CALCULATED, not
	// authored, so a mission maker never places or orients a single queue marker. Starting at the
	// checkpoint and walking back along the road network, ComputeQueueGates() drops a gate every
	// m_fQueueSlotSpacing metres, each with its own heading taken from the road at that exact point
	// (correct through a curve). Every vehicle drives ONE continuous route toward the real end point
	// (m_sDespawnPointName) the whole way through the queue - never retargeted to an off-road marker,
	// never retargeted per-gate either - so its heading is always whatever the road already gave it.
	// ArrivalTick halts each vehicle the instant its own position crosses its assigned gate's
	// perpendicular line (HasCrossedAssignedGate), which is a geometric test independent of the AI's
	// own waypoint-arrival behaviour. That independence is the point: three rounds of live testing
	// (section 11) showed the AI's own stopping precision for a car is not a fixed, tunable constant -
	// vehicles were measured settling anywhere from 3.5m to 7.2m short of a requested waypoint, and
	// widening the completion radius to chase that number made it worse, not better. Relying on our
	// own crossing test instead of the AI's arrival self-report sidesteps that unpredictability
	// entirely - see CHECKPOINT_AI_NOTES.md section 9 for the original design and section 12 for why
	// it's back after section 11 replaced it, and what was actually wrong with it the first time
	// (masked by a different bug, not a flaw in crossing-detection itself).
	// --------------------------------------------------------

	[Attribute("18.0", UIWidgets.EditBox, "Distance in metres between queue gates, walking back from the checkpoint along the road. Should clear the longest vehicle in the pool plus a buffer for post-crossing braking overshoot (e.g. a ~7.5m truck + ~5m clearance).")]
	protected float m_fQueueSlotSpacing;

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

	[Attribute("8.0", UIWidgets.EditBox, "Cruise speed cap (km/h) once a vehicle is inside the checkpoint zone - the hard slow-down applied the instant it crosses the trigger so it eases up to the queue instead of braking hard behind it. Lowered from 12 (section 11 second re-test) alongside the wider queue spacing/completion radius - gives the AI more reaction distance to stop cleanly behind the vehicle ahead instead of nudging/contacting it. Set <= 0 to not slow down in the zone.")]
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

	//! Calculated, ordered queue gates (index 0 = front / checkpoint stop line). Lazily computed from
	//! the road network by ComputeQueueGates(). (#23 redesign)
	protected ref array<ref EEF_CheckpointQueueGate> m_aQueueGates = new array<ref EEF_CheckpointQueueGate>();
	protected bool m_bQueueGatesComputed = false;

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

		// #23 redesign, section 12: aim at the real end point (despawn/exit marker), not the
		// checkpoint origin. The vehicle drives this ONE continuous route the whole way through -
		// approach, zone entry, queueing, all the way to the front - and is only ever halted by
		// ArrivalTick's gate-crossing check (EnterQueue does no retargeting). Aiming this far away
		// matters: it keeps the AI's own generous completion radius from ever being satisfiable
		// anywhere near the queue zone, which is what silently desynced the crossing check from
		// reality the first time this design was tried (see notes section 12) - aiming at the nearby
		// checkpoint origin let the AI call itself "arrived" and stop on its own before ever crossing
		// a gate line, so the crossing test (which only fires on an actual line crossing) never saw
		// one happen.
		AssignMoveWaypoint(state.m_OccupantGroup, m_DespawnPoint.GetOrigin(), m_fWaypointCompletionRadius);

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

				case EEF_ECheckpointVehicleState.QUEUED:
				case EEF_ECheckpointVehicleState.AT_FRONT:
				{
					// #23 redesign, section 12: the vehicle drives ONE continuous route toward the real
					// end point the whole way through (never retargeted per-gate) and is halted exactly
					// where it is the instant it crosses its currently assigned gate's perpendicular
					// line - a geometric test on the vehicle's own position, independent of whatever the
					// AI's own waypoint-arrival logic decides to do. Self-limiting: once stopped, both
					// sides of the crossing test read "past the gate" on every later poll, so this can't
					// re-fire once it has fired.
					EEF_CheckpointQueueGate gate = GetQueueGate(state.m_iQueueSlot);
					if (!gate)
						break;

					if (HasCrossedAssignedGate(state, vehiclePos))
					{
						// #23 fix (notes section 13c): halt in place by taking the physics body out of
						// dynamic simulation (SimulationState.COLLISION) WITHOUT clearing the group's
						// waypoint. The drive order toward the far despawn point - and the road-tangent
						// path the AI already computed for it while moving - stays live, so
						// promotion/release just re-enters simulation, never issues a fresh order to a
						// stopped car. Live tracing pinned "fresh waypoint from a dead stop -> ~3.75s
						// stall -> ~130-degree turn-around into reverse" as the actual release jank; not
						// clearing the order removes that path entirely.
						HoldVehicle(state);

						if (state.m_iQueueSlot == 0)
						{
							SetState(state, EEF_ECheckpointVehicleState.HELD);
							DebugLog("Front vehicle reached the stop line - HELD, awaiting release.");
							BeginHold(state);
						}
						else
						{
							DebugLog(string.Format("Vehicle reached queue slot %1 - holding.", state.m_iQueueSlot));
						}
					}
					else if (m_bDebugLog && now - state.m_fLastStallLogTime >= CHECKPOINT_STALL_LOG_INTERVAL)
					{
						// Diagnostic: still useful even with crossing-detection - shows whether a vehicle
						// that hasn't crossed its gate yet is making real progress (distance shrinking, a
						// live path) or has stalled for some other reason (e.g. genuinely blocked).
						state.m_fLastStallLogTime = now;
						LogQueueProgress(state, gate, vehiclePos);
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

				// HELD: parked at the front, waiting to be released - nothing to poll.
				// SPAWNING / DESPAWNED: handled elsewhere (seat poll / removal).
			}

			state.m_LastPolledPos = vehiclePos;
		}
	}

	//! Debug only (#23, second live-test round): a queued/front vehicle hasn't yet reached its gate -
	//! log how far short it still is plus its current AI path/request state, throttled by the caller
	//! to CHECKPOINT_STALL_LOG_INTERVAL per vehicle. Distinguishes "still driving, just far away" (0
	//! path nodes, requestCompleted=0, distance shrinking across samples) from a genuine stall
	//! (requestCompleted=1 with no new order ever taking, or a path that keeps re-routing without the
	//! distance closing - most likely single-lane contention with the vehicle ahead of it).
	protected void LogQueueProgress(EEF_CheckpointVehicleState state, EEF_CheckpointQueueGate gate, vector vehiclePos)
	{
		float dx = vehiclePos[0] - gate.m_Point[0];
		float dz = vehiclePos[2] - gate.m_Point[2];
		float dist = Math.Sqrt(dx * dx + dz * dz);

		AICarMovementComponent movement = AICarMovementComponent.Cast(
			state.m_Vehicle.FindComponent(AICarMovementComponent)
		);
		if (!movement)
		{
			DebugLog(string.Format("Slot %1 still %2m from its gate - no AICarMovementComponent to sample path.", state.m_iQueueSlot, dist));
			return;
		}

		array<vector> pts = {};
		movement.GetCurrentPath(pts);
		bool done = movement.HasCompletedRequest(false);
		DebugLog(string.Format("Slot %1 still %2m from its gate - %3 path node(s), requestCompleted=%4.", state.m_iQueueSlot, dist, pts.Count(), done));
	}

	//------------------------------------------------------------------------------------------------
	// QUEUE (Stage 2 #18)
	//
	// This component's own m_aVehicles array is the source of truth for queue order - there is no
	// native "trigger contains N vehicles, ordered" query. Each queued vehicle carries m_iQueueSlot
	// (0 = front), mapped to a calculated EEF_CheckpointQueueGate via GetQueueGate() (#23 redesign -
	// see CHECKPOINT_AI_NOTES.md section 9). When the front vehicle departs we PromoteQueue(): every
	// remaining vehicle's slot decrements and it resumes toward its new (closer) gate, so the whole
	// line advances.
	//------------------------------------------------------------------------------------------------

	//! Transition an APPROACHING vehicle into the queue: claim the lowest free slot and set QUEUED (or
	//! AT_FRONT if it took slot 0). No retargeting (#23 redesign, section 12) - it keeps driving its
	//! existing route toward the real end point; ArrivalTick's gate-crossing check halts it at the
	//! right spot. If the lane is full it stays APPROACHING and retries on the next tick (a slot frees
	//! when the front vehicle departs).
	protected void EnterQueue(EEF_CheckpointVehicleState state)
	{
		int slot = AssignQueueSlot();
		if (slot < 0)
		{
			// Lane full - hold position here and retry once a slot frees. #23 fix (notes section 13c):
			// halt by taking the physics body out of dynamic simulation, keeping the drive order live,
			// instead of clearing it - so when a slot frees the vehicle resumes by re-entering
			// simulation, not with a fresh order to a stopped car. Guarded by m_bHeld so it only acts
			// (and logs) once, not every tick.
			if (!state.m_bHeld)
			{
				DebugLog("Checkpoint zone entered but the queue is full - holding position (increase max concurrent vehicles or reduce queue slot spacing so more gates fit).");
				HoldVehicle(state);
			}
			return;
		}

		state.m_iQueueSlot = slot;

		// Unfreeze first if this vehicle was held at the zone edge waiting for a slot (no-op otherwise).
		// Then hard slow-down the instant it enters the zone so it eases up to its slot instead of
		// braking hard behind the queue. Cruise speed persists (SetCruiseSpeed is sticky) through any
		// promotion until the vehicle is released.
		ResumeVehicle(state);
		ApplyCruiseSpeed(state, m_fZoneSpeedKmh);

		// #23 redesign, section 12: no retargeting here. The vehicle is already driving toward the
		// real end point on its original Dispatch() waypoint - ArrivalTick's gate-crossing check halts
		// it the instant it reaches its assigned gate. Continuing the same route the whole way keeps
		// its heading correct instead of retasking it toward a separate stop point, and keeps the
		// AI's own arrival behaviour irrelevant to when it actually stops.
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
	//! computed gates. Returns -1 if the lane is full or no gates could be computed.
	protected int AssignQueueSlot()
	{
		ComputeQueueGates();

		int capacity = m_aQueueGates.Count();
		if (capacity == 0)
		{
			DebugLog("No queue gates available - cannot queue. Check the road network near the checkpoint (see CHECKPOINT_AI_NOTES.md section 9).");
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
	//! slots 0..n-1 - resuming each toward its new (closer or unchanged) gate. Whoever ends up in
	//! slot 0 becomes AT_FRONT and heads for the stop line. Re-packing (rather than a blind
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
			// lane - never resume it back toward a gate.
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

				ResumeTowardFront(state);
				SetState(state, EEF_ECheckpointVehicleState.AT_FRONT);
				DebugLog("Queue advanced - next vehicle promoted to the front.");
			}
			else if (slotChanged)
			{
				ResumeTowardFront(state);
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

	//! Get a halted queued vehicle moving again after a promotion (#23 fix, notes section 13c). It was
	//! halted in place by HoldVehicle (physics body taken out of dynamic simulation) when it crossed its
	//! previous gate, with its drive order toward the far despawn point STILL LIVE - so resuming is
	//! re-entering simulation (ResumeVehicle) plus re-applying the in-zone cruise speed. No fresh
	//! waypoint, no replan: the vehicle just starts driving its existing path forward again from a
	//! correct, road-tangent pose. ArrivalTick's gate-crossing check picks up its (now closer) assigned
	//! gate on the next poll and halts it there in turn.
	protected void ResumeTowardFront(EEF_CheckpointVehicleState state)
	{
		ResumeVehicle(state);
		ApplyCruiseSpeed(state, m_fZoneSpeedKmh);
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

	//! Send a held vehicle on its way: leave the queue and drive to the exit. #23 fix (notes section
	//! 13): does NOT issue a fresh waypoint. The vehicle was halted by HoldVehicle (cruise 0) with its
	//! original drive order toward the despawn point still live and its path still computed, so release
	//! is only ApplyCruiseSpeed() lifting the cap - it resumes the same order and drives its existing
	//! path forward from a correct, road-tangent pose. This is the whole point: a fresh clear+add
	//! waypoint to a stopped car is exactly what live tracing pinned as the release jank (a ~3.75s
	//! stall then a ~130-degree turn into reverse); never re-tasking a stopped car removes that path.
	protected void ReleaseVehicle(EEF_CheckpointVehicleState state)
	{
		if (!state || !state.m_Vehicle)
			return;

		state.m_iQueueSlot = -1;
		state.m_bReleasePending = false;

		SetState(state, EEF_ECheckpointVehicleState.DEPARTING);

		// Unfreeze the physics body (lift the hold) and depart at the (controlled) approach speed. The
		// vehicle's original drive order toward the despawn point was never cleared, so putting it back
		// into simulation is enough to resume it: it drives its existing, already-computed path forward
		// from a road-tangent pose. No fresh waypoint is issued (#23 fix - see the method doc above).
		ResumeVehicle(state);
		ApplyCruiseSpeed(state, m_fApproachSpeedKmh);

		DebugLog("Vehicle released - departing toward the exit.");

		// Diagnostic: confirm the resumed order is actually driving (requestCompleted should read 0 and
		// the vehicle should hold its road-tangent heading - no turn-around). Sampled across departure.
		if (m_bDebugLog)
		{
			GetGame().GetCallqueue().CallLater(DumpDeparturePath, 300, false, state);
			GetGame().GetCallqueue().CallLater(DumpDeparturePath, 1200, false, state);
			GetGame().GetCallqueue().CallLater(DumpDeparturePath, 2500, false, state);
		}

		// Advance everyone behind it now that the front slot is free.
		PromoteQueue();
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

	//------------------------------------------------------------------------------------------------
	// ZONE / SLOT HELPERS (Stage 2 #18)
	//------------------------------------------------------------------------------------------------

	//! Lazily compute the ordered queue gates from the road network exactly once (#23 redesign,
	//! replaces the old hand-authored marker list - see CHECKPOINT_AI_NOTES.md section 9). Finds the
	//! road nearest the checkpoint, then walks its point list back toward the spawn point, dropping a
	//! gate every m_fQueueSlotSpacing metres with a heading taken from the road at that exact point.
	protected void ComputeQueueGates()
	{
		if (m_bQueueGatesComputed)
			return;

		m_bQueueGatesComputed = true;
		m_aQueueGates.Clear();

		SCR_AIWorld aiWorld = SCR_AIWorld.Cast(GetGame().GetAIWorld());
		if (!aiWorld)
		{
			DebugLog("AI world unavailable - cannot compute queue gates.");
			return;
		}

		RoadNetworkManager roadMgr = aiWorld.GetRoadNetworkManager();
		if (!roadMgr)
		{
			DebugLog("Road network manager unavailable - cannot compute queue gates.");
			return;
		}

		vector checkpointPos = GetOwner().GetOrigin();

		BaseRoad road = null;
		float roadDist = 0;
		roadMgr.GetClosestRoad(checkpointPos, road, roadDist);
		if (!road)
		{
			DebugLog("No road found near the checkpoint - cannot compute queue gates.");
			return;
		}

		array<vector> points = {};
		road.GetPoints(points);
		if (points.Count() < 2)
		{
			DebugLog("Road near the checkpoint has fewer than 2 points - cannot compute queue gates.");
			return;
		}

		// Build a "back-directed" polyline: start at the checkpoint's projection onto the road, then
		// walk the road's point list toward the spawn point, accumulating distance as we go. Slot 0's
		// gate is that projected point itself (the front stop line); slot N's gate is
		// N * m_fQueueSlotSpacing further back along this polyline.
		int nearestIndex = FindNearestSegment(points, checkpointPos);
		vector originOnRoad = ProjectOnSegment(checkpointPos, points[nearestIndex], points[nearestIndex + 1]);

		array<vector> backPoints = {};
		backPoints.Insert(originOnRoad);

		vector segDir = points[nearestIndex + 1] - points[nearestIndex];
		vector toSpawn = m_SpawnPoint.GetOrigin() - checkpointPos;

		if (segDir[0] * toSpawn[0] + segDir[2] * toSpawn[2] >= 0)
		{
			// The segment already runs toward the spawn point - walk the list forward from here.
			backPoints.Insert(points[nearestIndex + 1]);
			for (int i = nearestIndex + 2; i < points.Count(); i++)
				backPoints.Insert(points[i]);
		}
		else
		{
			// The segment runs toward the checkpoint - walk the list backward instead.
			backPoints.Insert(points[nearestIndex]);
			for (int i = nearestIndex - 1; i >= 0; i--)
				backPoints.Insert(points[i]);
		}

		// Cumulative horizontal distance along backPoints, index-aligned: cumDist[i] is the distance
		// from backPoints[0] to backPoints[i].
		array<float> cumDist = {};
		cumDist.Insert(0.0);
		for (int i = 1; i < backPoints.Count(); i++)
		{
			float dx = backPoints[i][0] - backPoints[i - 1][0];
			float dz = backPoints[i][2] - backPoints[i - 1][2];
			cumDist.Insert(cumDist[i - 1] + Math.Sqrt(dx * dx + dz * dz));
		}

		float roadLength = cumDist[cumDist.Count() - 1];

		for (int slot = 0; slot < m_iMaxConcurrent; slot++)
		{
			float targetDist = slot * m_fQueueSlotSpacing;
			if (targetDist > roadLength)
			{
				DebugLog(string.Format("Road runs out %1m short for queue slot %2 - computed %3 of %4 requested gate(s). Extend the road, reduce spacing, or lower max concurrent.", targetDist - roadLength, slot, m_aQueueGates.Count(), m_iMaxConcurrent));
				break;
			}

			// Find the backPoints segment containing targetDist and interpolate within it.
			int segIndex = 0;
			for (int i = 1; i < cumDist.Count(); i++)
			{
				if (cumDist[i] >= targetDist)
				{
					segIndex = i - 1;
					break;
				}
			}

			vector segStart = backPoints[segIndex];
			vector segEnd = backPoints[segIndex + 1];
			float segLen = cumDist[segIndex + 1] - cumDist[segIndex];

			float t = 0;
			if (segLen > 0.001)
				t = (targetDist - cumDist[segIndex]) / segLen;

			vector forward = segEnd - segStart;
			forward[1] = 0;
			float forwardLen = Math.Sqrt(forward[0] * forward[0] + forward[2] * forward[2]);
			if (forwardLen > 0.001)
			{
				forward[0] = forward[0] / forwardLen;
				forward[2] = forward[2] / forwardLen;
			}

			EEF_CheckpointQueueGate gate = new EEF_CheckpointQueueGate();
			gate.m_Point = segStart + (segEnd - segStart) * t;
			// backPoints runs from the checkpoint toward the spawn point ("back"); the vehicle drives
			// the opposite way, so its expected heading at the gate is the reverse of that direction.
			gate.m_Forward = forward * -1;
			m_aQueueGates.Insert(gate);

			// Diagnostic: print each gate's exact position/heading so it can be sanity-checked in
			// Workbench (or the console log) against where vehicles actually stop.
			DebugLog(string.Format("  gate[%1] = %2, forward %3", slot, gate.m_Point, gate.m_Forward));
		}

		DebugLog(string.Format("Computed %1 of %2 requested queue gate(s), spacing %3m.", m_aQueueGates.Count(), m_iMaxConcurrent, m_fQueueSlotSpacing));
	}

	//! The gate for a queue slot index, or null if out of range. Triggers ComputeQueueGates() on
	//! first use.
	protected EEF_CheckpointQueueGate GetQueueGate(int slot)
	{
		ComputeQueueGates();

		if (slot < 0 || slot >= m_aQueueGates.Count())
			return null;

		return m_aQueueGates[slot];
	}

	//! Index of the points[] segment (points[i]..points[i+1]) whose nearest point on the segment is
	//! closest to pos, horizontal-only. points must have at least 2 entries.
	protected int FindNearestSegment(array<vector> points, vector pos)
	{
		int best = 0;
		float bestDistSq = -1;

		for (int i = 0; i < points.Count() - 1; i++)
		{
			vector proj = ProjectOnSegment(pos, points[i], points[i + 1]);
			float dx = pos[0] - proj[0];
			float dz = pos[2] - proj[2];
			float distSq = dx * dx + dz * dz;

			if (bestDistSq < 0 || distSq < bestDistSq)
			{
				bestDistSq = distSq;
				best = i;
			}
		}

		return best;
	}

	//! Horizontal-only projection of pos onto the segment a->b, clamped to the segment.
	protected vector ProjectOnSegment(vector pos, vector a, vector b)
	{
		float abx = b[0] - a[0];
		float abz = b[2] - a[2];
		float lenSq = abx * abx + abz * abz;
		if (lenSq < 0.001)
			return a;

		float t = ((pos[0] - a[0]) * abx + (pos[2] - a[2]) * abz) / lenSq;
		if (t < 0)
			t = 0;
		else if (t > 1)
			t = 1;

		vector result;
		result[0] = a[0] + abx * t;
		result[1] = a[1] + (b[1] - a[1]) * t;
		result[2] = a[2] + abz * t;
		return result;
	}

	//! True exactly once, the poll tick a queued vehicle's position crosses from the approach side of
	//! its assigned gate to the far side (#23 redesign, section 12). Horizontal-only, matching
	//! HasArrivedWithin. Self-limiting: once the vehicle has stopped past the gate, both sides of the
	//! comparison read "at or past it" on every later poll, so this only ever fires on the actual
	//! crossing - independent of the AI's own waypoint-arrival radius, which measured data showed is
	//! not a reliable proxy for "has this vehicle actually reached this point".
	protected bool HasCrossedAssignedGate(EEF_CheckpointVehicleState state, vector currPos)
	{
		EEF_CheckpointQueueGate gate = GetQueueGate(state.m_iQueueSlot);
		if (!gate)
			return false;

		float dPrev = SignedDistanceAlongGate(state.m_LastPolledPos, gate);
		float dCurr = SignedDistanceAlongGate(currPos, gate);
		return dPrev < 0 && dCurr >= 0;
	}

	//! Horizontal-only signed distance of pos along gate.m_Forward from gate.m_Point. Negative = still
	//! approaching the gate, >= 0 = at or past it.
	protected float SignedDistanceAlongGate(vector pos, EEF_CheckpointQueueGate gate)
	{
		float dx = pos[0] - gate.m_Point[0];
		float dz = pos[2] - gate.m_Point[2];
		return dx * gate.m_Forward[0] + dz * gate.m_Forward[2];
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

	//! True halt of a queued/held vehicle by taking its physics body OUT of dynamic simulation while
	//! leaving the drive order and computed path untouched (#23 fix, notes section 13c). SetCruiseSpeed(0)
	//! was confirmed by live test NOT to stop the car; this does, and keeps every property the
	//! hold-in-place design needs:
	//!   - SimulationState.COLLISION = the body is still in the collision world (a solid obstacle the
	//!     vehicle behind it stops against) but is no longer dynamically simulated, so it can neither
	//!     creep from AI throttle nor be shoved by a contact - the two failures the earlier attempts
	//!     (cruise 0, and SetActive(INACTIVE) which merely sleeps and wakes on contact) both hit.
	//!   - The waypoint and the AI's already-computed, road-tangent path live in the AI components, not
	//!     in physics, so they survive the freeze. Release (ResumeVehicle) puts the body back into
	//!     SIMULATION and the AI drives the SAME order onward from a correct pose - never the fresh
	//!     waypoint from a dead stop that live tracing pinned as the ~130-degree reverse jank.
	//! Zeroes velocity first so no stored momentum snaps back on release. Idempotent via m_bHeld.
	//! Also DEACTIVATES the occupant AI (see SetGroupAIActive): the physics freeze holds the car, but on
	//! its own it leaves the driver's behaviour tree running an unsatisfiable move order, so the AI
	//! decides it is stuck and revs / tries to reverse out (visible/audible even though the frozen body
	//! can't actually move). DeactivateAI() stops the tree issuing throttle, so the driver sits quietly.
	//! NOTE: non-recursive - freezes the vehicle's own chassis body, not its occupants. If a live test
	//! shows the wheeled simulation still nudges the car, switch the call to
	//! SCR_PhysicsHelper.ChangeSimulationState(state.m_Vehicle, SimulationState.COLLISION, true).
	protected void HoldVehicle(EEF_CheckpointVehicleState state)
	{
		if (!state || !state.m_Vehicle || state.m_bHeld)
			return;

		Physics phys = state.m_Vehicle.GetPhysics();
		if (phys)
		{
			phys.SetVelocity(vector.Zero);
			phys.SetAngularVelocity(vector.Zero);
		}

		SCR_PhysicsHelper.ChangeSimulationState(state.m_Vehicle, SimulationState.COLLISION);
		SetGroupAIActive(state, false);
		state.m_bHeld = true;
	}

	//! Reverse of HoldVehicle: put the vehicle's physics body back into dynamic SIMULATION, THEN
	//! reactivate the occupant AI so it drives on. Order matters - the body must be dynamic again before
	//! the driver's tree wakes and commands throttle. Callers pair this with ApplyCruiseSpeed() to set
	//! the resume speed (zone speed for a promotion, approach speed for a release). Idempotent via
	//! m_bHeld - a no-op on a vehicle that was never frozen (e.g. a fresh arrival taking a free slot).
	//!
	//! Reactivation RESTARTS the driver's behaviour (BT OnEnter re-issues the move request → the path is
	//! recomputed), so this is technically a replan - but the group move waypoint was never cleared, so
	//! the driver rebuilds intent straight back onto it, and it does so from the exact road-tangent pose
	//! the freeze held it at mid-route. That clean pose is the whole point: the original #23 jank was a
	//! replan from a BAD pose (arrived-imprecise / retasked); a replan from a mid-route tangent pose is
	//! the "same car follows the same curve fine once moving" case. If a live test still shows a hitch
	//! on release, the next lever is to also re-issue the move waypoint here explicitly.
	protected void ResumeVehicle(EEF_CheckpointVehicleState state)
	{
		if (!state || !state.m_Vehicle || !state.m_bHeld)
			return;

		SCR_PhysicsHelper.ChangeSimulationState(state.m_Vehicle, SimulationState.SIMULATION);
		SetGroupAIActive(state, true);
		state.m_bHeld = false;
	}

	//! Activate or deactivate the AI of every occupant agent in the group (AIAgent.ActivateAI() /
	//! DeactivateAI(), confirmed on the shipped headers). Deactivated agents suspend their behaviour
	//! trees - the driver stops issuing throttle/steer, so a held vehicle sits quietly instead of
	//! revving against the frozen body. The plan/order survives (it lives on the group + script AI
	//! components, not the activation state), so reactivation rebuilds intent from the still-present
	//! group waypoint. Deactivating the whole crew (not just the driver) avoids having to identify the
	//! pilot agent and keeps the group from reacting to a partially-disabled roster.
	protected void SetGroupAIActive(EEF_CheckpointVehicleState state, bool active)
	{
		if (!state || !state.m_OccupantGroup)
			return;

		array<AIAgent> agents = {};
		state.m_OccupantGroup.GetAgents(agents);
		foreach (AIAgent agent : agents)
		{
			if (!agent)
				continue;

			if (active)
				agent.ActivateAI();
			else
				agent.DeactivateAI();
		}
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
		// stops no longer rely on this at all (#23 redesign) - they're halted directly by
		// ArrivalTick's gate-crossing check, independent of whatever radius this waypoint has.
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
