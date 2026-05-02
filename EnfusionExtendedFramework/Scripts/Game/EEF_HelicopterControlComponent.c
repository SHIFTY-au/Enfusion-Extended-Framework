// ============================================================
// EEF_HelicopterControlComponent.c
// Enfusion Extended Framework
//
// Helicopter flight primitive for EEF. Attach this component
// (together with EEF_HelicopterInsertionComponent or other
// sibling modules) to a spawn-point entity in the World Editor
// at the desired helicopter spawn location.
//
// Responsibilities:
//   - Spawns the configured helicopter prefab at the owner
//     entity's world transform at runtime via SpawnHelicopter().
//   - Owns all flight logic: takeoff, cruise, approach, landing,
//     dwell, and optional fly-off/despawn.
//   - Fires script events so sibling components can react
//     without polling: OnHelicopterSpawned, OnLanded, OnDeparted.
//
// Sibling components (e.g. EEF_HelicopterInsertionComponent)
// subscribe to the events in OnPostInit, call SpawnHelicopter()
// to start the sequence, and call StartFlight() once their own
// setup (e.g. seating troops) is complete.
//
// Attach to: a spawn-point (empty/helper) entity.
// Runs on:   SERVER (authority) only.
// ============================================================

enum EEF_EHelicopterSpawnMode
{
    ON_GROUND,  //! Spawns on ground at owner transform. Lifts off on StartFlight().
    IN_AIR      //! Spawns at cruise altitude AGL. Skips takeoff phases; begins cruise immediately.
}

enum EEF_EHelicopterControlLandingMode
{
    FULL_LANDING,
    HOVER_LANDING
}

enum EEF_EHelicopterControlState
{
    IDLE,
    FLYING,
    ARRIVING,
    DEPARTING,
    LANDED
}

//! Internal flight phase. Drives the velocity/altitude profile this tick.
enum EEF_EFlightPhase
{
    TAKEOFF_VERTICAL,    //! Straight up to FLIGHT_TAKEOFF_VERTICAL_AGL.
    TAKEOFF_TRANSITION,  //! Begin forward flight while still climbing.
    CRUISE,              //! Forward flight at cruise altitude and configured speed.
    APPROACH_BLEED,      //! Within FLIGHT_APPROACH_RANGE of LZ - reduce speed, hold altitude.
    APPROACH_DESCENT,    //! Within FLIGHT_DESCENT_RANGE of LZ - begin descent.
    APPROACH_FLARE,      //! Within m_fPhysicsHandoffAGL of LZ - handoff to physics for touchdown.
    LANDED_PHASE,        //! Touchdown confirmed.
    HOVER_HOLD           //! Hovering at m_fHoverAltitudeAGL (HOVER_LANDING mode only).
}

[ComponentEditorProps(category: "EEF/Helicopter", description: "EEF Helicopter Control - attach to a spawn-point entity. Spawns a helicopter prefab at runtime and drives all flight behaviour. Fires OnHelicopterSpawned, OnLanded, OnDeparted events for sibling components.")]
class EEF_HelicopterControlComponentClass : ScriptComponentClass {}

class EEF_HelicopterControlComponent : ScriptComponent
{
    // --------------------------------------------------------
    // CONFIGURATION
    // --------------------------------------------------------

    [Attribute("", UIWidgets.ResourceNamePicker, "Helicopter prefab to spawn at runtime at the owner entity's transform.", "et")]
    protected ResourceName m_sHelicopterPrefab;

    [Attribute(EEF_EHelicopterSpawnMode.ON_GROUND.ToString(), UIWidgets.ComboBox, "ON_GROUND: spawns on ground and lifts off. IN_AIR: spawns at cruise altitude, begins cruise immediately.", "", ParamEnumArray.FromEnum(EEF_EHelicopterSpawnMode))]
    protected EEF_EHelicopterSpawnMode m_eSpawnMode;

    [Attribute("0", UIWidgets.CheckBox, "Spawn helicopter and start flight automatically on init. Disable when a sibling component (e.g. InsertionComponent) controls the sequence.")]
    protected bool m_bAutoStart;

    [Attribute("", UIWidgets.EditBox, "Optional destination entity name. The helicopter flies to this entity.")]
    protected string m_sDestinationEntityName;

    [Attribute("", UIWidgets.EditBox, "Optional intermediate waypoint 1 entity name.")]
    protected string m_sWaypoint1Name;

    [Attribute("", UIWidgets.EditBox, "Optional intermediate waypoint 2 entity name.")]
    protected string m_sWaypoint2Name;

    [Attribute("", UIWidgets.EditBox, "Optional intermediate waypoint 3 entity name.")]
    protected string m_sWaypoint3Name;

    [Attribute("80.0", UIWidgets.EditBox, "Cruise altitude in metres above ground level.")]
    protected float m_fCruiseAltitudeAGL;

    [Attribute("30.0", UIWidgets.EditBox, "Cruise speed in metres per second (m/s).")]
    protected float m_fCruiseSpeed;

    [Attribute(EEF_EHelicopterControlLandingMode.FULL_LANDING.ToString(), UIWidgets.ComboBox, "Landing mode used at the final waypoint.", "", ParamEnumArray.FromEnum(EEF_EHelicopterControlLandingMode))]
    protected EEF_EHelicopterControlLandingMode m_eLandingMode;

    [Attribute("4.0", UIWidgets.EditBox, "Hover altitude in metres AGL for HOVER_LANDING mode.")]
    protected float m_fHoverAltitudeAGL;

    [Attribute("8.0", UIWidgets.EditBox, "Distance in metres used to consider a waypoint reached.")]
    protected float m_fWaypointArrivalTolerance;

    [Attribute("2.0", UIWidgets.EditBox, "Altitude AGL in metres at which physics handoff begins for natural touchdown.")]
    protected float m_fPhysicsHandoffAGL;

    [Attribute("0", UIWidgets.CheckBox, "Print debug flight information to the server log.")]
    protected bool m_bDebugLog;

    [Attribute("0", UIWidgets.CheckBox, "Enable fly-off and despawn after landing. HOVER_LANDING only; ignored for FULL_LANDING.")]
    protected bool m_bEnableFlyOff;

    [Attribute("180.0", UIWidgets.EditBox, "Seconds to hover at LZ before departing. Set to 0 to wait for TriggerFlyOff() instead.")]
    protected float m_fDwellTime;

    [Attribute("0.0", UIWidgets.EditBox, "World-space departure heading in degrees (0 = north, 90 = east).")]
    protected float m_fDepartureHeadingDeg;

    [Attribute("1500.0", UIWidgets.EditBox, "Horizontal distance from LZ in metres at which the helicopter is deleted after fly-off.")]
    protected float m_fDespawnDistance;

    [Attribute("", UIWidgets.ResourceNamePicker, "Prefab path for the pilot character. Leave empty to spawn no pilot.", "et")]
    protected ResourceName m_sPilotPrefab;

    [Attribute("", UIWidgets.ResourceNamePicker, "Prefab path for the copilot character. Leave empty to spawn no copilot.", "et")]
    protected ResourceName m_sCopilotPrefab;

    // --------------------------------------------------------
    // SCRIPT EVENTS
    // Subscribe via GetOnHelicopterSpawned().Insert(MyCallback).
    // --------------------------------------------------------

    protected ref ScriptInvoker m_OnHelicopterSpawned = new ScriptInvoker();
    protected ref ScriptInvoker m_OnLanded = new ScriptInvoker();
    protected ref ScriptInvoker m_OnDeparted = new ScriptInvoker();

    //! Fired when the helicopter entity has been spawned. Callback signature: void Fn(IEntity heli).
    ScriptInvoker GetOnHelicopterSpawned() { return m_OnHelicopterSpawned; }
    //! Fired when the helicopter reaches the LZ (HOVER_HOLD or LANDED_PHASE). No parameters.
    ScriptInvoker GetOnLanded() { return m_OnLanded; }
    //! Fired when TriggerFlyOff() transitions the helicopter to DEPARTING state. No parameters.
    ScriptInvoker GetOnDeparted() { return m_OnDeparted; }

    // --------------------------------------------------------
    // RUNTIME STATE
    // --------------------------------------------------------

    protected IEntity m_HeliEntity;
    protected EEF_EHelicopterControlState m_eState = EEF_EHelicopterControlState.IDLE;
    protected bool m_bFlightTickRunning;
    protected bool m_bEngineStarted;
    protected int m_iCurrentWaypointIndex;
    protected ref array<vector> m_aWaypoints;
    // Densely sampled points along the path between configured waypoints. Built once at
    // StartFlight from m_aWaypoints. Provides the look-ahead targets for smooth banking.
    protected ref array<vector> m_aSplinePoints;
    protected VehicleHelicopterSimulation m_HelicopterSim;
    protected SCR_DamageManagerComponent m_DamageManager;
    //! Highest spline index reached. Look-ahead search never goes backwards.
    protected int m_iSplineProgressIndex;
    protected float m_fStatusLogTimer;
    protected IEntity m_PilotEntity;
    protected IEntity m_CopilotEntity;
    protected vector m_vLZOrigin;
    protected float m_fDwellTimer;
    protected bool m_bDwellActive;

    // --------------------------------------------------------
    // CONSTANTS
    // --------------------------------------------------------

    protected const float FLIGHT_VERTICAL_ARRIVAL_TOLERANCE = 1.5;
    protected const float SPLINE_SAMPLE_INTERVAL = 20.0;
    protected const float SPLINE_LOOKAHEAD_DISTANCE = 60.0;
    protected const float FLIGHT_ROTOR_FAILURE_RPM_THRESHOLD = 10.0;
    protected const float FLIGHT_DAMAGE_RELEASE_THRESHOLD = 0.4;

    // --------------------------------------------------------
    // INITIALISATION
    // --------------------------------------------------------

    override void OnPostInit(IEntity owner)
    {
        super.OnPostInit(owner);

        if (!Replication.IsServer())
            return;

        if (!m_aWaypoints)
            m_aWaypoints = new array<vector>();

        // Frame tick runs on the spawn-point owner. TickFlightController acts on m_HeliEntity.
        SetEventMask(owner, EntityEvent.FRAME);

        DebugLog("Initialised. Call SpawnHelicopter() to begin.");
    }

    override void OnDelete(IEntity owner)
    {
        if (Replication.IsServer())
        {
            if (m_PilotEntity)
            {
                SCR_EntityHelper.DeleteEntityAndChildren(m_PilotEntity);
                m_PilotEntity = null;
            }
            if (m_CopilotEntity)
            {
                SCR_EntityHelper.DeleteEntityAndChildren(m_CopilotEntity);
                m_CopilotEntity = null;
            }
            if (m_HeliEntity)
            {
                SCR_EntityHelper.DeleteEntityAndChildren(m_HeliEntity);
                m_HeliEntity = null;
            }
        }

        super.OnDelete(owner);
    }

    override void EOnFrame(IEntity owner, float timeSlice)
    {
        if (!Replication.IsServer())
            return;

        if (m_bFlightTickRunning)
            TickFlightController(timeSlice);
    }

    // --------------------------------------------------------
    // PUBLIC API
    // --------------------------------------------------------

    //! Spawn the helicopter prefab at the owner entity's world transform.
    //! Fires OnHelicopterSpawned(IEntity heli) when the entity is ready.
    //! If m_bAutoStart is true, StartFlight() is called automatically shortly after.
    //! Otherwise the caller (e.g. EEF_HelicopterInsertionComponent) calls StartFlight()
    //! once their own setup is complete.
    void SpawnHelicopter()
    {
        if (!Replication.IsServer())
            return;

        if (m_HeliEntity)
        {
            Print("[EEF HelicopterControl] WARNING: SpawnHelicopter called but helicopter already spawned.", LogLevel.WARNING);
            return;
        }

        if (m_sHelicopterPrefab.IsEmpty())
        {
            Print("[EEF HelicopterControl] ERROR: No helicopter prefab configured.", LogLevel.ERROR);
            return;
        }

        Resource res = Resource.Load(m_sHelicopterPrefab);
        if (!res || !res.IsValid())
        {
            Print("[EEF HelicopterControl] ERROR: Could not load helicopter prefab: " + m_sHelicopterPrefab, LogLevel.ERROR);
            return;
        }

        IEntity owner = GetOwner();
        vector mat[4];
        owner.GetWorldTransform(mat);

        if (m_eSpawnMode == EEF_EHelicopterSpawnMode.IN_AIR)
        {
            float groundY = GetSurfaceHeightAt(mat[3][0], mat[3][2]);
            mat[3][1] = groundY + m_fCruiseAltitudeAGL;
        }

        EntitySpawnParams spawnParams = new EntitySpawnParams();
        spawnParams.TransformMode = ETransformMode.WORLD;
        spawnParams.Transform[0] = mat[0];
        spawnParams.Transform[1] = mat[1];
        spawnParams.Transform[2] = mat[2];
        spawnParams.Transform[3] = mat[3];

        m_HeliEntity = GetGame().SpawnEntityPrefab(res, GetGame().GetWorld(), spawnParams);
        if (!m_HeliEntity)
        {
            Print("[EEF HelicopterControl] ERROR: Failed to spawn helicopter entity.", LogLevel.ERROR);
            return;
        }

        m_HelicopterSim = VehicleHelicopterSimulation.Cast(
            m_HeliEntity.GetRootParent().FindComponent(VehicleHelicopterSimulation)
        );
        if (!m_HelicopterSim)
            Print("[EEF HelicopterControl] WARNING: VehicleHelicopterSimulation not found on spawned helicopter. Flight control disabled.", LogLevel.WARNING);

        m_DamageManager = SCR_DamageManagerComponent.Cast(
            m_HeliEntity.FindComponent(SCR_DamageManagerComponent)
        );
        if (m_DamageManager)
            m_DamageManager.GetOnDamageStateChanged().Insert(OnVehicleDamageStateChanged);

        // Crew spawned after a short delay to allow the vehicle entity to fully initialise.
        GetGame().GetCallqueue().CallLater(SpawnCrew, 500, false);

        DebugLog("Helicopter spawned.");
        m_OnHelicopterSpawned.Invoke(m_HeliEntity);

        if (m_bAutoStart)
            GetGame().GetCallqueue().CallLater(AutoStartFlight, 100, false);
    }

    void StartFlight()
    {
        if (!Replication.IsServer())
            return;

        if (m_bFlightTickRunning)
            return;

        if (!m_HelicopterSim)
        {
            Print("[EEF HelicopterControl] ERROR: Cannot start flight - no VehicleHelicopterSimulation.", LogLevel.ERROR);
            return;
        }

        if (m_aWaypoints.IsEmpty())
        {
            if (!ResolveConfiguredWaypoints() && m_aWaypoints.IsEmpty())
            {
                Print("[EEF HelicopterControl] ERROR: No waypoints available to begin flight.", LogLevel.ERROR);
                return;
            }
        }

        if (m_aWaypoints.IsEmpty())
        {
            Print("[EEF HelicopterControl] WARNING: No waypoints configured. Flight will remain idle.", LogLevel.WARNING);
            return;
        }

        BuildSpline();

        m_iCurrentWaypointIndex = 0;
        m_iSplineProgressIndex = 0;
        m_fStatusLogTimer = 0;
        m_eState = EEF_EHelicopterControlState.FLYING;
        if (m_eSpawnMode == EEF_EHelicopterSpawnMode.IN_AIR)
            m_ePhase = EEF_EFlightPhase.CRUISE;
        else
            m_ePhase = EEF_EFlightPhase.TAKEOFF_VERTICAL;
        m_vCurrentVelocity = vector.Zero;
        m_bFlightTickRunning = true;
        m_bEngineStarted = true;
        m_bRotorForceApplied = false;
        m_bLandingShutdown = false;
        m_bDwellActive = false;
        m_fDwellTimer = 0;

        m_HelicopterSim.EngineStart();
        m_HelicopterSim.SetThrottle(FLIGHT_CONSTANT_THROTTLE);

        int splineCount = 0;
        if (m_aSplinePoints)
            splineCount = m_aSplinePoints.Count();
        DebugLog(string.Format("Flight started. Phase: %1. Waypoints: %2, spline samples: %3.",
            m_ePhase, m_aWaypoints.Count(), splineCount));
    }

    void StopFlight()
    {
        if (!Replication.IsServer())
            return;

        if (m_HelicopterSim)
        {
            m_HelicopterSim.SetThrottle(0);
            m_HelicopterSim.RotorSetForceScaleState(0, 0);
            m_HelicopterSim.RotorSetForceScaleState(1, 0);
        }

        m_bFlightTickRunning = false;
        m_bEngineStarted = false;
        m_eState = EEF_EHelicopterControlState.IDLE;
        DebugLog("Flight stopped.");
    }

    void ClearWaypoints()
    {
        if (!m_aWaypoints)
            m_aWaypoints = new array<vector>();
        m_aWaypoints.Clear();
        m_iCurrentWaypointIndex = 0;
    }

    void AddWaypoint(vector pos)
    {
        if (!m_aWaypoints)
            m_aWaypoints = new array<vector>();
        m_aWaypoints.Insert(pos);
    }

    //! Trigger an immediate fly-off from the LZ. Only valid when state is ARRIVING (HOVER_LANDING mode).
    //! Called automatically by the dwell timer, or externally by sibling modules.
    void TriggerFlyOff()
    {
        if (!Replication.IsServer())
            return;

        if (m_eState != EEF_EHelicopterControlState.ARRIVING)
            return;

        m_bDwellActive = false;
        m_eState = EEF_EHelicopterControlState.DEPARTING;
        m_ePhase = EEF_EFlightPhase.TAKEOFF_TRANSITION;
        m_OnDeparted.Invoke();
        DebugLog("TriggerFlyOff: departing LZ.");
    }

    // --------------------------------------------------------
    // CREW
    // --------------------------------------------------------

    protected void SpawnCrew()
    {
        if (!Replication.IsServer() || !m_HeliEntity)
            return;

        if (!m_sPilotPrefab.IsEmpty())
            m_PilotEntity = SpawnCrewMember(m_HeliEntity, m_sPilotPrefab, ECompartmentType.PILOT, "pilot");

        if (!m_sCopilotPrefab.IsEmpty())
            m_CopilotEntity = SpawnCrewMember(m_HeliEntity, m_sCopilotPrefab, ECompartmentType.PILOT, "copilot");
    }

    protected IEntity SpawnCrewMember(IEntity heli, ResourceName prefab, ECompartmentType compartmentType, string role)
    {
        Resource res = Resource.Load(prefab);
        if (!res || !res.IsValid())
        {
            Print(string.Format("[EEF HelicopterControl] WARNING: Could not load %1 prefab: %2", role, prefab), LogLevel.WARNING);
            return null;
        }

        EntitySpawnParams spawnParams = new EntitySpawnParams();
        spawnParams.TransformMode = ETransformMode.WORLD;
        Math3D.MatrixIdentity4(spawnParams.Transform);
        spawnParams.Transform[3] = heli.GetOrigin();

        IEntity crew = GetGame().SpawnEntityPrefab(res, GetGame().GetWorld(), spawnParams);
        if (!crew)
        {
            Print(string.Format("[EEF HelicopterControl] WARNING: Failed to spawn %1 entity.", role), LogLevel.WARNING);
            return null;
        }

        SCR_CompartmentAccessComponent access = SCR_CompartmentAccessComponent.Cast(
            crew.FindComponent(SCR_CompartmentAccessComponent)
        );
        if (!access)
        {
            Print(string.Format("[EEF HelicopterControl] WARNING: %1 entity missing SCR_CompartmentAccessComponent.", role), LogLevel.WARNING);
            return crew;
        }

        if (!access.MoveInVehicle(heli, compartmentType))
            Print(string.Format("[EEF HelicopterControl] WARNING: No %1 compartment slot found on vehicle prefab.", role), LogLevel.WARNING);
        else
            DebugLog(string.Format("%1 spawned and seated.", role));

        return crew;
    }

    protected void AutoStartFlight()
    {
        if (!Replication.IsServer())
            return;

        if (!GetGame() || !GetGame().GetWorld())
        {
            GetGame().GetCallqueue().CallLater(AutoStartFlight, 100, false);
            return;
        }

        BuildWaypoints();
        StartFlight();
    }

    // --------------------------------------------------------
    // WAYPOINTS
    // --------------------------------------------------------

    protected bool ResolveConfiguredWaypoints()
    {
        bool added = false;

        if (!m_aWaypoints)
            m_aWaypoints = new array<vector>();

        added |= ResolveOptionalWaypoint(m_sWaypoint1Name);
        added |= ResolveOptionalWaypoint(m_sWaypoint2Name);
        added |= ResolveOptionalWaypoint(m_sWaypoint3Name);

        if (!m_sDestinationEntityName.IsEmpty())
        {
            vector dest;
            if (ResolveEntityPosition(m_sDestinationEntityName, dest))
            {
                dest[1] = GetSurfaceHeightAt(dest[0], dest[2]) + m_fCruiseAltitudeAGL;
                m_aWaypoints.Insert(dest);
                added = true;
            }
        }

        return added;
    }

    protected bool ResolveOptionalWaypoint(string entityName)
    {
        if (entityName.IsEmpty())
            return false;

        vector pos;
        if (!ResolveEntityPosition(entityName, pos))
            return false;

        pos[1] = GetSurfaceHeightAt(pos[0], pos[2]) + m_fCruiseAltitudeAGL;
        m_aWaypoints.Insert(pos);
        return true;
    }

    protected bool ResolveEntityPosition(string entityName, out vector outPos)
    {
        if (!GetGame() || !GetGame().GetWorld())
            return false;

        IEntity ent = GetGame().GetWorld().FindEntityByName(entityName);
        if (!ent)
            return false;

        outPos = ent.GetOrigin();
        return true;
    }

    protected float GetSurfaceHeightAt(float x, float z)
    {
        if (!GetGame() || !GetGame().GetWorld())
            return 0;

        return GetGame().GetWorld().GetSurfaceY(x, z);
    }

    // --------------------------------------------------------
    // SPLINE PATH
    // --------------------------------------------------------

    protected void BuildSpline()
    {
        if (!m_aSplinePoints)
            m_aSplinePoints = new array<vector>();
        m_aSplinePoints.Clear();

        if (!m_aWaypoints || m_aWaypoints.IsEmpty() || !m_HeliEntity)
            return;

        vector here = m_HeliEntity.GetOrigin();
        float startGroundY = GetSurfaceHeightAt(here[0], here[2]);
        vector prev = Vector(here[0], startGroundY + m_fCruiseAltitudeAGL, here[2]);
        m_aSplinePoints.Insert(prev);

        foreach (vector wp : m_aWaypoints)
        {
            float wpGroundY = GetSurfaceHeightAt(wp[0], wp[2]);
            vector wpAtCruise = Vector(wp[0], wpGroundY + m_fCruiseAltitudeAGL, wp[2]);

            vector segDelta = wpAtCruise - prev;
            float segLen = segDelta.Length();

            if (segLen > 0.01)
            {
                int sampleCount = Math.Floor(segLen / SPLINE_SAMPLE_INTERVAL);
                vector segDir = segDelta * (1.0 / segLen);

                for (int i = 1; i <= sampleCount; i++)
                {
                    vector sample = prev + segDir * (SPLINE_SAMPLE_INTERVAL * i);
                    float sampleGroundY = GetSurfaceHeightAt(sample[0], sample[2]);
                    sample[1] = sampleGroundY + m_fCruiseAltitudeAGL;
                    m_aSplinePoints.Insert(sample);
                }
            }

            m_aSplinePoints.Insert(wpAtCruise);
            prev = wpAtCruise;
        }

        DebugLog(string.Format("Spline built: %1 sample points across %2 waypoint(s).", m_aSplinePoints.Count(), m_aWaypoints.Count()));
    }

    protected vector GetLookAheadTarget(vector currentPos)
    {
        if (!m_aSplinePoints || m_aSplinePoints.IsEmpty())
        {
            vector wp = m_aWaypoints[m_iCurrentWaypointIndex];
            float gy = GetSurfaceHeightAt(wp[0], wp[2]);
            return Vector(wp[0], gy + m_fCruiseAltitudeAGL, wp[2]);
        }

        int closestIdx = m_iSplineProgressIndex;
        float closestDistSq = float.MAX;
        for (int i = m_iSplineProgressIndex; i < m_aSplinePoints.Count(); i++)
        {
            vector sp = m_aSplinePoints[i];
            float dx = sp[0] - currentPos[0];
            float dz = sp[2] - currentPos[2];
            float distSq = dx * dx + dz * dz;
            if (distSq < closestDistSq)
            {
                closestDistSq = distSq;
                closestIdx = i;
            }
        }

        m_iSplineProgressIndex = closestIdx;

        float accumulated = 0;
        int targetIdx = closestIdx;
        for (int j = closestIdx; j < m_aSplinePoints.Count() - 1; j++)
        {
            vector segDelta = m_aSplinePoints[j + 1] - m_aSplinePoints[j];
            accumulated += segDelta.Length();
            targetIdx = j + 1;
            if (accumulated >= SPLINE_LOOKAHEAD_DISTANCE)
                break;
        }

        return m_aSplinePoints[targetIdx];
    }

    // --------------------------------------------------------
    // FLIGHT CONTROLLER
    //
    // Velocity-driven flight model. Each tick:
    //   1. Determine flight phase from altitude, distance to LZ, and waypoint state.
    //   2. Compute desired horizontal direction and speed from phase.
    //   3. Compute desired vertical speed from phase.
    //   4. Smoothly track current velocity toward desired (3s time constant).
    //   5. Compute desired attitude from acceleration.
    //   6. Smoothly track current attitude toward desired.
    //
    // All physics operations act on m_HeliEntity, not the spawn-point owner.
    // --------------------------------------------------------

    protected const float FLIGHT_TAKEOFF_VERTICAL_AGL = 5.0;
    protected const float FLIGHT_TAKEOFF_CLIMB_RATE = 4.0;
    protected const float FLIGHT_APPROACH_RANGE = 300.0;
    protected const float FLIGHT_APPROACH_SPEED = 22.0;
    protected const float FLIGHT_DESCENT_RANGE = 100.0;
    protected const float FLIGHT_DESCENT_RATE = 3.0;
    protected const float FLIGHT_FLARE_RATE = 0.8;
    protected const float FLIGHT_VELOCITY_TIME_CONSTANT = 3.0;
    protected const float FLIGHT_ATTITUDE_TIME_CONSTANT = 1.5;
    protected const float FLIGHT_MAX_PITCH_RAD = 0.35;
    protected const float FLIGHT_MAX_ROLL_RAD = 0.4;
    protected const float FLIGHT_PITCH_PER_ACCEL = 0.04;
    protected const float FLIGHT_ROLL_PER_LATERAL = 0.06;
    protected const float FLIGHT_CONSTANT_THROTTLE = 0.8;
    protected const float FLIGHT_TOUCHDOWN_AGL = 0.5;

    protected EEF_EFlightPhase m_ePhase = EEF_EFlightPhase.TAKEOFF_VERTICAL;
    protected vector m_vCurrentVelocity = vector.Zero;
    protected vector m_vDesiredHorizDir = vector.Zero;
    protected bool m_bRotorForceApplied = false;
    protected bool m_bLandingShutdown = false;

    protected void TickFlightController(float timeSlice)
    {
        if (!m_HelicopterSim || !m_aWaypoints || m_aWaypoints.IsEmpty() || !m_HeliEntity)
            return;

        Physics phys = m_HeliEntity.GetPhysics();
        if (!phys)
            return;

        m_HelicopterSim.SetThrottle(FLIGHT_CONSTANT_THROTTLE);

        if (!m_HelicopterSim.EngineIsOn())
            return;

        float rotorTargetRPM = m_HelicopterSim.RotorGetRPMTarget(0);
        float rotorRPM = m_HelicopterSim.RotorGetRPM(0);
        if (rotorTargetRPM > 0 && rotorRPM < rotorTargetRPM * 0.9)
        {
            m_fStatusLogTimer += timeSlice;
            if (m_bDebugLog && m_fStatusLogTimer >= 1.0)
            {
                m_fStatusLogTimer = 0;
                Print(string.Format("[EEF HelicopterControl] Spooling up: rotor 0 RPM %1 / target %2.", rotorRPM, rotorTargetRPM));
            }
            return;
        }

        if (!m_bRotorForceApplied)
        {
            m_HelicopterSim.RotorSetForceScaleState(0, 5.0);
            m_HelicopterSim.RotorSetForceScaleState(1, 5.0);
            m_bRotorForceApplied = true;
            DebugLog("Rotor force applied - flight control active.");
        }

        if (m_bRotorForceApplied)
        {
            if (m_HelicopterSim.RotorGetRPM(0) < FLIGHT_ROTOR_FAILURE_RPM_THRESHOLD)
            {
                OnRotorFailure("main rotor");
                return;
            }
            if (m_HelicopterSim.RotorGetRPM(1) < FLIGHT_ROTOR_FAILURE_RPM_THRESHOLD)
            {
                OnRotorFailure("tail rotor");
                return;
            }

            if (m_DamageManager && m_DamageManager.GetHealthScaled() < FLIGHT_DAMAGE_RELEASE_THRESHOLD)
            {
                OnRotorFailure("critical damage");
                return;
            }

            if (IsAllCrewDead())
            {
                OnRotorFailure("crew incapacitated");
                return;
            }
        }

        vector here = m_HeliEntity.GetOrigin();
        float groundY = GetSurfaceHeightAt(here[0], here[2]);
        float altAGL = here[1] - groundY;

        if (m_iCurrentWaypointIndex < 0 || m_iCurrentWaypointIndex >= m_aWaypoints.Count())
        {
            DebugLog(string.Format("Waypoint index %1 out of bounds (%2 waypoints). Treating as landed.", m_iCurrentWaypointIndex, m_aWaypoints.Count()));
            OnFlightArrived();
            return;
        }

        vector waypoint = m_aWaypoints[m_iCurrentWaypointIndex];
        bool isFinalWaypoint = (m_iCurrentWaypointIndex >= m_aWaypoints.Count() - 1);

        vector wpHorizDelta = Vector(waypoint[0] - here[0], 0, waypoint[2] - here[2]);
        float wpHorizDist = wpHorizDelta.Length();

        m_fStatusLogTimer += timeSlice;
        if (m_bDebugLog && m_fStatusLogTimer >= 1.0)
        {
            m_fStatusLogTimer = 0;
            vector finalWP = m_aWaypoints[m_aWaypoints.Count() - 1];
            float finalDist = Vector(finalWP[0] - here[0], 0, finalWP[2] - here[2]).Length();
            int splineCount = 0;
            if (m_aSplinePoints)
                splineCount = m_aSplinePoints.Count();
            Print(string.Format("[EEF HelicopterControl] Status: phase=%1, wp=%2/%3 (final=%4), dist_wp=%5m, dist_final=%6m, alt_agl=%7m, spline_progress=%8/%9",
                m_ePhase, m_iCurrentWaypointIndex, m_aWaypoints.Count() - 1, isFinalWaypoint, wpHorizDist, finalDist, altAGL, m_iSplineProgressIndex, splineCount));
        }

        EEF_EFlightPhase phaseBefore = m_ePhase;
        UpdateFlightPhase(altAGL, wpHorizDist, isFinalWaypoint);

        if (m_ePhase == EEF_EFlightPhase.HOVER_HOLD && phaseBefore != EEF_EFlightPhase.HOVER_HOLD)
            OnFlightArrived();
        else if (m_ePhase == EEF_EFlightPhase.LANDED_PHASE && phaseBefore != EEF_EFlightPhase.LANDED_PHASE
            && m_eLandingMode == EEF_EHelicopterControlLandingMode.FULL_LANDING)
            OnFlightArrived();

        if (m_bDwellActive && m_eState == EEF_EHelicopterControlState.ARRIVING)
        {
            m_fDwellTimer -= timeSlice;
            if (m_fDwellTimer <= 0)
            {
                m_bDwellActive = false;
                TriggerFlyOff();
            }
        }

        if (m_eState == EEF_EHelicopterControlState.DEPARTING)
        {
            float dx = here[0] - m_vLZOrigin[0];
            float dz = here[2] - m_vLZOrigin[2];
            float distFromLZ = Math.Sqrt(dx * dx + dz * dz);
            if (distFromLZ >= m_fDespawnDistance)
            {
                DebugLog(string.Format("Despawn distance reached (%.0fm from LZ). Scheduling entity deletion.", distFromLZ));
                m_bFlightTickRunning = false;
                GetGame().GetCallqueue().CallLater(DespawnHelicopter, 0, false);
                return;
            }
        }

        if (!isFinalWaypoint && wpHorizDist < m_fWaypointArrivalTolerance)
        {
            m_iCurrentWaypointIndex++;
            DebugLog(string.Format("Waypoint %1 reached, advancing to %2.", m_iCurrentWaypointIndex - 1, m_iCurrentWaypointIndex));
            return;
        }

        if (m_ePhase == EEF_EFlightPhase.LANDED_PHASE && m_eLandingMode != EEF_EHelicopterControlLandingMode.HOVER_LANDING)
        {
            if (!m_bLandingShutdown)
            {
                m_bLandingShutdown = true;
                DebugLog("Touchdown detected. Initiating engine shutdown sequence.");
            }
        }

        if (m_ePhase == EEF_EFlightPhase.APPROACH_FLARE)
        {
            m_HelicopterSim.RotorSetForceScaleState(0, 0.4);
            m_HelicopterSim.RotorSetForceScaleState(1, 0.4);
            m_HelicopterSim.SetThrottle(0.1);
            phys.SetAngularVelocity(vector.Zero);

            m_vCurrentVelocity[0] = 0;
            m_vCurrentVelocity[2] = 0;
            m_vCurrentVelocity[1] = Math.Max(m_vCurrentVelocity[1], -0.7);
            phys.SetVelocity(m_vCurrentVelocity);
            return;
        }

        vector horizDir;
        if (wpHorizDist > 0.5)
            horizDir = wpHorizDelta * (1.0 / wpHorizDist);
        else
            horizDir = vector.Zero;

        bool isFinalApproachFreeze = isFinalWaypoint
            && (m_ePhase == EEF_EFlightPhase.APPROACH_DESCENT || m_ePhase == EEF_EFlightPhase.HOVER_HOLD)
            && wpHorizDist < 30.0;

        if (horizDir.LengthSq() > 0.0001 && !isFinalApproachFreeze)
            m_vDesiredHorizDir = horizDir;

        if (isFinalApproachFreeze && m_vDesiredHorizDir.LengthSq() > 0.0001)
            horizDir = m_vDesiredHorizDir;

        if (m_eState == EEF_EHelicopterControlState.DEPARTING)
        {
            float headingRad = m_fDepartureHeadingDeg * (Math.PI / 180.0);
            horizDir = Vector(Math.Sin(headingRad), 0, Math.Cos(headingRad));
            m_vDesiredHorizDir = horizDir;
        }

        float desiredHorizSpeed = ComputeDesiredHorizSpeed(wpHorizDist);
        float desiredVertSpeed = ComputeDesiredVertSpeed(altAGL, here, waypoint, isFinalWaypoint, wpHorizDist);

        vector desiredVelocity = horizDir * desiredHorizSpeed;
        desiredVelocity[1] = desiredVertSpeed;

        if (m_bLandingShutdown)
        {
            desiredVelocity = vector.Zero;

            float horiSpeed = Vector(m_vCurrentVelocity[0], 0, m_vCurrentVelocity[2]).Length();
            float vertSpeedSq = m_vCurrentVelocity[1] * m_vCurrentVelocity[1];
            if (horiSpeed < 0.5 && vertSpeedSq < 0.25)
            {
                m_HelicopterSim.RotorSetForceScaleState(0, 0);
                m_HelicopterSim.RotorSetForceScaleState(1, 0);
                m_HelicopterSim.SetThrottle(0.0);
                m_HelicopterSim.EngineStop();
                m_bFlightTickRunning = false;
                m_eState = EEF_EHelicopterControlState.LANDED;
                DebugLog("Engine shutdown complete.");
                return;
            }
        }

        float velAlpha = Math.Clamp(timeSlice / FLIGHT_VELOCITY_TIME_CONSTANT, 0.0, 1.0);
        vector previousVelocity = m_vCurrentVelocity;
        m_vCurrentVelocity = m_vCurrentVelocity + (desiredVelocity - m_vCurrentVelocity) * velAlpha;

        if (m_eLandingMode == EEF_EHelicopterControlLandingMode.HOVER_LANDING
            && altAGL < m_fHoverAltitudeAGL + 4.0
            && (m_ePhase == EEF_EFlightPhase.APPROACH_DESCENT || m_ePhase == EEF_EFlightPhase.HOVER_HOLD))
        {
            float fastAlpha = Math.Clamp(timeSlice / 0.05, 0.0, 1.0);
            m_vCurrentVelocity[1] = m_vCurrentVelocity[1] + (desiredVelocity[1] - m_vCurrentVelocity[1]) * fastAlpha;
            if (altAGL <= m_fHoverAltitudeAGL && m_vCurrentVelocity[1] < 0)
                m_vCurrentVelocity[1] = 0;
        }

        phys.SetVelocity(m_vCurrentVelocity);

        vector accel = (m_vCurrentVelocity - previousVelocity) * (1.0 / Math.Max(timeSlice, 0.001));

        ApplyAttitude(phys, m_vCurrentVelocity, accel, m_vDesiredHorizDir, timeSlice);
    }

    protected void UpdateFlightPhase(float altAGL, float wpHorizDist, bool isFinalWaypoint)
    {
        if (m_eState == EEF_EHelicopterControlState.DEPARTING)
        {
            if (m_ePhase == EEF_EFlightPhase.TAKEOFF_TRANSITION && altAGL >= m_fCruiseAltitudeAGL - FLIGHT_VERTICAL_ARRIVAL_TOLERANCE)
            {
                m_ePhase = EEF_EFlightPhase.CRUISE;
                DebugLog("Phase: TAKEOFF_TRANSITION -> CRUISE (departing)");
            }
            return;
        }

        switch (m_ePhase)
        {
            case EEF_EFlightPhase.TAKEOFF_VERTICAL:
            {
                if (altAGL >= FLIGHT_TAKEOFF_VERTICAL_AGL)
                {
                    m_ePhase = EEF_EFlightPhase.TAKEOFF_TRANSITION;
                    DebugLog("Phase: TAKEOFF_VERTICAL -> TAKEOFF_TRANSITION");
                }
                return;
            }

            case EEF_EFlightPhase.TAKEOFF_TRANSITION:
            {
                if (altAGL >= m_fCruiseAltitudeAGL - FLIGHT_VERTICAL_ARRIVAL_TOLERANCE)
                {
                    m_ePhase = EEF_EFlightPhase.CRUISE;
                    DebugLog("Phase: TAKEOFF_TRANSITION -> CRUISE");
                }
                return;
            }

            case EEF_EFlightPhase.CRUISE:
            {
                if (isFinalWaypoint && wpHorizDist < FLIGHT_APPROACH_RANGE)
                {
                    m_ePhase = EEF_EFlightPhase.APPROACH_BLEED;
                    DebugLog("Phase: CRUISE -> APPROACH_BLEED");
                }
                return;
            }

            case EEF_EFlightPhase.APPROACH_BLEED:
            {
                if (wpHorizDist < FLIGHT_DESCENT_RANGE)
                {
                    m_ePhase = EEF_EFlightPhase.APPROACH_DESCENT;
                    DebugLog("Phase: APPROACH_BLEED -> APPROACH_DESCENT");
                }
                return;
            }

            case EEF_EFlightPhase.APPROACH_DESCENT:
            {
                bool isHover = (m_eLandingMode == EEF_EHelicopterControlLandingMode.HOVER_LANDING);

                if (isHover && m_fHoverAltitudeAGL > m_fPhysicsHandoffAGL)
                {
                    if (altAGL <= m_fHoverAltitudeAGL + FLIGHT_VERTICAL_ARRIVAL_TOLERANCE && wpHorizDist < m_fWaypointArrivalTolerance)
                    {
                        m_ePhase = EEF_EFlightPhase.HOVER_HOLD;
                        DebugLog("Phase: APPROACH_DESCENT -> HOVER_HOLD");
                    }
                }
                else
                {
                    float touchdownAGL;
                    if (isHover)
                        touchdownAGL = m_fHoverAltitudeAGL;
                    else
                        touchdownAGL = FLIGHT_TOUCHDOWN_AGL;

                    if (altAGL <= m_fPhysicsHandoffAGL)
                    {
                        m_ePhase = EEF_EFlightPhase.APPROACH_FLARE;
                        DebugLog("Phase: APPROACH_DESCENT -> APPROACH_FLARE");
                    }
                    else if (altAGL <= touchdownAGL && wpHorizDist < m_fWaypointArrivalTolerance)
                    {
                        m_ePhase = EEF_EFlightPhase.LANDED_PHASE;
                        DebugLog("Phase: APPROACH_DESCENT -> LANDED");
                    }
                }
                return;
            }

            case EEF_EFlightPhase.APPROACH_FLARE:
            {
                bool isHover = (m_eLandingMode == EEF_EHelicopterControlLandingMode.HOVER_LANDING);
                float touchdownAGL;
                if (isHover)
                    touchdownAGL = m_fHoverAltitudeAGL;
                else
                    touchdownAGL = FLIGHT_TOUCHDOWN_AGL;

                if (altAGL <= touchdownAGL && wpHorizDist < m_fWaypointArrivalTolerance)
                {
                    m_ePhase = EEF_EFlightPhase.LANDED_PHASE;
                    DebugLog("Phase: APPROACH_FLARE -> LANDED");
                }
                return;
            }

            case EEF_EFlightPhase.HOVER_HOLD:
                return;
        }
    }

    protected float ComputeDesiredHorizSpeed(float wpHorizDist)
    {
        switch (m_ePhase)
        {
            case EEF_EFlightPhase.TAKEOFF_VERTICAL:    return 0;
            case EEF_EFlightPhase.TAKEOFF_TRANSITION:  return m_fCruiseSpeed * 0.4;
            case EEF_EFlightPhase.CRUISE:              return m_fCruiseSpeed;
            case EEF_EFlightPhase.APPROACH_BLEED:
            {
                float approachFactor = Math.Clamp(wpHorizDist / FLIGHT_APPROACH_RANGE, 0.0, 1.0);
                float minSpeed = Math.Max(8.0, m_fCruiseSpeed * 0.25);
                return Math.Max(minSpeed, FLIGHT_APPROACH_SPEED * (0.35 + 0.65 * approachFactor));
            }
            case EEF_EFlightPhase.APPROACH_DESCENT:
            {
                if (m_eLandingMode == EEF_EHelicopterControlLandingMode.HOVER_LANDING && wpHorizDist < m_fWaypointArrivalTolerance)
                    return 0;
                float descentFactor = Math.Clamp(wpHorizDist / FLIGHT_DESCENT_RANGE, 0.0, 1.0);
                float minSpeed = Math.Max(5.0, m_fCruiseSpeed * 0.2);
                if (wpHorizDist < 20.0)
                    minSpeed = Math.Max(1.5, m_fCruiseSpeed * 0.08);
                if (wpHorizDist < 10.0)
                    minSpeed = Math.Max(0.5, m_fCruiseSpeed * 0.05);
                return Math.Max(minSpeed, FLIGHT_APPROACH_SPEED * 0.35 * descentFactor);
            }
            case EEF_EFlightPhase.APPROACH_FLARE:  return 0;
            case EEF_EFlightPhase.LANDED_PHASE:    return 0;
            case EEF_EFlightPhase.HOVER_HOLD:      return 0;
        }
        return 0;
    }

    protected float ComputeDesiredVertSpeed(float altAGL, vector here, vector waypoint, bool isFinalWaypoint, float wpHorizDist)
    {
        switch (m_ePhase)
        {
            case EEF_EFlightPhase.TAKEOFF_VERTICAL:
                return FLIGHT_TAKEOFF_CLIMB_RATE;

            case EEF_EFlightPhase.TAKEOFF_TRANSITION:
                if (altAGL >= m_fCruiseAltitudeAGL)
                    return 0;
                return FLIGHT_TAKEOFF_CLIMB_RATE * Math.Min(1.0, (m_fCruiseAltitudeAGL - altAGL) / 10.0);

            case EEF_EFlightPhase.CRUISE:
            {
                float targetAlt;
                if (isFinalWaypoint && m_eState != EEF_EHelicopterControlState.DEPARTING)
                    targetAlt = GetSurfaceHeightAt(waypoint[0], waypoint[2]) + m_fCruiseAltitudeAGL;
                else
                    targetAlt = GetSurfaceHeightAt(here[0], here[2]) + m_fCruiseAltitudeAGL;

                float altError = targetAlt - here[1];
                return Math.Clamp(altError * 0.5, -2.0, 2.0);
            }

            case EEF_EFlightPhase.APPROACH_BLEED:
            {
                float wpGroundY = GetSurfaceHeightAt(waypoint[0], waypoint[2]);
                float approachFloorAGL;
                if (m_eLandingMode == EEF_EHelicopterControlLandingMode.HOVER_LANDING)
                    approachFloorAGL = Math.Max(m_fHoverAltitudeAGL * 3.0, 12.0);
                else
                    approachFloorAGL = Math.Max(FLIGHT_TOUCHDOWN_AGL, 8.0);
                float progress = Math.Clamp((FLIGHT_APPROACH_RANGE - wpHorizDist) / FLIGHT_APPROACH_RANGE, 0.0, 1.0);
                float expo = Math.Pow(2.0, -6.0 * progress);
                float slopeAltitude = wpGroundY + approachFloorAGL + expo * (m_fCruiseAltitudeAGL - approachFloorAGL);
                float altError = slopeAltitude - here[1];
                return Math.Clamp(altError * 0.8, -5.0, 2.0);
            }

            case EEF_EFlightPhase.APPROACH_DESCENT:
            {
                float wpGroundY = GetSurfaceHeightAt(waypoint[0], waypoint[2]);
                float terminalAGL;
                float approachFloorAGL;
                if (m_eLandingMode == EEF_EHelicopterControlLandingMode.HOVER_LANDING)
                {
                    terminalAGL = m_fHoverAltitudeAGL;
                    approachFloorAGL = Math.Max(m_fHoverAltitudeAGL * 3.0, 12.0);
                }
                else
                {
                    terminalAGL = FLIGHT_TOUCHDOWN_AGL;
                    approachFloorAGL = 4.0;
                }
                float factor = Math.Clamp(wpHorizDist / FLIGHT_DESCENT_RANGE, 0.0, 1.0);
                float targetAltAGL = terminalAGL + factor * (approachFloorAGL - terminalAGL);
                float targetAlt = wpGroundY + targetAltAGL;
                float altError = targetAlt - here[1];
                float maxDescent = -5.0;
                if (m_eLandingMode == EEF_EHelicopterControlLandingMode.HOVER_LANDING && altAGL < terminalAGL + 4.0)
                    maxDescent = -1.5;
                return Math.Clamp(altError * 0.7, maxDescent, 1.0);
            }

            case EEF_EFlightPhase.APPROACH_FLARE:
                return 0;

            case EEF_EFlightPhase.LANDED_PHASE:
                return 0;

            case EEF_EFlightPhase.HOVER_HOLD:
            {
                float wpGroundY = GetSurfaceHeightAt(waypoint[0], waypoint[2]);
                float targetAlt = wpGroundY + m_fHoverAltitudeAGL;
                float altError = targetAlt - here[1];
                return Math.Clamp(altError * 0.7, -2.0, 2.0);
            }
        }
        return 0;
    }

    protected void ApplyAttitude(Physics phys, vector velocity, vector accel, vector desiredHorizDir, float timeSlice)
    {
        if (m_bLandingShutdown)
        {
            phys.SetAngularVelocity(vector.Zero);
            return;
        }

        vector mat[4];
        m_HeliEntity.GetWorldTransform(mat);
        vector heliUp = mat[1];
        vector heliForward = mat[2];

        vector heliFwdFlat = Vector(heliForward[0], 0, heliForward[2]);
        if (heliFwdFlat.LengthSq() < 0.0001)
        {
            phys.SetAngularVelocity(vector.Zero);
            return;
        }
        heliFwdFlat.Normalize();

        vector horizVel = Vector(velocity[0], 0, velocity[2]);
        float horizSpeed = horizVel.Length();

        vector desiredFwdFlat = heliFwdFlat;
        if (desiredHorizDir.LengthSq() > 0.0001)
            desiredFwdFlat = desiredHorizDir;
        else if (horizSpeed > 1.0)
            desiredFwdFlat = horizVel * (1.0 / horizSpeed);

        float forwardAccel = accel[0] * heliFwdFlat[0] + accel[2] * heliFwdFlat[2];
        float desiredPitch = Math.Clamp(-forwardAccel * FLIGHT_PITCH_PER_ACCEL, -FLIGHT_MAX_PITCH_RAD, FLIGHT_MAX_PITCH_RAD);

        vector heliRightFlat = Vector(heliFwdFlat[2], 0, -heliFwdFlat[0]);
        float lateralAccel = accel[0] * heliRightFlat[0] + accel[2] * heliRightFlat[2];
        float desiredRoll = Math.Clamp(-lateralAccel * FLIGHT_ROLL_PER_LATERAL, -FLIGHT_MAX_ROLL_RAD, FLIGHT_MAX_ROLL_RAD);

        vector desiredRight = Vector(desiredFwdFlat[2], 0, -desiredFwdFlat[0]);
        vector desiredUp = vector.Up;
        desiredUp = EEF_RotateAroundAxis(desiredUp, desiredFwdFlat, desiredRoll);
        desiredUp = EEF_RotateAroundAxis(desiredUp, desiredRight, desiredPitch);
        desiredUp.Normalize();

        vector angVel = vector.Zero;
        angVel = angVel + EEF_ComputeAngularVelocity(heliFwdFlat, desiredFwdFlat, FLIGHT_ATTITUDE_TIME_CONSTANT);
        angVel = angVel + EEF_ComputeAngularVelocity(heliUp, desiredUp, FLIGHT_ATTITUDE_TIME_CONSTANT);

        phys.SetAngularVelocity(angVel);
    }

    protected void OnFlightArrived()
    {
        m_eState = EEF_EHelicopterControlState.ARRIVING;
        m_vLZOrigin = m_HeliEntity.GetOrigin();
        m_OnLanded.Invoke();
        DebugLog("Final waypoint reached. Firing OnLanded.");

        if (m_eLandingMode == EEF_EHelicopterControlLandingMode.HOVER_LANDING)
        {
            if (m_bEnableFlyOff && m_fDwellTime > 0)
            {
                m_fDwellTimer = m_fDwellTime;
                m_bDwellActive = true;
                DebugLog(string.Format("Dwell timer started: %.0fs before fly-off.", m_fDwellTime));
            }
            else if (m_bEnableFlyOff)
            {
                DebugLog("Fly-off enabled with dwell 0 — waiting for TriggerFlyOff() call.");
            }
        }
        else if (m_bEnableFlyOff)
        {
            Print("[EEF HelicopterControl] WARNING: m_bEnableFlyOff is set but landing mode is FULL_LANDING. Fly-off requires HOVER_LANDING. Ignoring.", LogLevel.WARNING);
        }
    }

    protected void OnRotorFailure(string rotorName)
    {
        Print(string.Format("[EEF HelicopterControl] WARNING: Rotor failure detected (%1). Releasing flight control.", rotorName), LogLevel.WARNING);
        m_bFlightTickRunning = false;
        m_bLandingShutdown   = false;
        m_bRotorForceApplied = false;
        m_eState = EEF_EHelicopterControlState.IDLE;
    }

    protected void OnVehicleDamageStateChanged(EDamageState state)
    {
        if (state == EDamageState.DESTROYED)
            OnRotorFailure("vehicle destroyed");
    }

    protected bool IsAllCrewDead()
    {
        bool hasCrew = (m_PilotEntity != null || m_CopilotEntity != null);
        if (!hasCrew)
            return false;

        bool pilotDead   = !m_PilotEntity   || IsCrewMemberDead(m_PilotEntity);
        bool copilotDead = !m_CopilotEntity || IsCrewMemberDead(m_CopilotEntity);
        return pilotDead && copilotDead;
    }

    protected bool IsCrewMemberDead(IEntity crew)
    {
        if (!crew)
            return true;
        SCR_CharacterDamageManagerComponent dmg = SCR_CharacterDamageManagerComponent.Cast(
            crew.FindComponent(SCR_CharacterDamageManagerComponent)
        );
        if (!dmg)
            return false;
        return dmg.GetState() == EDamageState.DESTROYED;
    }

    // --------------------------------------------------------
    // HELPERS
    // --------------------------------------------------------

    protected void BuildWaypoints()
    {
        if (!m_aWaypoints)
            m_aWaypoints = new array<vector>();

        ClearWaypoints();
        if (!ResolveConfiguredWaypoints())
        {
            AddDefaultForwardWaypoint();
            DebugLog("No configured waypoints found; added default forward waypoint.");
        }
    }

    protected void AddDefaultForwardWaypoint()
    {
        if (!m_HeliEntity)
            return;

        vector mat[4];
        m_HeliEntity.GetWorldTransform(mat);
        vector forward = Vector(mat[2][0], 0, mat[2][2]);
        if (forward.LengthSq() < 0.0001)
            forward = Vector(0, 0, 1);
        forward.Normalize();

        vector here = m_HeliEntity.GetOrigin();
        vector dest = here + forward * 150.0;
        float groundY = GetSurfaceHeightAt(dest[0], dest[2]);
        dest[1] = groundY + m_fCruiseAltitudeAGL;

        m_aWaypoints.Insert(dest);
    }

    protected void DespawnHelicopter()
    {
        if (!m_HeliEntity)
            return;
        SCR_EntityHelper.DeleteEntityAndChildren(m_HeliEntity);
        m_HeliEntity = null;
    }

    protected static vector EEF_ComputeAngularVelocity(vector fromDir, vector toDir, float time)
    {
        fromDir.Normalize();
        toDir.Normalize();

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

    protected static vector EEF_RotateAroundAxis(vector v, vector axis, float angleRad)
    {
        axis.Normalize();
        float c = Math.Cos(angleRad);
        float s = Math.Sin(angleRad);
        float dot = axis[0] * v[0] + axis[1] * v[1] + axis[2] * v[2];
        return Vector(
            v[0] * c + (axis[1] * v[2] - axis[2] * v[1]) * s + axis[0] * dot * (1.0 - c),
            v[1] * c + (axis[2] * v[0] - axis[0] * v[2]) * s + axis[1] * dot * (1.0 - c),
            v[2] * c + (axis[0] * v[1] - axis[1] * v[0]) * s + axis[2] * dot * (1.0 - c)
        );
    }

    protected void DebugLog(string message)
    {
        if (m_bDebugLog)
            Print("[EEF HelicopterControl] " + message);
    }
}
