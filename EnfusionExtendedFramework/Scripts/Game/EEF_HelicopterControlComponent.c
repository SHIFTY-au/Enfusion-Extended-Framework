// ============================================================
// EEF_HelicopterControlComponent.c
// Enfusion Extended Framework
//
// Generic helicopter control layer for EEF. Attach this component
// to a helicopter entity and use it to steer any helicopter prefab
// with native `VehicleHelicopterSimulation`.
//
// This component is intentionally lightweight: it uses native
// helicopter throttle and angular steering while providing a
// reusable waypoint/landing control layer for future modules.
//
// Attach to: a helicopter vehicle entity.
// Runs on: SERVER (authority) only.
// ============================================================

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
    LANDED
}

//! Internal flight phase. Drives the velocity/altitude profile this tick.
enum EEF_EFlightPhase
{
    TAKEOFF_VERTICAL,    //! Straight up to FLIGHT_TAKEOFF_VERTICAL_AGL.
    TAKEOFF_TRANSITION,  //! Begin forward flight while still climbing.
    CRUISE,              //! Forward flight at cruise altitude and configured speed.
    APPROACH_BLEED,      //! Within FLIGHT_APPROACH_RANGE of LZ - reduce speed to FLIGHT_APPROACH_SPEED, hold altitude.
    APPROACH_DESCENT,    //! Within FLIGHT_DESCENT_RANGE of LZ - begin descent.
    APPROACH_FLARE,      //! Within m_fPhysicsHandoffAGL of LZ - handoff to physics for natural touchdown.
    LANDED_PHASE,        //! Touchdown confirmed.
    HOVER_HOLD           //! Hovering at m_fHoverAltitudeAGL indefinitely (HOVER_LANDING mode only).
}

[ComponentEditorProps(category: "EEF/Helicopter", description: "EEF Helicopter Control - native throttle/steering flight control for any helicopter.")]
class EEF_HelicopterControlComponentClass : ScriptComponentClass {}

class EEF_HelicopterControlComponent : ScriptComponent
{
    // --------------------------------------------------------
    // CONFIGURATION
    // --------------------------------------------------------

    [Attribute("0", UIWidgets.CheckBox, "Start flying automatically when the component is initialised.")]
    protected bool m_bAutoStart;

    [Attribute("", UIWidgets.EditBox, "Optional destination entity name. The helicopter will fly to this entity if provided.")]
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

    [Attribute(EEF_EHelicopterControlLandingMode.FULL_LANDING.ToString(), UIWidgets.ComboBox, "Landing mode used for the final waypoint.", "", ParamEnumArray.FromEnum(EEF_EHelicopterControlLandingMode))]
    protected EEF_EHelicopterControlLandingMode m_eLandingMode;

    [Attribute("4.0", UIWidgets.EditBox, "Hover altitude in metres AGL for HOVER_LANDING mode.")]
    protected float m_fHoverAltitudeAGL;

    [Attribute("8.0", UIWidgets.EditBox, "Distance in metres used to consider a waypoint reached.")]
    protected float m_fWaypointArrivalTolerance;

    [Attribute("2.0", UIWidgets.EditBox, "Altitude AGL in metres at which physics handoff begins for natural landing.")]
    protected float m_fPhysicsHandoffAGL;

    [Attribute("0", UIWidgets.CheckBox, "Print debug flight information to the server log.")]
    protected bool m_bDebugLog;

    // --------------------------------------------------------
    // RUNTIME STATE
    // --------------------------------------------------------

    protected EEF_EHelicopterControlState m_eState = EEF_EHelicopterControlState.IDLE;
    protected bool m_bFlightTickRunning;
    protected bool m_bEngineStarted;
    protected int m_iCurrentWaypointIndex;
    protected ref array<vector> m_aWaypoints;
    // Densely sampled points along the path between configured waypoints. Built once at
    // StartFlight from m_aWaypoints. Provides the look-ahead targets that give the helicopter
    // a smooth bank-into-turn feel rather than aiming straight at the next discrete waypoint.
    protected ref array<vector> m_aSplinePoints;
    protected VehicleHelicopterSimulation m_HelicopterSim;
    //! Highest spline index the helicopter has reached. The look-ahead search will not
    //! search backwards from this point, preventing the controller from chasing earlier
    //! waypoints once the helicopter has progressed past them.
    protected int m_iSplineProgressIndex;
    //! Throttle for status debug logs (per-second).
    protected float m_fStatusLogTimer;

    // --------------------------------------------------------
    // CONSTANTS
    // --------------------------------------------------------

    protected const float FLIGHT_VERTICAL_ARRIVAL_TOLERANCE = 1.5;

    // Spline sampling - points are placed every SPLINE_SAMPLE_INTERVAL metres between
    // consecutive configured waypoints. Smaller = smoother but more memory/compute.
    protected const float SPLINE_SAMPLE_INTERVAL = 20.0;
    // Look-ahead distance the controller aims at, measured along the spline from the
    // closest sample point to the helicopter's current position. Tunes how aggressively
    // the helicopter banks into turns - smaller = sharper, larger = smoother and slower.
    protected const float SPLINE_LOOKAHEAD_DISTANCE = 60.0;

    // --------------------------------------------------------
    // INITIALISATION
    // --------------------------------------------------------

    override void OnPostInit(IEntity owner)
    {
        super.OnPostInit(owner);

        if (!Replication.IsServer())
            return;

        m_HelicopterSim = VehicleHelicopterSimulation.Cast(
            owner.GetRootParent().FindComponent(VehicleHelicopterSimulation)
        );

        if (!m_HelicopterSim)
        {
            Print("[EEF HelicopterControl] WARNING: VehicleHelicopterSimulation not found. Flight control will be disabled.", LogLevel.WARNING);
            return;
        }

        if (!m_aWaypoints)
            m_aWaypoints = new array<vector>();

        if (m_bAutoStart)
        {
            GetGame().GetCallqueue().CallLater(AutoStartFlight, 100, false, owner);
        }

        SetEventMask(owner, EntityEvent.FRAME);
        DebugLog("Initialised.");
    }

    protected void AutoStartFlight(IEntity owner)
    {
        if (!Replication.IsServer())
            return;

        if (!GetGame() || !GetGame().GetWorld())
        {
            GetGame().GetCallqueue().CallLater(AutoStartFlight, 100, false, owner);
            return;
        }

        BuildWaypoints(owner);
        StartFlight(owner);
    }

    override void EOnFrame(IEntity owner, float timeSlice)
    {
        if (!Replication.IsServer())
            return;

        if (m_bFlightTickRunning)
            TickFlightController(owner, timeSlice);
    }

    // --------------------------------------------------------
    // PUBLIC API
    // --------------------------------------------------------

    void StartFlight(IEntity owner)
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
            if (!ResolveConfiguredWaypoints(owner) && m_aWaypoints.IsEmpty())
            {
                Print("[EEF Helicopter] ERROR: No waypoint available to begin flight.", LogLevel.ERROR);
                return;
            }
        }

        if (m_aWaypoints.IsEmpty())
        {
            Print("[EEF Helicopter] WARNING: No waypoints configured. Flight will remain idle.", LogLevel.WARNING);
            return;
        }

        BuildSpline(owner);

        m_iCurrentWaypointIndex = 0;
        m_iSplineProgressIndex = 0;
        m_fStatusLogTimer = 0;
        m_eState = EEF_EHelicopterControlState.FLYING;
        m_ePhase = EEF_EFlightPhase.TAKEOFF_VERTICAL;
        m_vCurrentVelocity = vector.Zero;
        m_bFlightTickRunning = true;
        m_bEngineStarted = true;
        m_bRotorForceApplied = false;
        m_bLandingShutdown = false;

        m_HelicopterSim.EngineStart();
        m_HelicopterSim.SetThrottle(FLIGHT_CONSTANT_THROTTLE);

        // Keep rotor force at 0 until after spool-up. Rotor force is applied in TickFlightController
        // after the engine RPM reaches target, preventing uncontrolled lift during startup.

        // Startup status: confirms the configured waypoint set actually resolved.
        int splineCount = 0;
        if (m_aSplinePoints)
            splineCount = m_aSplinePoints.Count();
        DebugLog(string.Format("Flight started. Waypoints: %1, spline samples: %2.", m_aWaypoints.Count(), splineCount));
    }

    void StopFlight(IEntity owner)
    {
        if (!Replication.IsServer())
            return;

        if (m_HelicopterSim)
        {
            m_HelicopterSim.SetThrottle(0);
            // Drop rotor force scaling to zero. Together with throttle 0 this mirrors
            // Darc's landing approach - the helicopter loses lift cleanly rather than
            // continuing to push against the ground.
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

    bool StartFlightToPosition(IEntity owner, vector position)
    {
        if (!Replication.IsServer())
            return false;

        ClearWaypoints();
        AddWaypoint(Vector(position[0], position[1], position[2]));
        StartFlight(owner);
        return m_bFlightTickRunning;
    }

    bool StartFlightToEntity(IEntity owner, string entityName)
    {
        if (entityName.IsEmpty())
            return false;

        IEntity ent = GetGame().GetWorld().FindEntityByName(entityName);
        if (!ent)
            return false;

        vector pos = ent.GetOrigin();
        StartFlightToPosition(owner, pos);
        return m_bFlightTickRunning;
    }

    // --------------------------------------------------------
    // WAYPOINTS
    // --------------------------------------------------------

    protected bool ResolveConfiguredWaypoints(IEntity owner)
    {
        bool added = false;

        if (!m_aWaypoints)
            m_aWaypoints = new array<vector>();

        // Intermediates are inserted FIRST in their declared order. The destination is
        // appended LAST so the controller correctly treats it as the final waypoint
        // (which triggers the approach/descent profile - intermediates are flown through
        // at cruise speed and altitude).
        added |= ResolveOptionalWaypoint(owner, m_sWaypoint1Name);
        added |= ResolveOptionalWaypoint(owner, m_sWaypoint2Name);
        added |= ResolveOptionalWaypoint(owner, m_sWaypoint3Name);

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

    protected bool ResolveOptionalWaypoint(IEntity owner, string entityName)
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
        {
            return false;
        }

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

    //! Build a densely sampled point list along the path from helicopter origin through
    //! all configured waypoints. Linear interpolation - smooth enough for our purposes
    //! and avoids Catmull-Rom complexity for the typical 1-3 waypoint case.
    //! Each sample is set to cruise altitude AGL above local terrain.
    protected void BuildSpline(IEntity owner)
    {
        if (!m_aSplinePoints)
            m_aSplinePoints = new array<vector>();
        m_aSplinePoints.Clear();

        if (!m_aWaypoints || m_aWaypoints.IsEmpty())
            return;

        // Start the spline at the helicopter's current XZ position so look-ahead immediately
        // has somewhere to target on the first tick. The Y is set to cruise altitude.
        vector here = owner.GetOrigin();
        float startGroundY = GetSurfaceHeightAt(here[0], here[2]);
        vector prev = Vector(here[0], startGroundY + m_fCruiseAltitudeAGL, here[2]);
        m_aSplinePoints.Insert(prev);

        // Sample between each consecutive pair: prev -> waypoint, prev -> next waypoint, etc.
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

                // Insert intermediate samples. Each sample's altitude is set against its own
                // local terrain so the spline follows the ground undulation rather than
                // cutting through hills.
                for (int i = 1; i <= sampleCount; i++)
                {
                    vector sample = prev + segDir * (SPLINE_SAMPLE_INTERVAL * i);
                    float sampleGroundY = GetSurfaceHeightAt(sample[0], sample[2]);
                    sample[1] = sampleGroundY + m_fCruiseAltitudeAGL;
                    m_aSplinePoints.Insert(sample);
                }
            }

            // Always insert the waypoint itself so the spline ends exactly on the configured point.
            m_aSplinePoints.Insert(wpAtCruise);
            prev = wpAtCruise;
        }

        DebugLog(string.Format("Spline built: %1 sample points across %2 waypoint(s).", m_aSplinePoints.Count(), m_aWaypoints.Count()));
    }

    //! Find a point along the spline ahead of the helicopter, used as the steering target.
    //! Walks the spline from the closest sample to find one approximately
    //! SPLINE_LOOKAHEAD_DISTANCE metres further along.
    //!
    //! Search is restricted to indices >= m_iSplineProgressIndex - the helicopter never
    //! looks backwards. This prevents a kinked path (e.g. zig-zag waypoints) from causing
    //! the helicopter to chase an earlier segment that's geometrically close to its current
    //! position, which would otherwise produce a cycle between waypoints.
    protected vector GetLookAheadTarget(vector currentPos)
    {
        if (!m_aSplinePoints || m_aSplinePoints.IsEmpty())
        {
            // Fallback: just aim at the current waypoint at cruise altitude.
            vector wp = m_aWaypoints[m_iCurrentWaypointIndex];
            float gy = GetSurfaceHeightAt(wp[0], wp[2]);
            return Vector(wp[0], gy + m_fCruiseAltitudeAGL, wp[2]);
        }

        // Find the closest spline sample to current position (XZ only), only searching
        // forward from the highest progress index ever reached.
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

        // Lock in this progress so future searches won't go backwards.
        m_iSplineProgressIndex = closestIdx;

        // Walk forward along the spline accumulating distance until we've gone LOOKAHEAD,
        // or we've run out of samples (in which case we return the last one - the helicopter
        // is near the end of the path).
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
    //   5. Compute desired attitude from acceleration:
    //        - Pitch: forward when accelerating, neutral at constant velocity.
    //        - Roll:  bank into turns (lateral acceleration).
    //        - Yaw:   align with horizontal velocity direction.
    //   6. Smoothly track current attitude toward desired.
    //
    // Throttle is held constant for engine sound and rotor visuals only - lift comes from
    // the vertical component of SetVelocity, not from the simulation's lift response.
    // --------------------------------------------------------

    // Flight profile constants. Tuned for cinematic feel; locked for v1.
    protected const float FLIGHT_TAKEOFF_VERTICAL_AGL = 5.0;     //! Altitude AGL to clear vertically before transitioning.
    protected const float FLIGHT_TAKEOFF_CLIMB_RATE = 4.0;       //! m/s vertical during takeoff and climb.
    protected const float FLIGHT_APPROACH_RANGE = 300.0;         //! Horizontal m from LZ at which speed bleed begins.
    protected const float FLIGHT_APPROACH_SPEED = 22.0;          //! Target speed during approach bleed (~80 km/h).
    protected const float FLIGHT_DESCENT_RANGE = 100.0;          //! Horizontal m from LZ at which descent begins.
    protected const float FLIGHT_DESCENT_RATE = 3.0;             //! m/s descent during APPROACH_DESCENT.
    protected const float FLIGHT_FLARE_RATE = 0.8;               //! m/s descent in final 5m AGL.
    protected const float FLIGHT_VELOCITY_TIME_CONSTANT = 3.0;   //! Seconds to track 63% toward target velocity.
    protected const float FLIGHT_ATTITUDE_TIME_CONSTANT = 1.5;   //! Seconds to track 63% toward target attitude. Tighter than velocity for snappier visual response.
    protected const float FLIGHT_MAX_PITCH_RAD = 0.35;           //! Max forward/back tilt (~20 degrees).
    protected const float FLIGHT_MAX_ROLL_RAD = 0.4;             //! Max bank angle (~23 degrees).
    protected const float FLIGHT_PITCH_PER_ACCEL = 0.04;         //! Radians of pitch per m/s^2 acceleration.
    protected const float FLIGHT_ROLL_PER_LATERAL = 0.06;        //! Radians of roll per m/s^2 lateral accel.
    protected const float FLIGHT_CRUISE_NOSE_DOWN_RAD = 0.12;    //! ~7 degrees nose-down bias at full cruise speed.
    protected const float FLIGHT_CONSTANT_THROTTLE = 0.8;        //! Throttle held constant for engine/rotor visuals.
    protected const float FLIGHT_TOUCHDOWN_AGL = 0.5;             //! AGL below which we consider the helicopter landed.

    // Persistent state across ticks for smoothing.
    protected EEF_EFlightPhase m_ePhase = EEF_EFlightPhase.TAKEOFF_VERTICAL;
    protected vector m_vCurrentVelocity = vector.Zero; //! Tracked velocity, lerped toward desired.
    protected vector m_vDesiredHorizDir = vector.Zero; //! Last stable heading direction used during approach.
    protected bool m_bRotorForceApplied = false; //! Flag to track if rotor force has been set after spool-up.
    protected bool m_bLandingShutdown = false; //! Flag to track if we're in the landing shutdown sequence.

    protected void TickFlightController(IEntity owner, float timeSlice)
    {
        if (!m_HelicopterSim || !m_aWaypoints || m_aWaypoints.IsEmpty())
            return;

        Physics phys = owner.GetPhysics();
        if (!phys)
            return;

        // Hold throttle constant - lift is now driven by SetVelocity, not throttle.
        // Throttle is commanded even before engine is ready so the spool-up actually progresses.
        m_HelicopterSim.SetThrottle(FLIGHT_CONSTANT_THROTTLE);

        // Wait for engine spool-up before commanding velocity or attitude. EngineIsOn() is
        // a binary state flag (engine switched on yes/no), not a readiness signal - it
        // returns true the moment EngineStart is called. The actual readiness condition is
        // rotor RPM reaching target, which we check via RotorGetRPM vs RotorGetRPMTarget.
        if (!m_HelicopterSim.EngineIsOn())
        {
            return;
        }

        float rotorTargetRPM = m_HelicopterSim.RotorGetRPMTarget(0);
        float rotorRPM = m_HelicopterSim.RotorGetRPM(0);
        if (rotorTargetRPM > 0 && rotorRPM < rotorTargetRPM * 0.9)
        {
            // Throttled debug log so we can see spool-up progress.
            m_fStatusLogTimer += timeSlice;
            if (m_bDebugLog && m_fStatusLogTimer >= 1.0)
            {
                m_fStatusLogTimer = 0;
                Print(string.Format("[EEF HelicopterControl] Spooling up: rotor 0 RPM %1 / target %2.", rotorRPM, rotorTargetRPM));
            }
            return;
        }

        // Apply rotor force once spool-up is complete. This prevents uncontrolled lift during startup.
        if (!m_bRotorForceApplied)
        {
            m_HelicopterSim.RotorSetForceScaleState(0, 5.0);
            m_HelicopterSim.RotorSetForceScaleState(1, 5.0);
            m_bRotorForceApplied = true;
            DebugLog("Rotor force applied - flight control active.");
        }

        vector here = owner.GetOrigin();
        float groundY = GetSurfaceHeightAt(here[0], here[2]);
        float altAGL = here[1] - groundY;

        // Defensive bounds check. If somehow we've advanced past the last waypoint, treat
        // as landed. Should not happen with the current advance logic, but guards against
        // edge cases in re-entrant flight starts.
        if (m_iCurrentWaypointIndex < 0 || m_iCurrentWaypointIndex >= m_aWaypoints.Count())
        {
            DebugLog(string.Format("Waypoint index %1 out of bounds (%2 waypoints). Treating as landed.", m_iCurrentWaypointIndex, m_aWaypoints.Count()));
            OnFlightArrived(owner);
            return;
        }

        // Resolve current waypoint and final-waypoint flag.
        vector waypoint = m_aWaypoints[m_iCurrentWaypointIndex];
        bool isFinalWaypoint = (m_iCurrentWaypointIndex >= m_aWaypoints.Count() - 1);

        // Horizontal distance to current waypoint. Used for phase detection on the final
        // waypoint, and for waypoint advance on intermediates.
        vector wpHorizDelta = Vector(waypoint[0] - here[0], 0, waypoint[2] - here[2]);
        float wpHorizDist = wpHorizDelta.Length();

        // Per-second status snapshot: lets us see what the controller is actually doing
        // when phase transitions and waypoint advances are sparse. Throttled so it doesn't
        // flood the log on every flight tick.
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

        // --- Phase detection ---
        // Bug 4 fix: track phase before update so we can detect HOVER_HOLD entry and fire OnFlightArrived.
        EEF_EFlightPhase phaseBefore = m_ePhase;
        UpdateFlightPhase(altAGL, wpHorizDist, isFinalWaypoint);
        if (m_ePhase == EEF_EFlightPhase.HOVER_HOLD && phaseBefore != EEF_EFlightPhase.HOVER_HOLD)
            OnFlightArrived(owner);

        // Check arrival on intermediate waypoints (advance), or on final (land).
        if (!isFinalWaypoint && wpHorizDist < m_fWaypointArrivalTolerance)
        {
            m_iCurrentWaypointIndex++;
            DebugLog(string.Format("Waypoint %1 reached, advancing to %2.", m_iCurrentWaypointIndex - 1, m_iCurrentWaypointIndex));
            return;
        }

        // Bug 3 fix: only shut down on LANDED_PHASE for FULL_LANDING. HOVER_LANDING stabilises
        // into HOVER_HOLD and keeps the engine running.
        if (m_ePhase == EEF_EFlightPhase.LANDED_PHASE && m_eLandingMode != EEF_EHelicopterControlLandingMode.HOVER_LANDING)
        {
            if (!m_bLandingShutdown)
            {
                m_bLandingShutdown = true;
                DebugLog("Touchdown detected. Initiating engine shutdown sequence.");
            }
        }

        // Physics handoff: stop aggressive scripted control and allow a soft natural descent.
        if (m_ePhase == EEF_EFlightPhase.APPROACH_FLARE)
        {
            // Set minimal rotor force and throttle for stability without full control.
            m_HelicopterSim.RotorSetForceScaleState(0, 0.4);
            m_HelicopterSim.RotorSetForceScaleState(1, 0.4);
            m_HelicopterSim.SetThrottle(0.1);
            phys.SetAngularVelocity(vector.Zero); // Dampen any rotation.

            // Prevent residual scripted downward velocity from forcing the skids through terrain.
            m_vCurrentVelocity[0] = 0;
            m_vCurrentVelocity[2] = 0;
            m_vCurrentVelocity[1] = Math.Max(m_vCurrentVelocity[1], -0.7);
            phys.SetVelocity(m_vCurrentVelocity);
            return;
        }

        // --- Steering target (horizontal direction) ---
        vector horizDir;
        if (wpHorizDist > 0.5)
            horizDir = wpHorizDelta * (1.0 / wpHorizDist);
        else
            horizDir = vector.Zero;

        if (horizDir.LengthSq() > 0.0001)
            m_vDesiredHorizDir = horizDir;

        if (isFinalWaypoint && m_ePhase == EEF_EFlightPhase.APPROACH_DESCENT && wpHorizDist < 20.0 && m_vDesiredHorizDir.LengthSq() > 0.0001)
            horizDir = m_vDesiredHorizDir;

        float desiredHorizSpeed = ComputeDesiredHorizSpeed(wpHorizDist);
        float desiredVertSpeed = ComputeDesiredVertSpeed(altAGL, here, waypoint, isFinalWaypoint, wpHorizDist);

        vector desiredVelocity = horizDir * desiredHorizSpeed;
        desiredVelocity[1] = desiredVertSpeed;

        // Landing shutdown: bring velocity and rotor force to zero before stopping control loop.
        if (m_bLandingShutdown)
        {
            // Force desired velocity to zero during shutdown.
            desiredVelocity = vector.Zero;
            
            // Reduce rotor force gradually to prevent sudden loss of lift.
            float horiSpeed = Vector(m_vCurrentVelocity[0], 0, m_vCurrentVelocity[2]).Length();
            float vertSpeedSq = m_vCurrentVelocity[1] * m_vCurrentVelocity[1];
            if (horiSpeed < 0.5 && vertSpeedSq < 0.25)  // 0.5^2 = 0.25
            {
                // Velocity near zero - shut down the engine.
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

        // --- Velocity smoothing (3s time constant exponential approach) ---
        // Velocity smoothing alpha. Linear approximation of (1 - exp(-dt/TC)), accurate
        // to within ~5% for the dt/TC ratios we deal with (~0.03 typical). Math.Exp is
        // not available in Enforce script. Clamped to [0, 1] in case dt is unusually
        // large (e.g. first tick after a long pause) so alpha can't overshoot.
        float velAlpha = Math.Clamp(timeSlice / FLIGHT_VELOCITY_TIME_CONSTANT, 0.0, 1.0);
        vector previousVelocity = m_vCurrentVelocity;
        m_vCurrentVelocity = m_vCurrentVelocity + (desiredVelocity - m_vCurrentVelocity) * velAlpha;

        // Apply velocity. SetVelocity overrides the engine's own velocity each tick.
        phys.SetVelocity(m_vCurrentVelocity);

        // --- Compute acceleration this tick (used for attitude) ---
        vector accel = (m_vCurrentVelocity - previousVelocity) * (1.0 / Math.Max(timeSlice, 0.001));

        // --- Apply attitude based on velocity and acceleration ---
        ApplyAttitude(owner, phys, m_vCurrentVelocity, accel, m_vDesiredHorizDir, timeSlice);
    }

    //! Determine which flight phase we should be in based on altitude and distance to LZ.
    protected void UpdateFlightPhase(float altAGL, float wpHorizDist, bool isFinalWaypoint)
    {
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

                // Bug 1 fix: in hover mode the hover altitude is above the physics handoff threshold,
                // so APPROACH_FLARE would fire first. Skip it entirely and go straight to HOVER_HOLD.
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
                // Hover indefinitely. Departure triggered externally via TriggerFlyOff() (#2).
                return;
        }
    }

    //! Horizontal speed for the current phase.
    protected float ComputeDesiredHorizSpeed(float wpHorizDist)
    {
        switch (m_ePhase)
        {
            case EEF_EFlightPhase.TAKEOFF_VERTICAL:    return 0;
            case EEF_EFlightPhase.TAKEOFF_TRANSITION:  return m_fCruiseSpeed * 0.4; // Gentle forward as we climb.
            case EEF_EFlightPhase.CRUISE:              return m_fCruiseSpeed;
            case EEF_EFlightPhase.APPROACH_BLEED:
            {
                float approachFactor = Math.Clamp(wpHorizDist / FLIGHT_APPROACH_RANGE, 0.0, 1.0);
                float minSpeed = Math.Max(8.0, m_fCruiseSpeed * 0.25);
                return Math.Max(minSpeed, FLIGHT_APPROACH_SPEED * (0.35 + 0.65 * approachFactor));
            }
            case EEF_EFlightPhase.APPROACH_DESCENT:
            {
                float descentFactor = Math.Clamp(wpHorizDist / FLIGHT_DESCENT_RANGE, 0.0, 1.0);
                float minSpeed = Math.Max(5.0, m_fCruiseSpeed * 0.2);
                if (wpHorizDist < 20.0)
                    minSpeed = Math.Max(1.5, m_fCruiseSpeed * 0.08);
                if (wpHorizDist < 10.0)
                    minSpeed = Math.Max(0.5, m_fCruiseSpeed * 0.05);
                return Math.Max(minSpeed, FLIGHT_APPROACH_SPEED * 0.35 * descentFactor);
            }
            case EEF_EFlightPhase.APPROACH_FLARE:           return 0;
            case EEF_EFlightPhase.LANDED_PHASE:              return 0;
            case EEF_EFlightPhase.HOVER_HOLD:               return 0;
        }
        return 0;
    }

    //! Vertical speed for the current phase.
    protected float ComputeDesiredVertSpeed(float altAGL, vector here, vector waypoint, bool isFinalWaypoint, float wpHorizDist)
    {
        switch (m_ePhase)
        {
            case EEF_EFlightPhase.TAKEOFF_VERTICAL:
                return FLIGHT_TAKEOFF_CLIMB_RATE;

            case EEF_EFlightPhase.TAKEOFF_TRANSITION:
                // Continue climbing, but ease as we approach cruise altitude.
                if (altAGL >= m_fCruiseAltitudeAGL)
                    return 0;
                return FLIGHT_TAKEOFF_CLIMB_RATE * Math.Min(1.0, (m_fCruiseAltitudeAGL - altAGL) / 10.0);

            case EEF_EFlightPhase.CRUISE:
            {
                // Hold cruise altitude. Compute correction proportional to altitude error.
                float wpGroundY = GetSurfaceHeightAt(waypoint[0], waypoint[2]);
                float targetAlt;
                if (isFinalWaypoint)
                    targetAlt = wpGroundY + m_fCruiseAltitudeAGL;
                else
                    targetAlt = GetSurfaceHeightAt(here[0], here[2]) + m_fCruiseAltitudeAGL;

                float altError = targetAlt - here[1];
                return Math.Clamp(altError * 0.5, -2.0, 2.0);
            }

            case EEF_EFlightPhase.APPROACH_BLEED:
            {
                // Create an exponential descent profile into the LZ.
                // At FLIGHT_APPROACH_RANGE the helicopter remains at cruise altitude.
                // As it closes in, altitude target decays rapidly toward a low approach altitude.
                float wpGroundY = GetSurfaceHeightAt(waypoint[0], waypoint[2]);
                float approachFloorAGL = Math.Max(FLIGHT_TOUCHDOWN_AGL, 8.0);
                float progress = Math.Clamp((FLIGHT_APPROACH_RANGE - wpHorizDist) / FLIGHT_APPROACH_RANGE, 0.0, 1.0);
                float expo = Math.Pow(2.0, -6.0 * progress);
                float slopeAltitude = wpGroundY + approachFloorAGL + expo * (m_fCruiseAltitudeAGL - approachFloorAGL);
                float altError = slopeAltitude - here[1];
                return Math.Clamp(altError * 0.5, -3.0, 2.0);
            }

            case EEF_EFlightPhase.APPROACH_DESCENT:
            {
                // Final descent from the low approach altitude into touchdown.
                // Bug 2 fix: use hover altitude as terminal target in HOVER_LANDING mode.
                float wpGroundY = GetSurfaceHeightAt(waypoint[0], waypoint[2]);
                float approachFloorAGL = 4.0;
                float factor = Math.Clamp(wpHorizDist / FLIGHT_DESCENT_RANGE, 0.0, 1.0);
                float terminalAGL;
                if (m_eLandingMode == EEF_EHelicopterControlLandingMode.HOVER_LANDING)
                    terminalAGL = m_fHoverAltitudeAGL;
                else
                    terminalAGL = FLIGHT_TOUCHDOWN_AGL;
                float targetAltAGL = terminalAGL + factor * (approachFloorAGL - terminalAGL);
                float targetAlt = wpGroundY + targetAltAGL;
                float altError = targetAlt - here[1];
                return Math.Clamp(altError * 0.7, -5.0, 1.0);
            }

            case EEF_EFlightPhase.APPROACH_FLARE:
                return 0;

            case EEF_EFlightPhase.LANDED_PHASE:
                return 0;

            case EEF_EFlightPhase.HOVER_HOLD:
            {
                // Hold at configured hover altitude above the final waypoint ground level.
                float wpGroundY = GetSurfaceHeightAt(waypoint[0], waypoint[2]);
                float targetAlt = wpGroundY + m_fHoverAltitudeAGL;
                float altError = targetAlt - here[1];
                return Math.Clamp(altError * 0.7, -2.0, 2.0);
            }
        }
        return 0;
    }

    //! Compute and apply attitude (pitch/roll/yaw) to match the helicopter's motion.
    //! Pitch responds to forward acceleration. Roll responds to lateral acceleration.
    //! Yaw aligns the nose with the desired horizontal direction.
    protected void ApplyAttitude(IEntity owner, Physics phys, vector velocity, vector accel, vector desiredHorizDir, float timeSlice)
    {
        // During landing shutdown, stop all attitude control to prevent unwanted heading changes.
        if (m_bLandingShutdown)
        {
            phys.SetAngularVelocity(vector.Zero);
            return;
        }

        vector mat[4];
        owner.GetWorldTransform(mat);
        vector heliRight = mat[0];
        vector heliUp = mat[1];
        vector heliForward = mat[2];

        // Horizontal forward direction of helicopter.
        vector heliFwdFlat = Vector(heliForward[0], 0, heliForward[2]);
        if (heliFwdFlat.LengthSq() < 0.0001)
        {
            phys.SetAngularVelocity(vector.Zero);
            return;
        }
        heliFwdFlat.Normalize();

        // Horizontal velocity direction (where we're going).
        vector horizVel = Vector(velocity[0], 0, velocity[2]);
        float horizSpeed = horizVel.Length();

        // --- Desired yaw: align nose with the waypoint direction when available. ---
        vector desiredFwdFlat = heliFwdFlat;
        if (desiredHorizDir.LengthSq() > 0.0001)
        {
            desiredFwdFlat = desiredHorizDir;
        }
        else if (horizSpeed > 1.0)
        {
            desiredFwdFlat = horizVel * (1.0 / horizSpeed);
        }

        // --- Desired pitch: tilt forward when accelerating forward, with speed-proportional cruise bias ---
        // Project acceleration onto helicopter forward axis.
        float forwardAccel = accel[0] * heliFwdFlat[0] + accel[2] * heliFwdFlat[2];
        // Baseline nose-down only during CRUISE. Applying it during approach phases would cancel
        // the deceleration-induced nose-up and also tilt the rotor forward (reducing vertical lift),
        // causing the altitude controller to over-correct upward and produce a too-high approach path.
        float basePitch = 0;
        if (m_ePhase == EEF_EFlightPhase.CRUISE)
        {
            float speedRatio = Math.Clamp(horizSpeed / m_fCruiseSpeed, 0.0, 1.0);
            float speedTaper = Math.Clamp(horizSpeed / 3.0, 0.0, 1.0);
            basePitch = -speedRatio * speedTaper * FLIGHT_CRUISE_NOSE_DOWN_RAD;
        }
        float desiredPitch = Math.Clamp(basePitch + (-forwardAccel * FLIGHT_PITCH_PER_ACCEL), -FLIGHT_MAX_PITCH_RAD, FLIGHT_MAX_PITCH_RAD);

        // --- Desired roll: bank into the turn ---
        // Lateral accel = component of accel perpendicular to forward, in horizontal plane.
        vector heliRightFlat = Vector(heliFwdFlat[2], 0, -heliFwdFlat[0]); // 90 degrees clockwise of forward (in horizontal plane)
        float lateralAccel = accel[0] * heliRightFlat[0] + accel[2] * heliRightFlat[2];
        float desiredRoll = Math.Clamp(-lateralAccel * FLIGHT_ROLL_PER_LATERAL, -FLIGHT_MAX_ROLL_RAD, FLIGHT_MAX_ROLL_RAD);
        // Negative because right-positive lateral accel = bank right = negative roll in most conventions.

        // --- Build desired up vector from desired pitch and roll, around desired yaw ---
        // Start from world up, apply roll around desired forward, then pitch around desired right.
        vector desiredRight = Vector(desiredFwdFlat[2], 0, -desiredFwdFlat[0]);
        vector desiredUp = vector.Up;
        desiredUp = EEF_RotateAroundAxis(desiredUp, desiredFwdFlat, desiredRoll);
        desiredUp = EEF_RotateAroundAxis(desiredUp, desiredRight, desiredPitch);
        desiredUp.Normalize();

        // --- Combine attitude corrections via angular velocity ---
        vector angVel = vector.Zero;
        // Yaw correction.
        angVel = angVel + EEF_ComputeAngularVelocity(heliFwdFlat, desiredFwdFlat, FLIGHT_ATTITUDE_TIME_CONSTANT);
        // Pitch + roll correction (combined into single up-vector tracking).
        angVel = angVel + EEF_ComputeAngularVelocity(heliUp, desiredUp, FLIGHT_ATTITUDE_TIME_CONSTANT);

        phys.SetAngularVelocity(angVel);
    }

    protected void OnFlightArrived(IEntity owner)
    {
        // Note: Full landing shutdown is now handled in TickFlightController's landing shutdown sequence.
        // This method is kept for potential future use (e.g., event callbacks).
        
        if (m_eLandingMode == EEF_EHelicopterControlLandingMode.HOVER_LANDING)
        {
            // Hover landing: maintain hover altitude on final waypoint indefinitely.
            m_eState = EEF_EHelicopterControlState.ARRIVING;
            DebugLog("Final waypoint reached. Hover landing active.");
        }
    }

    // --------------------------------------------------------
    // HELPERS
    // --------------------------------------------------------

    protected void BuildWaypoints(IEntity owner)
    {
        if (!m_aWaypoints)
            m_aWaypoints = new array<vector>();

        ClearWaypoints();
        if (!ResolveConfiguredWaypoints(owner))
        {
            AddDefaultForwardWaypoint(owner);
            DebugLog("No configured waypoints found; added default forward waypoint.");
        }
    }

    protected void AddDefaultForwardWaypoint(IEntity owner)
    {
        vector mat[4];
        owner.GetWorldTransform(mat);
        vector forward = Vector(mat[2][0], 0, mat[2][2]);
        if (forward.LengthSq() < 0.0001)
            forward = Vector(0, 0, 1);
        forward.Normalize();

        vector here = owner.GetOrigin();
        vector dest = here + forward * 150.0;
        float groundY = GetSurfaceHeightAt(dest[0], dest[2]);
        dest[1] = groundY + m_fCruiseAltitudeAGL;

        m_aWaypoints.Insert(dest);
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
