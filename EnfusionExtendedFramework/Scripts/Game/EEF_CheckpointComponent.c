// ============================================================
// EEF_CheckpointComponent.c
// Enfusion Extended Framework
//
// Single orchestrator component for the whole checkpoint traffic
// system. Attach to a SCR_BaseTriggerEntity placed in a World
// Editor layer - that trigger entity's ORIGIN is the checkpoint
// location itself (its sphere radius is unused in this stage,
// it only becomes relevant in #18 zone queueing).
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
// Waypoint sequence per vehicle: spawn point -> checkpoint
// origin -> despawn point, then despawn. No queueing, holding,
// zone-enter detection or player interaction - those are added
// in Stage 2 (#18), built directly on top of this same file.
//
// Runs on SERVER (authority) only.
// ============================================================

// ============================================================
// Per-vehicle lifecycle state.
//
// Stage 1 uses: SPAWNING -> APPROACHING -> DEPARTING -> DESPAWNED.
//   SPAWNING     - vehicle + occupant group spawned, waiting for
//                  the group's members to finish delayed spawning
//                  before they can be seated.
//   APPROACHING  - driver seated, driving toward the checkpoint origin.
//   DEPARTING    - driving from the checkpoint toward the despawn point.
//   DESPAWNED    - marked for removal from the active array.
//
// Stage 2 (#18) inserts QUEUED / AT_FRONT / HELD / PERMITTED /
// DENIED between APPROACHING and DEPARTING - do not add them here.
// ============================================================
enum EEF_ECheckpointVehicleState
{
	SPAWNING,
	APPROACHING,
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

//------------------------------------------------------------------------------------------------
[ComponentEditorProps(category: "EEF/Checkpoint", description: "Checkpoint traffic orchestrator - attach to a SCR_BaseTriggerEntity in a layer. Spawns vehicles that drive through the checkpoint and despawn. Stage 1: no queueing yet.")]
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

	// --------------------------------------------------------
	// Route markers (referenced by entity name in the World Editor)
	// --------------------------------------------------------

	[Attribute("", UIWidgets.EditBox, "Name of the spawn point marker entity (upstream). Vehicles spawn here.")]
	protected string m_sSpawnPointName;

	[Attribute("", UIWidgets.EditBox, "Name of the despawn/exit point marker entity (downstream). Vehicles are deleted on arrival here.")]
	protected string m_sDespawnPointName;

	// --------------------------------------------------------
	// Prefab pools
	// --------------------------------------------------------

	[Attribute("", UIWidgets.Object, "Vehicle prefab pool. One entry is picked at random per spawn.")]
	protected ref array<ref EEF_CheckpointPrefabEntry> m_aVehiclePrefabs;

	[Attribute("", UIWidgets.Object, "Occupant group prefab pool (SCR_AIGroup prefabs). One entry is picked at random per spawn - first member drives, the rest ride as cargo.")]
	protected ref array<ref EEF_CheckpointPrefabEntry> m_aOccupantGroupPrefabs;

	[Attribute("", UIWidgets.ResourcePickerThumbnail, "Move waypoint prefab used to drive vehicles to the exit. Select AIWaypoint_Move from Prefabs/AI/Waypoints/ (the same one used for Patrol/Hunter).", "et")]
	protected ResourceName m_sWaypointPrefab;

	// --------------------------------------------------------
	// Spawn cadence
	// --------------------------------------------------------

	[Attribute("20.0", UIWidgets.EditBox, "How often in seconds to attempt spawning a new vehicle.")]
	protected float m_fSpawnInterval;

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

	//! Fired with the EEF_CheckpointVehicleState just before it is removed. Later stages
	//! (e.g. #21 "stop spawning during a firefight") observe this. Lazily created.
	protected ref ScriptInvoker m_OnVehicleDespawned;

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

		GetGame().GetCallqueue().CallLater(SpawnTick, m_fSpawnInterval * 1000, true);
		GetGame().GetCallqueue().CallLater(ArrivalTick, m_fArrivalPollInterval * 1000, true);

		DebugLog("Tickers started.");
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

		// Stage 1 is pure through-traffic: drive straight to the exit with a single waypoint and
		// simply OBSERVE the checkpoint passage as the vehicle drives past. We deliberately do NOT
		// place a waypoint at the checkpoint and hand off leg-by-leg - completing a waypoint stops
		// the vehicle dead, and the AI then reverses / three-point-turns to re-path from a standstill.
		// Stage 2 (#18) is where the vehicle actually stops at the checkpoint to queue.
		AssignMoveWaypoint(state.m_OccupantGroup, m_DespawnPoint.GetOrigin());

		DebugLog("Vehicle dispatched through the checkpoint toward the exit.");
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
	//! advance it along spawn -> checkpoint -> despawn, then delete it.
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

			// Observe the checkpoint passage (state only - the waypoint is NOT reassigned here, so
			// the vehicle keeps flowing straight through toward the exit).
			if (state.m_eState == EEF_ECheckpointVehicleState.APPROACHING
				&& HasArrived(vehiclePos, GetOwner().GetOrigin()))
			{
				SetState(state, EEF_ECheckpointVehicleState.DEPARTING);
				DebugLog("Vehicle passed through the checkpoint.");
			}

			// Despawn once it reaches the exit, regardless of whether the checkpoint passage was
			// detected (the route may not run exactly over the checkpoint origin). The vehicle
			// counts its drive waypoint as complete - and therefore STOPS - up to a full completion
			// radius short of the exit marker, so the despawn radius must span that gap or the
			// vehicle halts just outside it and never despawns.
			if (HasArrivedWithin(vehiclePos, m_DespawnPoint.GetOrigin(), GetExitDespawnRadius()))
			{
				DebugLog("Vehicle reached exit point - despawning.");
				DespawnVehicle(i, true);
			}
		}
	}

	//------------------------------------------------------------------------------------------------
	// WAYPOINTS
	//------------------------------------------------------------------------------------------------

	//! Clear the group's current waypoints and give it a single move waypoint at targetPos.
	//! A member in the driver seat makes the group drive the vehicle there.
	protected void AssignMoveWaypoint(SCR_AIGroup group, vector targetPos)
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

		// A generous completion radius stops the vehicle counting the waypoint as "reached" only
		// at a pinpoint - which causes overshoot-and-reverse. It arrives smoothly instead.
		if (m_fWaypointCompletionRadius > 0)
			waypoint.SetCompletionRadius(m_fWaypointCompletionRadius);

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
		m_aVehicles.Remove(index);
		DestroyVehicleState(state, fireEvent);
	}

	//! Same as DespawnVehicle but located by state reference (used before we have an index).
	protected void DespawnVehicleState(EEF_CheckpointVehicleState state, bool fireEvent)
	{
		int index = m_aVehicles.Find(state);
		if (index != -1)
			m_aVehicles.Remove(index);

		DestroyVehicleState(state, fireEvent);
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

	//! Horizontal-only arrival test against m_fArrivalRadius.
	protected bool HasArrived(vector fromPos, vector targetPos)
	{
		return HasArrivedWithin(fromPos, targetPos, m_fArrivalRadius);
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
		state.m_eState = newState;
		state.m_fStateEnterTime = GetWorldTimeSeconds();
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
