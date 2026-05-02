// ============================================================
// EEF_HelicopterInsertionComponent.c
// Enfusion Extended Framework
//
// Self-contained helicopter insertion component. Spawns a
// troop group at runtime, seats them via MoveInVehicle into
// the helicopter's CARGO compartments, flies to a
// mission-maker-placed LZ, disembarks the troops, and hands
// the disembarked group off to any downstream EEF module
// extending EEF_GroupReceiverComponent.
//
// Attaches to: a helicopter vehicle entity in the World Editor.
// Activated by: EEF_ScenarioFrameworkActionStartHelicopterInsertion.
// Runs on: SERVER (authority) only.
//
// ------------------------------------------------------------
// IMPLEMENTATION STATUS
// ------------------------------------------------------------
// IMPLEMENTED:
//   IDLE -> BOARDING -> FLYING -> INSERTING -> POST_INSERTION (STAY | DEPART)
//
//   Flight controller: waypoint-based navigation with heading-driven velocity,
//   dynamic rotor force multiplier for native lift, bank/pitch/rollback angular
//   velocity, terrain avoidance via forward surface-Y raycast.
//
// PLANNED (next polish pass):
//   - Refined disembark placement
//   - Physics raycast for true obstacle detection (currently surface-Y approximation)
// ============================================================

// ============================================================
// ENUMS — full set per design doc, including states the flight
// pass will activate later.
// ============================================================

//! Where the helicopter starts.
enum EEF_EHelicopterSpawnMode
{
	GROUND,		//! Placed on ground in editor. Lifts off when activated.
	IN_AIR		//! Spawns at cruise altitude AGL above its placed position. Begins cruise immediately.
}

//! How troops are delivered at the LZ.
enum EEF_EHelicopterLandingMode
{
	FULL_LANDING,	//! Helicopter touches down. Troops exit and walk away.
	HOVER_LANDING	//! Helicopter holds at hover altitude AGL. Troops exit and drop.
}

//! What the helicopter does after troops are delivered.
enum EEF_EPostInsertionMode
{
	STAY,	//! Helicopter holds position indefinitely. Physics suspended.
	DEPART	//! Helicopter climbs and flies off-map after a delay.
}

//! Internal state machine.
enum EEF_EHelicopterInsertionState
{
	IDLE,			//! Waiting for SF activation.
	BOARDING,		//! Spawning cargo group, seating via MoveInVehicle.
	FLYING,			//! Progressing through waypoints to LZ (climb, cruise, approach).
	INSERTING,		//! On ground or hovering — disembarking troops.
	POST_INSERTION	//! Staying or departing.
}

[ComponentEditorProps(category: "EEF/Helicopter", description: "EEF Helicopter Insertion - attach to a helicopter entity. Spawns a troop group, flies to an LZ, disembarks, and hands off to a downstream EEF receiver. Activated via EEF_ScenarioFrameworkActionStartHelicopterInsertion.")]
class EEF_HelicopterInsertionComponentClass : ScriptComponentClass {}

class EEF_HelicopterInsertionComponent : ScriptComponent
{
	// --------------------------------------------------------
	// SPAWN
	// --------------------------------------------------------

	[Attribute(EEF_EHelicopterSpawnMode.GROUND.ToString(), UIWidgets.ComboBox, "Whether the helicopter starts on the ground or spawns in-air.", "", ParamEnumArray.FromEnum(EEF_EHelicopterSpawnMode))]
	protected EEF_EHelicopterSpawnMode m_eSpawnMode;

	[Attribute("", UIWidgets.ResourcePickerThumbnail, "AI group prefab to spawn as cargo.", "et")]
	protected ResourceName m_sGroupPrefab;

	[Attribute("", UIWidgets.ResourcePickerThumbnail, "Pilot character prefab to spawn in the pilot seat. Leave empty to skip crew spawning.", "et")]
	protected ResourceName m_sPilotPrefab;

	// --------------------------------------------------------
	// FLIGHT
	// --------------------------------------------------------

	[Attribute("80.0", UIWidgets.EditBox, "Cruise altitude in metres above ground level.")]
	protected float m_fCruiseAltitudeAGL;

	[Attribute("30.0", UIWidgets.EditBox, "Cruise speed in m/s toward LZ.")]
	protected float m_fCruiseSpeed;

	[Attribute("10.0", UIWidgets.EditBox, "Reduced speed during LZ approach.")]
	protected float m_fApproachSpeed;

	[Attribute("45.0", UIWidgets.EditBox, "Max degrees per second for heading alignment.")]
	protected float m_fRotationSpeed;

	// --------------------------------------------------------
	// LZ
	// --------------------------------------------------------

	[Attribute("", UIWidgets.EditBox, "Name of the LZ entity placed in the World Editor. The helicopter flies to this entity's origin and looks up the EEF_GroupReceiverComponent on it for handoff.")]
	protected string m_sLZEntityName;

	[Attribute(EEF_EHelicopterLandingMode.FULL_LANDING.ToString(), UIWidgets.ComboBox, "How troops are delivered at the LZ.", "", ParamEnumArray.FromEnum(EEF_EHelicopterLandingMode))]
	protected EEF_EHelicopterLandingMode m_eLandingMode;

	[Attribute("4.0", UIWidgets.EditBox, "AGL hover height for HOVER_LANDING mode.")]
	protected float m_fHoverAltitude;

	[Attribute("50.0", UIWidgets.EditBox, "Distance in metres from LZ at which the helicopter begins its final approach.")]
	protected float m_fApproachRadius;

	[Attribute("", UIWidgets.EditBox, "Optional intermediate waypoint 1 (world entity name). Helicopter passes through at cruise altitude.")]
	protected string m_sWaypoint1Name;

	[Attribute("", UIWidgets.EditBox, "Optional intermediate waypoint 2 (world entity name).")]
	protected string m_sWaypoint2Name;

	[Attribute("", UIWidgets.EditBox, "Optional intermediate waypoint 3 (world entity name).")]
	protected string m_sWaypoint3Name;

	[Attribute("15.0", UIWidgets.EditBox, "Horizontal distance (m) within which an intermediate waypoint is considered reached.")]
	protected float m_fWaypointArrivalTolerance;

	// --------------------------------------------------------
	// POST-INSERTION
	// --------------------------------------------------------

	[Attribute(EEF_EPostInsertionMode.DEPART.ToString(), UIWidgets.ComboBox, "What the helicopter does after troops disembark.", "", ParamEnumArray.FromEnum(EEF_EPostInsertionMode))]
	protected EEF_EPostInsertionMode m_ePostInsertionMode;

	[Attribute("5.0", UIWidgets.EditBox, "Seconds after disembark confirmed before departing. Only used if Post Insertion Mode is DEPART.")]
	protected float m_fDepartDelay;

	[Attribute("120.0", UIWidgets.EditBox, "Altitude in metres the helicopter climbs to before flying off-map. DEPART mode only.")]
	protected float m_fDepartAltitude;

	// --------------------------------------------------------
	// DISEMBARK
	// --------------------------------------------------------

	[Attribute("2.0", UIWidgets.EditBox, "Metres AGL below which touchdown is considered confirmed. FULL_LANDING only.")]
	protected float m_fTouchdownAltitudeThreshold;

	[Attribute("1.5", UIWidgets.EditBox, "m/s below which the helicopter is considered stationary enough to disembark. FULL_LANDING only.")]
	protected float m_fTouchdownVelocityThreshold;

	[Attribute("15.0", UIWidgets.EditBox, "Seconds before stubborn seated AI are forcibly ejected via EjectRandomOccupants.")]
	protected float m_fDisembarkTimeout;

	// --------------------------------------------------------
	// DEBUG
	// --------------------------------------------------------

	[Attribute("0", UIWidgets.CheckBox, "Print state machine transitions and key events to log.")]
	protected bool m_bDebugLog;

	// --------------------------------------------------------
	// RUNTIME STATE
	// --------------------------------------------------------

	protected EEF_EHelicopterInsertionState m_eState = EEF_EHelicopterInsertionState.IDLE;
	protected float m_fStateEnteredTime;	//! Worldtime (seconds) at which current state was entered.
	protected SCR_AIGroup m_CargoGroup;		//! The group we spawned and will hand off.
	protected bool m_bTickRunning;			//! Whether our periodic Tick() is currently scheduled.
	protected bool m_bBoardingInitiated;	//! True once we've issued MoveInVehicle for this insertion's cargo.
	protected bool m_bHandoffDone;			//! True once we've handed the group off to the receiver. Disembark continues afterwards.

	// Flight controller state.
	protected bool m_bFlightTickRunning;		//! Whether the flight controller is active (EOnFrame drives it).
	protected bool m_bEngineStarted;			//! True once StartHelicopterEngine() has been called.
	protected float m_fDebugLogTimer;			//! Accumulates timeSlice; debug log fires every 0.5 s.
	protected vector m_vLZPosition;				//! Cached LZ entity origin, resolved when entering flight.
	protected vector m_vDepartHeading;			//! Direction the helicopter departs in (opposite of approach).
	protected float m_fRotorForceMultiplier;	//! Dynamic lift multiplier — drives both SetVelocity vertical and RotorSetForceScaleState.
	protected ref array<vector> m_aWaypoints;	//! Ordered 3D waypoints the helicopter flies through, ending at LZ.
	protected int m_iCurrentWaypointIndex;		//! Index into m_aWaypoints for the current flight target.
	protected VehicleHelicopterSimulation m_HelicopterSim; //! Cached rotor/engine simulation component.

	//! AGL (m) below which angular velocity is suppressed. Bank/pitch terms produce 100–900 deg/s
	//! near the ground and will swing the rotor into terrain.
	protected const float FLIGHT_ANGULAR_START_AGL = 8.0;

	//! Fixed periodic tick interval for state polling (boarding, disembark). EOnFrame handles flight.
	protected const float TICK_INTERVAL_SECONDS = 0.5;

	//! Vertical tolerance (m) used for hover-altitude arrival check.
	protected const float FLIGHT_VERTICAL_ARRIVAL_TOLERANCE = 1.0;

	//! Horizontal distance (m) past which a departing helicopter is deleted.
	protected const float FLIGHT_DEPART_DISTANCE_THRESHOLD = 1000.0;

	//! Distance band (m) inside which the helicopter begins slowing toward a waypoint.
	protected const float FLIGHT_SLOWDOWN_BAND = 40.0;

	//! Turn-angle multiplier to produce the bank (roll) angle. Matches DarcChopper helicopter type.
	protected const float FLIGHT_ROLL_ANGLE_MUL = 2.4;

	//! AGL (m) below which landing flare activates: horizontal zeroed, descent capped.
	protected const float FLIGHT_FLARE_AGL = 15.0;

	//! Max descent speed (m/s) during landing flare.
	protected const float FLIGHT_FLARE_DESCENT_SPEED = 3.0;

	//! Nose-up pitch at zero speed (degrees). Positive = nose up.
	protected const float FLIGHT_PITCH_SPEED_LOW = 5.0;

	//! Nose-down pitch at cruise speed (degrees). Negative = nose down.
	protected const float FLIGHT_PITCH_SPEED_HIGH = -10.0;

	//! Base rotor force scales — set once at takeoff, match DarcChopper attribute defaults.
	//! HandleRotorForce scales these dynamically via m_fRotorForceMultiplier.
	protected const float FLIGHT_BASE_ROTOR_SCALE_0 = 2.0;
	protected const float FLIGHT_BASE_ROTOR_SCALE_1 = 1.2;

	//! DarcChopper rotorForceMulUp for helicopter type (1.3 * 10 = 13). Scales vertical velocity.
	protected const float FLIGHT_ROTOR_FORCE_MUL_UP = 13.0;

	//! Forward raycast length (m) for terrain/obstacle detection.
	protected const float RAY_LENGTH_FRONT = 50.0;

	//! Vertical offset above helicopter origin for forward raycast start.
	protected const float RAY_DOWN_OFFSET = 5.0;

	// --------------------------------------------------------
	// INITIALISATION
	// --------------------------------------------------------

	override void OnPostInit(IEntity owner)
	{
		super.OnPostInit(owner);

		if (!Replication.IsServer())
			return;

		// Validate critical config up front. Don't abort init - mission maker may want to inspect
		// the helicopter even if config is incomplete - just log loudly so they catch it.
		if (m_sGroupPrefab.IsEmpty())
			Print("[EEF Helicopter] WARNING: No group prefab configured. StartInsertion will fail.", LogLevel.WARNING);

		if (m_sLZEntityName.IsEmpty())
			Print("[EEF Helicopter] WARNING: No LZ entity name configured. StartInsertion will fail.", LogLevel.WARNING);

		// Immediately zero velocity (DarcChopper OnPostInit pattern).
		Physics phys = owner.GetPhysics();
		if (phys)
			phys.SetVelocity(vector.Zero);

		// Pre-zero rotor forces before EngineStart so the default lift scales are never
		// non-zero on a grounded helicopter. StartHelicopterEngine() re-zeros them after
		// EngineStart() in case that call resets them internally.
		VehicleHelicopterSimulation heliSimInit = VehicleHelicopterSimulation.Cast(
			owner.GetRootParent().FindComponent(VehicleHelicopterSimulation)
		);
		if (heliSimInit)
		{
			heliSimInit.SetThrottle(0);
			heliSimInit.RotorSetForceScaleState(0, 0);
			heliSimInit.RotorSetForceScaleState(1, 0);
		}

		// Start engine immediately so rotors are spinning and audio is live from mission load.
		// Force scales are zeroed inside StartHelicopterEngine() so Reforger's lift simulation
		// does not apply physics — our SetVelocity/SetAngularVelocity calls own all motion.
		StartHelicopterEngine();

		// Prevent ground sliding during boarding.
		HelicopterControllerComponent hcc = HelicopterControllerComponent.Cast(
			owner.FindComponent(HelicopterControllerComponent)
		);
		if (hcc)
			hcc.SetPersistentWheelBrake(true);

		// Enable per-frame velocity zeroing (EOnFrame) until the flight tick takes over.
		SetEventMask(owner, EntityEvent.FRAME);

		if (!m_sPilotPrefab.IsEmpty())
			GetGame().GetCallqueue().CallLater(SpawnCrew, 1000, false);

		DebugLog("Initialised. Waiting for StartInsertion().");
	}

	// DarcChopper pattern: EOnFrame owns all velocity while the component is alive.
	// When flying, calls the flight controller every frame (smooth motion, correct time constants).
	// When not flying, zeros both velocity components so nothing can move the grounded helicopter.
	override void EOnFrame(IEntity owner, float timeSlice)
	{
		if (!Replication.IsServer())
			return;

		if (m_bFlightTickRunning)
		{
			TickFlightController(timeSlice);
			return;
		}

		Physics phys = owner.GetPhysics();
		if (phys)
		{
			phys.SetVelocity(vector.Zero);
			phys.SetAngularVelocity(vector.Zero);
		}
	}

	// --------------------------------------------------------
	// PUBLIC API
	// Called by EEF_ScenarioFrameworkActionStartHelicopterInsertion.
	// --------------------------------------------------------

	void StartInsertion()
	{
		if (!Replication.IsServer())
			return;

		if (m_eState != EEF_EHelicopterInsertionState.IDLE)
		{
			DebugLog(string.Format("StartInsertion ignored - already in state %1.", StateName(m_eState)));
			return;
		}

		// Resolve config. If anything is missing we fail fast and stay in IDLE.
		if (m_sGroupPrefab.IsEmpty())
		{
			Print("[EEF Helicopter] ERROR: Cannot start insertion - no group prefab configured.", LogLevel.ERROR);
			return;
		}

		if (m_sLZEntityName.IsEmpty())
		{
			Print("[EEF Helicopter] ERROR: Cannot start insertion - no LZ entity name configured.", LogLevel.ERROR);
			return;
		}

		// Verify the helicopter has at least one CARGO slot.
		// SCR_BaseCompartmentManagerComponent is required - the engine base class only exposes
		// GetCompartments, while the typed/free/occupant helpers live on the script subclass.
		SCR_BaseCompartmentManagerComponent compMgr = FindCompartmentManager();
		if (!compMgr)
		{
			Print("[EEF Helicopter] ERROR: Helicopter entity has no SCR_BaseCompartmentManagerComponent.", LogLevel.ERROR);
			return;
		}

		array<BaseCompartmentSlot> cargoSlots = {};
		compMgr.GetCompartmentsOfType(cargoSlots, ECompartmentType.CARGO);
		if (cargoSlots.IsEmpty())
		{
			Print("[EEF Helicopter] ERROR: Helicopter has no CARGO compartments. Pick a transport variant.", LogLevel.ERROR);
			return;
		}

		DebugLog(string.Format("StartInsertion - helicopter has %1 CARGO slot(s).", cargoSlots.Count()));

		TransitionTo(EEF_EHelicopterInsertionState.BOARDING);

		// Begin ticking.
		if (!m_bTickRunning)
		{
			GetGame().GetCallqueue().CallLater(Tick, TICK_INTERVAL_SECONDS * 1000, true);
			m_bTickRunning = true;
		}
	}

	// --------------------------------------------------------
	// STATE MACHINE PUMP
	// --------------------------------------------------------

	protected void Tick()
	{
		switch (m_eState)
		{
			case EEF_EHelicopterInsertionState.IDLE:
				// Should not be ticking in IDLE. Defensive stop.
				StopTick();
				return;

			case EEF_EHelicopterInsertionState.BOARDING:
				TickBoarding();
				return;

			case EEF_EHelicopterInsertionState.FLYING:
				// Flight is driven by EOnFrame — nothing for the slow tick to do here.
				return;

			case EEF_EHelicopterInsertionState.INSERTING:
				TickInserting();
				return;

			case EEF_EHelicopterInsertionState.POST_INSERTION:
				TickPostInsertion();
				return;
		}
	}

	// --------------------------------------------------------
	// STATE: BOARDING
	// --------------------------------------------------------

	protected void TickBoarding()
	{
		// First entry into BOARDING: spawn the group and either board now (synchronous spawn,
		// rare) or hook the readiness event and wait (delayed spawn, the common case).
		if (!m_CargoGroup)
		{
			m_CargoGroup = SpawnCargoGroup();
			if (!m_CargoGroup)
			{
				Print("[EEF Helicopter] ERROR: Failed to spawn cargo group. Aborting insertion.", LogLevel.ERROR);
				AbortToIdle();
				return;
			}

			// SCR_AIGroup spawns its members across multiple frames via EOnFrame. Until that
			// finishes, GetAgents() will return an empty array and MoveInVehicle has nothing
			// to seat. The group's OnAllDelayedEntitySpawned event fires exactly once when
			// delayed spawning completes - we hook it and wait.
			if (m_CargoGroup.IsInitializing())
			{
				m_CargoGroup.GetOnAllDelayedEntitySpawned().Insert(OnGroupReadyToBoard);
				DebugLog("Cargo group spawning members - hooked OnAllDelayedEntitySpawned, waiting.");
				return;
			}

			// Not initializing. Either the group spawned synchronously (rare) or the prefab
			// has no members configured (m_aUnitPrefabSlots empty / m_bSpawnImmediately false).
			// The latter is misconfig - no agents will ever appear and no event will fire.
			array<AIAgent> agents = {};
			m_CargoGroup.GetAgents(agents);
			if (agents.IsEmpty())
			{
				Print("[EEF Helicopter] ERROR: Cargo group prefab has no members configured (or SpawnImmediately is false). Aborting.", LogLevel.ERROR);
				AbortToIdle();
				return;
			}

			// Synchronous spawn path - board immediately.
			BoardGroupMembers(m_CargoGroup);
			m_bBoardingInitiated = true;
			return;
		}

		// Subsequent ticks. Two phases:
		//  - Waiting for OnGroupReadyToBoard to fire (m_bBoardingInitiated == false)
		//  - Polling for occupants to appear after MoveInVehicle (m_bBoardingInitiated == true)
		// We use a single timeout (m_fDisembarkTimeout) for both phases combined. If members
		// take long enough to spawn that seat-confirmation can't fit, raise the timeout.
		float elapsed = GetCurrentTime() - m_fStateEnteredTime;

		if (!m_bBoardingInitiated)
		{
			if (elapsed > m_fDisembarkTimeout)
			{
				Print(string.Format("[EEF Helicopter] ERROR: Cargo group did not finish initializing within %1s. Aborting.", elapsed), LogLevel.ERROR);
				AbortToIdle();
			}
			return;
		}

		int occupants = GetCargoOccupantCount();

		if (occupants > 0)
		{
			DebugLog(string.Format("Boarding complete - %1 occupant(s) seated after %2s.", occupants, elapsed));

			// Build the waypoint path and enter FLYING.
			if (!ResolveFlightPath())
			{
				Print("[EEF Helicopter] ERROR: Could not build flight path. Aborting insertion.", LogLevel.ERROR);
				AbortToIdle();
				return;
			}
			EnterFlying();
			TransitionTo(EEF_EHelicopterInsertionState.FLYING);
			StartFlightTick();
			return;
		}

		// Seat-confirmation timeout. MoveInVehicle is RPC-based so a tick or two of latency
		// is normal, but if no one has seated by m_fDisembarkTimeout something else is wrong.
		if (elapsed > m_fDisembarkTimeout)
		{
			Print(string.Format("[EEF Helicopter] ERROR: Boarding seat-confirmation timeout after %1s with no occupants. Aborting.", elapsed), LogLevel.ERROR);
			AbortToIdle();
			return;
		}
	}

	//! SCR_AIGroup.GetOnAllDelayedEntitySpawned() callback - fires once when the cargo
	//! group has finished spawning all its members.
	protected void OnGroupReadyToBoard(SCR_AIGroup group)
	{
		// Stale-callback guard. AbortToIdle nulls m_CargoGroup, so a late event from a
		// discarded group lands here with a mismatched reference and is ignored.
		if (group != m_CargoGroup)
		{
			DebugLog("OnGroupReadyToBoard fired for stale group, ignoring.");
			return;
		}

		if (m_eState != EEF_EHelicopterInsertionState.BOARDING)
		{
			DebugLog(string.Format("OnGroupReadyToBoard fired in unexpected state %1, ignoring.", StateName(m_eState)));
			return;
		}

		DebugLog("Cargo group ready - issuing MoveInVehicle.");
		BoardGroupMembers(m_CargoGroup);
		m_bBoardingInitiated = true;
	}

	// --------------------------------------------------------
	// STATE: INSERTING
	// --------------------------------------------------------

	protected void TickInserting()
	{
		// First tick in INSERTING: hand off the group reference to the receiver before
		// anyone has left cargo. The receiver (e.g. EEF_PatrolComponent) starts queueing
		// waypoints immediately, so each trooper has a destination ready as soon as they
		// drop. Reduces stacking and stationary loitering near the rotors.
		if (!m_bHandoffDone)
		{
			HandoffToReceiver();
			m_bHandoffDone = true;
		}

		int occupants = GetCargoOccupantCount();
		float elapsed = GetCurrentTime() - m_fStateEnteredTime;

		if (occupants == 0)
		{
			DebugLog(string.Format("Disembark complete after %1s.", elapsed));
			TransitionTo(EEF_EHelicopterInsertionState.POST_INSERTION);
			return;
		}

		// Eject one occupant per tick. With TICK_INTERVAL_SECONDS = 0.5 that's roughly
		// one trooper every half-second, giving each enough time to clear before the
		// next drops. Better than bulk teleport for visual cleanliness; not as good as
		// animated dismount (a future polish pass).
		if (EjectOneFromCargo())
			DebugLog(string.Format("Ejected one trooper from cargo. %1 remaining.", occupants - 1));

		// Defensive timeout. With ~14 cargo slots at 0.5s each that's ~7s of clean drops,
		// so the default 15s gives generous headroom. If still seated past timeout, fall
		// back to bulk eject and proceed.
		if (elapsed > m_fDisembarkTimeout)
		{
			DebugLog(string.Format("Disembark timeout reached with %1 still seated. Force-ejecting remainder.", occupants));
			ForceEjectAllCargo();
			TransitionTo(EEF_EHelicopterInsertionState.POST_INSERTION);
			return;
		}
	}

	// --------------------------------------------------------
	// STATE: POST_INSERTION
	// --------------------------------------------------------

	protected void TickPostInsertion()
	{
		switch (m_ePostInsertionMode)
		{
			case EEF_EPostInsertionMode.STAY:
			{
				// Wind down rotors before freezing physics so the helicopter is silent and
				// still on the ground rather than frozen mid-rotor.
				StopHelicopterEngine();
				StopFlightTick(); // Idempotent — catches HOVER_LANDING where tick stayed running.

				Physics phys = GetOwner().GetPhysics();
				if (phys)
					phys.ChangeSimulationState(SimulationState.NONE);

				DebugLog("POST_INSERTION (STAY) - engine off, physics suspended, terminal. Stopping tick.");
				StopTick();
				return;
			}

			case EEF_EPostInsertionMode.DEPART:
			{
				float elapsed = GetCurrentTime() - m_fStateEnteredTime;

				// Wait the configured delay before departing - gives troops time to clear
				// the rotor wash visually.
				if (elapsed < m_fDepartDelay)
					return;

				// First tick after the delay: switch the flight controller into depart mode.
				// We detect "first tick" by checking whether the flight tick is still running -
				// if it isn't, we haven't started departing yet.
				if (!m_bFlightTickRunning)
				{
					EnterDepart();
					StartFlightTick();
					DebugLog("POST_INSERTION (DEPART) - departing.");
					return;
				}

				// Subsequent ticks: poll distance from LZ. Once the helicopter is far enough
				// away, delete it.
				vector heliPos = GetOwner().GetOrigin();
				vector horizDelta = Vector(heliPos[0] - m_vLZPosition[0], 0, heliPos[2] - m_vLZPosition[2]);
				if (horizDelta.LengthSq() >= FLIGHT_DEPART_DISTANCE_THRESHOLD * FLIGHT_DEPART_DISTANCE_THRESHOLD)
				{
					DebugLog(string.Format("Helicopter departed beyond %1m. Deleting.", FLIGHT_DEPART_DISTANCE_THRESHOLD));
					StopFlightTick();
					StopTick();
					SCR_EntityHelper.DeleteEntityAndChildren(GetOwner());
					return;
				}
				return;
			}
		}
	}

	// --------------------------------------------------------
	// HELPERS — BOARDING
	// --------------------------------------------------------

	//! Spawn the cargo group at the helicopter's origin. Returns null on failure.
	protected SCR_AIGroup SpawnCargoGroup()
	{
		Resource groupResource = Resource.Load(m_sGroupPrefab);
		if (!groupResource || !groupResource.IsValid())
		{
			Print("[EEF Helicopter] ERROR: Could not load group prefab: " + m_sGroupPrefab, LogLevel.ERROR);
			return null;
		}

		EntitySpawnParams spawnParams = new EntitySpawnParams();
		spawnParams.TransformMode = ETransformMode.WORLD;
		Math3D.MatrixIdentity4(spawnParams.Transform);
		spawnParams.Transform[3] = GetOwner().GetOrigin();

		IEntity spawned = GetGame().SpawnEntityPrefab(groupResource, GetGame().GetWorld(), spawnParams);
		SCR_AIGroup group = SCR_AIGroup.Cast(spawned);
		if (!group)
		{
			Print("[EEF Helicopter] ERROR: Spawned cargo prefab is not an SCR_AIGroup.", LogLevel.ERROR);
			return null;
		}

		DebugLog("Cargo group spawned at helicopter origin.");
		return group;
	}

	//! Seat each member of the group in CARGO. Excess units beyond cargo capacity are
	//! left at the spawn point with a warning.
	protected void BoardGroupMembers(SCR_AIGroup group)
	{
		SCR_BaseCompartmentManagerComponent compMgr = FindCompartmentManager();
		if (!compMgr)
			return;

		array<BaseCompartmentSlot> freeSlots = {};
		compMgr.GetFreeCompartmentsOfType(freeSlots, ECompartmentType.CARGO);

		array<AIAgent> agents = {};
		group.GetAgents(agents);

		int boarded = 0;
		int orphaned = 0;

		foreach (AIAgent agent : agents)
		{
			if (!agent)
				continue;

			IEntity character = agent.GetControlledEntity();
			if (!character)
				continue;

			if (boarded >= freeSlots.Count())
			{
				orphaned++;
				continue;
			}

			SCR_CompartmentAccessComponent access = SCR_CompartmentAccessComponent.Cast(
				character.FindComponent(SCR_CompartmentAccessComponent)
			);

			if (!access)
			{
				DebugLog("Group member missing SCR_CompartmentAccessComponent, skipping.");
				continue;
			}

			// MoveInVehicle is the seat-teleport path - instant placement, no walk-and-enter.
			access.MoveInVehicle(GetOwner(), ECompartmentType.CARGO);
			boarded++;
		}

		DebugLog(string.Format("Boarding requested - %1 seated, %2 orphaned (no cargo slot).", boarded, orphaned));

		if (orphaned > 0)
		{
			Print(string.Format("[EEF Helicopter] WARNING: %1 group member(s) had no available CARGO slot and were left at spawn point. Reduce group size or pick a larger transport.", orphaned), LogLevel.WARNING);
		}
	}

	// --------------------------------------------------------
	// HELPERS — DISEMBARK
	// --------------------------------------------------------

	//! Eject the first occupant found in any CARGO compartment.
	//! Returns true if one was ejected, false if cargo was already empty.
	//! Called once per tick during INSERTING for staggered disembark.
	protected bool EjectOneFromCargo()
	{
		SCR_BaseCompartmentManagerComponent compMgr = FindCompartmentManager();
		if (!compMgr)
			return false;

		array<BaseCompartmentSlot> cargoSlots = {};
		compMgr.GetCompartmentsOfType(cargoSlots, ECompartmentType.CARGO);

		foreach (BaseCompartmentSlot slot : cargoSlots)
		{
			if (!slot)
				continue;
			if (!slot.GetOccupant())
				continue;

			// EjectOccupant(force, ejectUnconscious, out ejectedImmediately, ejectOnTheSpot)
			// - force = true: bypass the per-compartment ejection chance and the door-open
			//   gate (otherwise EjectOccupant would only succeed with doors physically open)
			// - ejectOnTheSpot = false: engine picks the nearest door to the character
			//   (PickDoorIndexForPoint) and teleports them to that door position via
			//   EGetOutType.TELEPORT. Falls back to on-the-spot internally if the door is
			//   configured as fake on the helicopter prefab.
			bool ejectedImmediately;
			return slot.EjectOccupant(true, false, ejectedImmediately, false);
		}

		return false;
	}

	//! Force-eject everyone in CARGO on the spot in a single call.
	//! Used as the timeout fallback when staggered disembark hasn't completed in time.
	//! Not the primary disembark path - see EjectOneFromCargo for that.
	protected void ForceEjectAllCargo()
	{
		SCR_BaseCompartmentManagerComponent compMgr = FindCompartmentManager();
		if (!compMgr)
			return;

		// EjectRandomOccupants(ejectionChance, ejectUnconscious, out allEjectedImmediately, ejectOnTheSpot)
		// ejectionChance = 1.0 -> force eject everyone
		// ejectOnTheSpot = true -> instant placement at compartment position, no animation
		bool allEjected;
		compMgr.EjectRandomOccupants(1.0, false, allEjected, true);

		DebugLog(string.Format("Force-eject (timeout fallback) issued. allEjectedImmediately = %1.", allEjected));
	}

	//! Count current CARGO occupants. Used for both boarding-complete and disembark-complete polling.
	protected int GetCargoOccupantCount()
	{
		SCR_BaseCompartmentManagerComponent compMgr = FindCompartmentManager();
		if (!compMgr)
			return 0;

		array<IEntity> occupants = {};
		compMgr.GetOccupantsOfType(occupants, ECompartmentType.CARGO);
		return occupants.Count();
	}

	// --------------------------------------------------------
	// FLIGHT CONTROLLER
	//
	// Two layers:
	//   - ResolveFlightPath / EnterFlying build the waypoint list and start navigation.
	//   - TickFlightController runs every EOnFrame frame, advances through waypoints,
	//     drives velocity/angular velocity, and calls OnFlightArrived at the final waypoint.
	// --------------------------------------------------------

	// --------------------------------------------------------
	// FLIGHT CONTROLLER — MATH HELPERS
	// Inlined equivalents of DarcChopper's SDRC_Math helpers so EEF stays self-contained.
	// --------------------------------------------------------

	//! Angular velocity (rad/s, world-space rotation vector) to rotate direction `fromDir`
	//! to align with `toDir` in `time` seconds. Returns zero for degenerate inputs.
	protected static vector EEF_ComputeAngularVelocity(vector fromDir, vector toDir, float time)
	{
		fromDir.Normalize();
		toDir.Normalize();

		// Cross product gives rotation axis scaled by sin(angle).
		float cx = fromDir[1] * toDir[2] - fromDir[2] * toDir[1];
		float cy = fromDir[2] * toDir[0] - fromDir[0] * toDir[2];
		float cz = fromDir[0] * toDir[1] - fromDir[1] * toDir[0];
		float sinA = Math.Sqrt(cx * cx + cy * cy + cz * cz);
		float cosA = fromDir[0] * toDir[0] + fromDir[1] * toDir[1] + fromDir[2] * toDir[2];
		float angle = Math.Atan2(sinA, cosA);

		if (sinA < 0.0001 || time <= 0)
			return vector.Zero;

		float scale = angle / (sinA * time);
		return Vector(cx * scale, cy * scale, cz * scale);
	}

	//! Rodrigues rotation: rotate vector `v` around unit `axis` by `angleRad` radians.
	protected static vector EEF_RotateAroundAxis(vector v, vector axis, float angleRad)
	{
		axis.Normalize();
		float c   = Math.Cos(angleRad);
		float s   = Math.Sin(angleRad);
		float dot = axis[0] * v[0] + axis[1] * v[1] + axis[2] * v[2];
		return Vector(
			v[0] * c + (axis[1] * v[2] - axis[2] * v[1]) * s + axis[0] * dot * (1.0 - c),
			v[1] * c + (axis[2] * v[0] - axis[0] * v[2]) * s + axis[1] * dot * (1.0 - c),
			v[2] * c + (axis[0] * v[1] - axis[1] * v[0]) * s + axis[2] * dot * (1.0 - c)
		);
	}

	// --------------------------------------------------------
	// FLIGHT CONTROLLER — ENGINE HELPERS
	// --------------------------------------------------------

	//! Start the helicopter's engine simulation so rotor spin, engine audio,
	//! and rotor-wash particle effects are active throughout flight.
	//! Gravity is left enabled — we counteract it via vertical velocity each tick.
	protected void StartHelicopterEngine()
	{
		// VehicleHelicopterSimulation lives on the vehicle root entity.
		m_HelicopterSim = VehicleHelicopterSimulation.Cast(
			GetOwner().GetRootParent().FindComponent(VehicleHelicopterSimulation)
		);
		if (!m_HelicopterSim)
		{
			Print("[EEF Helicopter] WARNING: VehicleHelicopterSimulation not found — engine effects will be silent.", LogLevel.WARNING);
			return;
		}

		m_HelicopterSim.EngineStart();
		m_HelicopterSim.SetThrottle(0.5);
		// EngineStart() resets rotor scales to prefab defaults — zero them immediately
		// so the grounded helicopter does not receive native lift forces.
		// HandleRotorForce will raise them once the helicopter clears the ground.
		m_HelicopterSim.RotorSetForceScaleState(0, 0);
		m_HelicopterSim.RotorSetForceScaleState(1, 0);
		m_fRotorForceMultiplier = 0;
		m_bEngineStarted = true;
		DebugLog("Engine started.");
	}

	//! Wind rotors down to zero and cut throttle. Called before physics suspension (STAY)
	//! so rotors are silent while the helicopter sits on the ground indefinitely.
	protected void StopHelicopterEngine()
	{
		if (!m_HelicopterSim)
			return;
		m_HelicopterSim.RotorSetForceScaleState(0, 0);
		m_HelicopterSim.RotorSetForceScaleState(1, 0);
		m_HelicopterSim.SetThrottle(0);
		m_bEngineStarted = false;
		DebugLog("Engine stopped.");
	}

	//! Spawn a pilot from m_sPilotPrefab and seat them in the first PILOT compartment.
	//! Called via CallLater 1s after OnPostInit so the vehicle entity is fully initialised.
	protected void SpawnCrew()
	{
		if (m_sPilotPrefab.IsEmpty())
			return;

		Resource pilotResource = Resource.Load(m_sPilotPrefab);
		if (!pilotResource || !pilotResource.IsValid())
		{
			Print("[EEF Helicopter] WARNING: Could not load pilot prefab: " + m_sPilotPrefab, LogLevel.WARNING);
			return;
		}

		EntitySpawnParams spawnParams = new EntitySpawnParams();
		spawnParams.TransformMode = ETransformMode.WORLD;
		Math3D.MatrixIdentity4(spawnParams.Transform);
		spawnParams.Transform[3] = GetOwner().GetOrigin();

		IEntity pilot = GetGame().SpawnEntityPrefab(pilotResource, GetGame().GetWorld(), spawnParams);
		if (!pilot)
		{
			Print("[EEF Helicopter] WARNING: Failed to spawn pilot entity.", LogLevel.WARNING);
			return;
		}

		SCR_CompartmentAccessComponent access = SCR_CompartmentAccessComponent.Cast(
			pilot.FindComponent(SCR_CompartmentAccessComponent)
		);
		if (!access)
		{
			Print("[EEF Helicopter] WARNING: Pilot entity missing SCR_CompartmentAccessComponent.", LogLevel.WARNING);
			return;
		}

		access.MoveInVehicle(GetOwner(), ECompartmentType.PILOT);
		DebugLog("Pilot spawned and seated.");
	}

	// --------------------------------------------------------
	// FLIGHT CONTROLLER — WAYPOINT PATH
	// --------------------------------------------------------

	//! Build the full waypoint list for the insertion flight. Called once at the end of BOARDING.
	//! Waypoints (each a world-space 3D point at cruise altitude):
	//!   [0]   climb point — directly above spawn, at cruise AGL
	//!   [1..n] optional named intermediate waypoints
	//!   [last] LZ at landing/hover altitude
	//! Returns false if the LZ cannot be resolved.
	protected bool ResolveFlightPath()
	{
		m_aWaypoints = new array<vector>();

		// Resolve LZ first so we can cache the depart heading before it's needed.
		IEntity lzEntity = GetGame().GetWorld().FindEntityByName(m_sLZEntityName);
		if (!lzEntity)
		{
			Print(string.Format("[EEF Helicopter] ERROR: LZ entity '%1' not found in world.", m_sLZEntityName), LogLevel.ERROR);
			return false;
		}
		m_vLZPosition = lzEntity.GetOrigin();

		// [0] Climb point — hover directly above spawn until at cruise altitude.
		vector here = GetOwner().GetOrigin();
		float groundY = GetGame().GetWorld().GetSurfaceY(here[0], here[2]);
		m_aWaypoints.Insert(Vector(here[0], groundY + m_fCruiseAltitudeAGL, here[2]));

		// [1..n] Optional named intermediate waypoints at cruise altitude.
		AddNamedWaypoint(m_sWaypoint1Name);
		AddNamedWaypoint(m_sWaypoint2Name);
		AddNamedWaypoint(m_sWaypoint3Name);

		// [last] LZ — at hover or ground altitude depending on landing mode.
		float lzGroundY = GetGame().GetWorld().GetSurfaceY(m_vLZPosition[0], m_vLZPosition[2]);
		float lzTargetY;
		if (m_eLandingMode == EEF_EHelicopterLandingMode.HOVER_LANDING)
			lzTargetY = lzGroundY + m_fHoverAltitude;
		else
			lzTargetY = lzGroundY;
		m_aWaypoints.Insert(Vector(m_vLZPosition[0], lzTargetY, m_vLZPosition[2]));

		// Cache depart heading as opposite of approach direction (from helicopter to LZ).
		vector toLZ = Vector(m_vLZPosition[0] - here[0], 0, m_vLZPosition[2] - here[2]);
		if (toLZ.LengthSq() > 0.01)
			m_vDepartHeading = (toLZ * -1).Normalized();

		DebugLog(string.Format("Flight path built: %1 waypoints. LZ at %2.", m_aWaypoints.Count(), m_vLZPosition.ToString()));
		return true;
	}

	//! Resolve a named world entity and append a waypoint at cruise altitude above it.
	//! Silently skips empty names or missing entities (optional waypoints are valid when absent).
	protected void AddNamedWaypoint(string entityName)
	{
		if (entityName.IsEmpty())
			return;
		IEntity ent = GetGame().GetWorld().FindEntityByName(entityName);
		if (!ent)
		{
			Print(string.Format("[EEF Helicopter] WARNING: Waypoint entity '%1' not found — skipping.", entityName), LogLevel.WARNING);
			return;
		}
		vector pos = ent.GetOrigin();
		float groundY = GetGame().GetWorld().GetSurfaceY(pos[0], pos[2]);
		m_aWaypoints.Insert(Vector(pos[0], groundY + m_fCruiseAltitudeAGL, pos[2]));
		DebugLog(string.Format("Intermediate waypoint '%1' added at %2.", entityName, m_aWaypoints[m_aWaypoints.Count() - 1].ToString()));
	}

	//! Add a waypoint at an arbitrary world position (for dynamic/scripted queuing).
	void AddWaypoint(vector pos)
	{
		if (!m_aWaypoints)
			m_aWaypoints = new array<vector>();
		m_aWaypoints.Insert(pos);
	}

	//! Activate the FLYING state — index starts at 0 (climb point).
	protected void EnterFlying()
	{
		m_iCurrentWaypointIndex = 0;
		DebugLog(string.Format("EnterFlying — %1 waypoints queued.", m_aWaypoints.Count()));
	}

	//! Append a single depart waypoint far in the depart heading direction and restart the flight tick.
	protected void EnterDepart()
	{
		if (m_vDepartHeading.LengthSq() < 0.01)
			m_vDepartHeading = Vector(0, 0, 1);

		if (!m_aWaypoints)
			m_aWaypoints = new array<vector>();
		m_aWaypoints.Clear();

		vector here = GetOwner().GetOrigin();
		float groundY = GetGame().GetWorld().GetSurfaceY(here[0], here[2]);
		float departDist = FLIGHT_DEPART_DISTANCE_THRESHOLD * 1.5;
		m_aWaypoints.Insert(Vector(
			here[0] + m_vDepartHeading[0] * departDist,
			groundY + m_fDepartAltitude,
			here[2] + m_vDepartHeading[2] * departDist
		));
		m_iCurrentWaypointIndex = 0;

		DebugLog(string.Format("EnterDepart — heading %1 altitude %2.", m_vDepartHeading.ToString(), m_fDepartAltitude));
	}

	//! Per-frame flight controller — runs every EOnFrame tick while m_bFlightTickRunning is true.
	//! Implements DarcChopper's cooperative model:
	//!   - Heading-driven horizontal velocity + small correction toward target.
	//!   - Vertical velocity driven by m_fRotorForceMultiplier (set by HandleRotorForce).
	//!   - Angular velocity: yaw + bank + rollback + pitch, all sharing turnTime.
	//!   - Waypoint progression: intermediate waypoints advance on arrival; final triggers OnFlightArrived.
	protected void TickFlightController(float timeSlice)
	{
		Physics phys = GetOwner().GetPhysics();
		if (!phys)
			return;

		if (!SCR_AIVehicleUsability.VehicleCanMove(GetOwner()))
		{
			Print("[EEF Helicopter] Vehicle no longer operational — stopping flight controller.", LogLevel.WARNING);
			StopFlightTick();
			return;
		}

		if (!m_aWaypoints || m_aWaypoints.IsEmpty())
		{
			StopFlightTick();
			return;
		}

		vector here = GetOwner().GetOrigin();
		float groundY = GetGame().GetWorld().GetSurfaceY(here[0], here[2]);
		float altAGL = here[1] - groundY;

		if (m_bDebugLog)
		{
			m_fDebugLogTimer += timeSlice;
			if (m_fDebugLogTimer >= 0.5)
			{
				m_fDebugLogTimer = 0;
				vector vel = phys.GetVelocity();
				Print(string.Format("[EEF Helicopter] state:%1 altAGL:%2 vel:(%3,%4,%5) rotorMul:%6 wp:%7/%8",
					StateName(m_eState), altAGL, vel[0], vel[1], vel[2],
					m_fRotorForceMultiplier, m_iCurrentWaypointIndex, m_aWaypoints.Count()));
			}
		}

		vector target = m_aWaypoints[m_iCurrentWaypointIndex];
		bool isFinalWaypoint = (m_iCurrentWaypointIndex >= m_aWaypoints.Count() - 1);

		vector mat[4];
		GetOwner().GetWorldTransform(mat);
		vector heliRight   = mat[0];
		vector heliUp      = mat[1];
		vector heliForward = mat[2];

		vector heliFwdFlat = Vector(heliForward[0], 0, heliForward[2]);
		if (heliFwdFlat.LengthSq() > 0.0001)
			heliFwdFlat.Normalize();
		else
			heliFwdFlat = Vector(0, 0, 1);

		// ─── Horizontal ────────────────────────────────────────────────────────
		vector horizDelta = Vector(target[0] - here[0], 0, target[2] - here[2]);
		float horizDist = horizDelta.Length();

		vector horizDir = vector.Zero;
		if (horizDist > 0.01)
			horizDir = horizDelta * (1.0 / horizDist);

		float speed;
		if (isFinalWaypoint)
			speed = m_fApproachSpeed;
		else
			speed = m_fCruiseSpeed;
		if (horizDist < FLIGHT_SLOWDOWN_BAND && speed > 0)
			speed = speed * (horizDist / FLIGHT_SLOWDOWN_BAND);

		float yawRad = GetOwner().GetAngles()[1] * Math.DEG2RAD;
		float velX = 0.0;
		float velZ = 0.0;
		if (speed > 0.05 && horizDist > 0.5)
		{
			velX = horizDir[0] + Math.Sin(yawRad) * speed;
			velZ = horizDir[2] + Math.Cos(yawRad) * speed;
		}

		// ─── Flare ─────────────────────────────────────────────────────────────
		bool isFlare = isFinalWaypoint
			&& m_eLandingMode == EEF_EHelicopterLandingMode.FULL_LANDING
			&& altAGL < FLIGHT_FLARE_AGL;
		if (isFlare)
		{
			velX = 0;
			velZ = 0;
		}

		// ─── Terrain avoidance (forward raycast via surface Y) ─────────────────
		float fwdX = here[0] + mat[2][0] * RAY_LENGTH_FRONT;
		float fwdZ = here[2] + mat[2][2] * RAY_LENGTH_FRONT;
		float fwdGroundY = GetGame().GetWorld().GetSurfaceY(fwdX, fwdZ);
		float fwdClearance = here[1] - fwdGroundY;

		// ─── Vertical / rotor ─────────────────────────────────────────────────
		HandleRotorForce(altAGL, target[1], fwdClearance);

		// DarcChopper vertical: normalizedDirY * BASE_ROTOR_SCALE_0 * ROTOR_FORCE_MUL_UP * multiplier
		float totalDist = (target - here).Length();
		float normalizedDirY = 0.0;
		if (totalDist > 0.1)
			normalizedDirY = (target[1] - here[1]) / totalDist;

		float velY = normalizedDirY * FLIGHT_BASE_ROTOR_SCALE_0 * FLIGHT_ROTOR_FORCE_MUL_UP * m_fRotorForceMultiplier;
		if (isFlare)
			velY = Math.Clamp(velY, -FLIGHT_FLARE_DESCENT_SPEED, 0);

		phys.SetVelocity(Vector(velX, velY, velZ));

		// ─── Waypoint advance ─────────────────────────────────────────────────
		float vertDist = Math.AbsFloat(here[1] - target[1]);
		if (!isFinalWaypoint && horizDist < m_fWaypointArrivalTolerance && vertDist < m_fWaypointArrivalTolerance)
		{
			m_iCurrentWaypointIndex++;
			DebugLog(string.Format("Waypoint %1 reached — advancing to %2.", m_iCurrentWaypointIndex - 1, m_iCurrentWaypointIndex));
			return;
		}

		if (isFinalWaypoint)
		{
			bool arrived = false;
			if (m_eLandingMode == EEF_EHelicopterLandingMode.HOVER_LANDING)
				arrived = horizDist < 4.0 && Math.AbsFloat(here[1] - target[1]) < FLIGHT_VERTICAL_ARRIVAL_TOLERANCE;
			else
				arrived = altAGL < m_fTouchdownAltitudeThreshold;

			if (arrived)
			{
				DebugLog(string.Format("Final waypoint arrived (altAGL:%1 horizDist:%2).", altAGL, horizDist));
				OnFlightArrived();
				return;
			}
		}

		// ─── Angular velocity ──────────────────────────────────────────────────
		float refSpeed;
		if (isFinalWaypoint)
			refSpeed = Math.Max(m_fApproachSpeed, 10.0);
		else
			refSpeed = Math.Max(m_fCruiseSpeed, 10.0);
		float turnTime = Math.Clamp(40.0 / refSpeed, 0.5, 2.5);

		vector angVelTotal = vector.Zero;

		if (horizDist > 1.0)
		{
			// 1. YAW
			angVelTotal = angVelTotal + EEF_ComputeAngularVelocity(heliFwdFlat, horizDir, turnTime);

			// 2. BANK — desiredUp=heliUp when straight → zero force; tilts proportionally when turning.
			float crossY  = heliFwdFlat[0] * horizDir[2] - heliFwdFlat[2] * horizDir[0];
			float dotXZ   = heliFwdFlat[0] * horizDir[0] + heliFwdFlat[2] * horizDir[2];
			float turnAngle = Math.Atan2(crossY, dotXZ);
			float rollAngle = Math.Clamp(turnAngle, -0.5, 0.5) * FLIGHT_ROLL_ANGLE_MUL;
			vector desiredUp = EEF_RotateAroundAxis(heliUp, heliForward, rollAngle);
			angVelTotal = angVelTotal + EEF_ComputeAngularVelocity(heliUp, desiredUp, turnTime);
		}

		// 3. ROLLBACK — drives heliUp toward world-up, counterbalancing bank.
		angVelTotal = angVelTotal + EEF_ComputeAngularVelocity(heliUp, vector.Up, turnTime * 0.6);

		// 4. PITCH — FLIGHT_PITCH_SPEED_LOW at rest, FLIGHT_PITCH_SPEED_HIGH at cruise.
		float speedFrac = 0.0;
		if (m_fCruiseSpeed > 0)
			speedFrac = Math.Clamp(speed / m_fCruiseSpeed, 0.0, 1.0);
		float pitchAngle = Math.Lerp(FLIGHT_PITCH_SPEED_LOW * Math.DEG2RAD, FLIGHT_PITCH_SPEED_HIGH * Math.DEG2RAD, speedFrac);
		vector pitchedFwd = EEF_RotateAroundAxis(heliForward, heliRight, pitchAngle);
		angVelTotal = angVelTotal + EEF_ComputeAngularVelocity(heliForward, pitchedFwd, turnTime * 0.5);

		if (altAGL > FLIGHT_ANGULAR_START_AGL && !isFlare)
			phys.SetAngularVelocity(angVelTotal);
		else
			phys.SetAngularVelocity(EEF_ComputeAngularVelocity(heliUp, vector.Up, 0.5));
	}

	//! Compute and apply m_fRotorForceMultiplier based on altitude error and forward clearance.
	//! Also updates RotorSetForceScaleState so rotor audio/visual matches the current lift level.
	protected void HandleRotorForce(float altAGL, float targetAltitude, float fwdClearance)
	{
		vector here = GetOwner().GetOrigin();
		float altError = targetAltitude - here[1];
		float heliAGL = Math.Max(altAGL, 1.0);

		// Primary: altitude error normalised by current AGL (DarcChopper bigMul pattern).
		float bigMul = Math.Clamp(altError / heliAGL, -1.0, 1.5);

		// Obstacle boost: if terrain ahead is within RAY_LENGTH_FRONT/2, climb harder.
		float rayLenMul = 1.0;
		if (fwdClearance < RAY_LENGTH_FRONT * 0.5 && fwdClearance > 0)
			rayLenMul = 2.0;

		m_fRotorForceMultiplier = bigMul * rayLenMul;

		if (!m_HelicopterSim)
			return;

		if (altAGL > FLIGHT_ANGULAR_START_AGL)
		{
			float scale0 = Math.Max(FLIGHT_BASE_ROTOR_SCALE_0 * m_fRotorForceMultiplier, 0);
			float scale1 = Math.Max(FLIGHT_BASE_ROTOR_SCALE_1 * m_fRotorForceMultiplier, 0);
			m_HelicopterSim.RotorSetForceScaleState(0, scale0);
			m_HelicopterSim.RotorSetForceScaleState(1, scale1);
		}
		else
		{
			m_HelicopterSim.RotorSetForceScaleState(0, 0);
			m_HelicopterSim.RotorSetForceScaleState(1, 0);
		}
	}

	//! Called when the final waypoint is reached. Stops tick for FULL_LANDING; keeps it for HOVER_LANDING.
	protected void OnFlightArrived()
	{
		if (m_eLandingMode == EEF_EHelicopterLandingMode.FULL_LANDING)
		{
			Physics phys = GetOwner().GetPhysics();
			if (phys)
			{
				phys.SetVelocity(vector.Zero);
				phys.SetAngularVelocity(vector.Zero);
			}
			if (m_HelicopterSim)
			{
				m_HelicopterSim.RotorSetForceScaleState(0, 0);
				m_HelicopterSim.RotorSetForceScaleState(1, 0);
			}
			m_fRotorForceMultiplier = 0;
			StopFlightTick();
		}
		// HOVER_LANDING: tick keeps running — controller holds hover altitude while troops exit.

		TransitionTo(EEF_EHelicopterInsertionState.INSERTING);
	}

	//! Activate the flight controller. EOnFrame will call TickFlightController every frame.
	protected void StartFlightTick()
	{
		if (m_bFlightTickRunning)
			return;
		m_fDebugLogTimer = 0;
		m_iCurrentWaypointIndex = 0;
		m_bFlightTickRunning = true;
	}

	//! Deactivate the flight controller.
	protected void StopFlightTick()
	{
		if (!m_bFlightTickRunning)
			return;
		m_bFlightTickRunning = false;
	}

	// --------------------------------------------------------
	// HELPERS — HANDOFF
	// --------------------------------------------------------

	//! Look up the LZ entity by name and dispatch OnGroupReceived to its
	//! EEF_GroupReceiverComponent (if present).
	protected void HandoffToReceiver()
	{
		if (!m_CargoGroup)
		{
			DebugLog("Handoff skipped - cargo group is null.");
			return;
		}

		IEntity lzEntity = GetGame().GetWorld().FindEntityByName(m_sLZEntityName);
		if (!lzEntity)
		{
			Print(string.Format("[EEF Helicopter] WARNING: LZ entity '%1' not found at handoff time. Group is orphaned at disembark point.", m_sLZEntityName), LogLevel.WARNING);
			return;
		}

		EEF_GroupReceiverComponent receiver = EEF_GroupReceiverComponent.Cast(
			lzEntity.FindComponent(EEF_GroupReceiverComponent)
		);

		if (!receiver)
		{
			Print(string.Format("[EEF Helicopter] WARNING: LZ entity '%1' has no EEF_GroupReceiverComponent. Group is orphaned at disembark point.", m_sLZEntityName), LogLevel.WARNING);
			return;
		}

		DebugLog(string.Format("Handing off cargo group to receiver on '%1'.", m_sLZEntityName));
		receiver.OnGroupReceived(m_CargoGroup);
	}

	// --------------------------------------------------------
	// HELPERS — STATE
	// --------------------------------------------------------

	protected void TransitionTo(EEF_EHelicopterInsertionState newState)
	{
		DebugLog(string.Format("State: %1 -> %2", StateName(m_eState), StateName(newState)));
		m_eState = newState;
		m_fStateEnteredTime = GetCurrentTime();
	}

	//! Abort back to IDLE on unrecoverable error and stop ticking.
	//! Resets transient state so the component can be re-triggered.
	protected void AbortToIdle()
	{
		m_eState = EEF_EHelicopterInsertionState.IDLE;
		m_bBoardingInitiated = false;
		m_bHandoffDone = false;
		m_bEngineStarted = false;
		m_CargoGroup = null;
		m_fRotorForceMultiplier = 0;
		if (m_aWaypoints)
			m_aWaypoints.Clear();
		m_iCurrentWaypointIndex = 0;
		StopFlightTick();
		StopTick();
	}

	protected void StopTick()
	{
		if (!m_bTickRunning)
			return;

		GetGame().GetCallqueue().Remove(Tick);
		m_bTickRunning = false;
	}

	// --------------------------------------------------------
	// HELPERS — UTILITY
	// --------------------------------------------------------

	protected SCR_BaseCompartmentManagerComponent FindCompartmentManager()
	{
		return SCR_BaseCompartmentManagerComponent.Cast(
			GetOwner().FindComponent(SCR_BaseCompartmentManagerComponent)
		);
	}

	protected float GetCurrentTime()
	{
		return GetGame().GetWorld().GetWorldTime() / 1000.0;
	}

	protected string StateName(EEF_EHelicopterInsertionState state)
	{
		switch (state)
		{
			case EEF_EHelicopterInsertionState.IDLE:			return "IDLE";
			case EEF_EHelicopterInsertionState.BOARDING:		return "BOARDING";
			case EEF_EHelicopterInsertionState.FLYING:			return "FLYING";
			case EEF_EHelicopterInsertionState.INSERTING:		return "INSERTING";
			case EEF_EHelicopterInsertionState.POST_INSERTION:	return "POST_INSERTION";
		}
		return "UNKNOWN";
	}

	protected void DebugLog(string message)
	{
		if (m_bDebugLog)
			Print("[EEF Helicopter] " + message);
	}
}
