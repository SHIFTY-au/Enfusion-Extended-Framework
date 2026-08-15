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
	bool m_bSeated;									//! True once the driver has been seated and dispatched
	bool m_bHasContraband;							//! Contraband roll result - population data for #20 (placement) / interaction
	int m_iLastAgentCount;							//! Agent count seen on the previous seat poll - used to wait for members to stop trickling in

	void EEF_CheckpointVehicleState(IEntity vehicle, SCR_AIGroup group, float spawnTime, bool hasContraband)
	{
		m_Vehicle = vehicle;
		m_OccupantGroup = group;
		m_eState = EEF_ECheckpointVehicleState.SPAWNING;
		m_fSpawnTime = spawnTime;
		m_fStateEnterTime = spawnTime;
		m_bSeated = false;
		m_bHasContraband = hasContraband;
		m_iLastAgentCount = -1;
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
	// Seating retry tuning - GetAgents() can lag the group-ready signal by a tick or two.
	protected const int CHECKPOINT_MAX_SEAT_ATTEMPTS = 20;	//! ~5s of retries at the interval below
	protected const int CHECKPOINT_SEAT_RETRY_MS = 250;

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

	[Attribute("{E37A00B4AFED7B31}Prefabs/AI/Waypoints/AIWaypoint_Move.et", UIWidgets.ResourcePickerThumbnail, "Move waypoint prefab used to drive vehicles between route points.", "et")]
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
	//! Spawn one vehicle at the spawn point, spawn its occupant group, roll contraband, and
	//! begin the seating handoff. Members spawn across several frames, so seating and the first
	//! waypoint are deferred until the group signals it is ready (OnOccupantGroupReady).
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

		// SCR_AIGroup spawns members across multiple frames via EOnFrame. Until that finishes
		// GetAgents() is empty and there is nobody to seat, so wait for the ready signal.
		if (group.IsInitializing())
		{
			group.GetOnAllDelayedEntitySpawned().Insert(OnOccupantGroupReady);
			DebugLog("Occupant group initialising - waiting for members before seating.");
		}
		else
		{
			SeatAndDispatch(state);
		}
	}

	//------------------------------------------------------------------------------------------------
	//! SCR_AIGroup.GetOnAllDelayedEntitySpawned() callback - fires once when the group's members
	//! have all spawned. Maps the group back to its state and seats it.
	protected void OnOccupantGroupReady(SCR_AIGroup group)
	{
		EEF_CheckpointVehicleState state = FindStateByGroup(group);
		if (!state)
		{
			// Stale callback - the vehicle was already cleaned up (e.g. StopCheckpoint).
			return;
		}

		SeatAndDispatch(state);
	}

	//------------------------------------------------------------------------------------------------
	//! Seat the group into the vehicle (first member drives, rest ride as cargo) and give the
	//! group its first move waypoint toward the checkpoint origin.
	//!
	//! Even after the group reports ready, GetAgents() can stay empty for a tick or two while the
	//! members finish appearing (same latency the helicopter boarding polls around), so an empty
	//! result is retried up to CHECKPOINT_MAX_SEAT_ATTEMPTS before giving up rather than despawning
	//! on the first miss.
	protected void SeatAndDispatch(EEF_CheckpointVehicleState state, int attempt = 0)
	{
		if (!state || !state.m_Vehicle || !state.m_OccupantGroup)
			return;

		if (state.m_bSeated)
			return;

		// Bail if the vehicle was cleaned up (e.g. StopCheckpoint) while this retry was pending.
		if (m_aVehicles.Find(state) == -1)
			return;

		array<AIAgent> agents = {};
		state.m_OccupantGroup.GetAgents(agents);
		int agentCount = agents.Count();

		// Wait for the group to be fully present before seating: members spawn across several
		// frames, so seat only once the count is non-zero AND unchanged since the previous poll.
		// This teleports the whole crew in one pass - otherwise a late-arriving passenger is left
		// to board on foot. The retry is still capped so a genuinely empty group can't loop forever.
		bool countStable = (agentCount > 0 && agentCount == state.m_iLastAgentCount);
		state.m_iLastAgentCount = agentCount;

		if (!countStable)
		{
			if (attempt < CHECKPOINT_MAX_SEAT_ATTEMPTS)
			{
				GetGame().GetCallqueue().CallLater(SeatAndDispatch, CHECKPOINT_SEAT_RETRY_MS, false, state, attempt + 1);
				return;
			}

			if (agentCount == 0)
			{
				DebugLog(string.Format("Occupant group still has no agents after %1 attempts - despawning vehicle. Check the group prefab has members with Spawn Immediately enabled.", attempt + 1));
				DespawnVehicleState(state, true);
				return;
			}
			// Count never settled but we do have members - seat what we have rather than stall.
			DebugLog(string.Format("Agent count did not settle after %1 attempts - seating %2 present member(s).", attempt + 1, agentCount));
		}

		bool driverSeated = false;
		foreach (AIAgent agent : agents)
		{
			if (!agent)
				continue;

			IEntity character = agent.GetControlledEntity();
			if (!character)
				continue;

			SCR_CompartmentAccessComponent access = SCR_CompartmentAccessComponent.Cast(
				character.FindComponent(SCR_CompartmentAccessComponent)
			);
			if (!access)
				continue;

			// First seated member takes the driver seat (PILOT is the driver compartment for
			// ground vehicles too); everyone else rides as cargo. MoveInVehicle teleports them
			// into the seat - no walk-and-enter.
			if (!driverSeated)
			{
				if (access.MoveInVehicle(state.m_Vehicle, ECompartmentType.PILOT))
					driverSeated = true;
			}
			else
			{
				access.MoveInVehicle(state.m_Vehicle, ECompartmentType.CARGO);
			}
		}

		if (!driverSeated)
		{
			DebugLog("Could not seat a driver (no free PILOT compartment?) - despawning vehicle.");
			DespawnVehicleState(state, true);
			return;
		}

		state.m_bSeated = true;
		SetState(state, EEF_ECheckpointVehicleState.APPROACHING);

		// Stage 1 is pure through-traffic: drive straight to the exit with a single waypoint and
		// simply OBSERVE the checkpoint passage as the vehicle drives past. We deliberately do NOT
		// place a waypoint at the checkpoint and hand off leg-by-leg - completing a waypoint stops
		// the vehicle dead, and the AI then reverses / three-point-turns to re-path from a standstill.
		// Stage 2 (#18) is where the vehicle actually stops at the checkpoint to queue.
		AssignMoveWaypoint(state.m_OccupantGroup, m_DespawnPoint.GetOrigin());

		DebugLog("Vehicle seated and dispatched through the checkpoint toward the exit.");
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
			// detected (the route may not run exactly over the checkpoint origin).
			if (HasArrived(vehiclePos, m_DespawnPoint.GetOrigin()))
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
		float dx = fromPos[0] - targetPos[0];
		float dz = fromPos[2] - targetPos[2];
		return (dx * dx + dz * dz) <= (m_fArrivalRadius * m_fArrivalRadius);
	}

	protected EEF_CheckpointVehicleState FindStateByGroup(SCR_AIGroup group)
	{
		foreach (EEF_CheckpointVehicleState state : m_aVehicles)
		{
			if (state && state.m_OccupantGroup == group)
				return state;
		}
		return null;
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
