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
	DESPAWNED,
	DETAINED		//! Pulled over to the roadside by a player; the crew gets out. Out of the queue, not counted as traffic.
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
	int m_iRoute;									//! Index of the spawn marker / feeder route this vehicle spawned on
	bool m_bOnRails;								//! True once the script (not the AI) drives this vehicle - scripted rails mode, see EEF_CheckpointComponent.StartScriptedApproach
	bool m_bCleared;								//! True once a player (or the auto-clear timer) has cleared this vehicle to pass through the roadblock
	bool m_bDriverDown;								//! The driver was killed or knocked out: the script no longer drives this car
	bool m_bSurrendered;							//! The surviving passengers got out and sit beside the wreck
	float m_fSurrenderTime;							//! World time (s) the passengers were ordered out
	vector m_vSpawnPos;								//! Where the vehicle spawned - a car that never leaves is culled
	vector m_vProgressPos;							//! Where the car last made progress (no-progress cull)
	float m_fProgressTime = -1;						//! World time (s) of that position; -1 = not tracking
	bool m_bFleeRequested;						//! The detain order made this car bolt: it speeds off once it reaches the stop line
	bool m_bFleeing;								//! True once the car is actually running away
	bool m_bCrewSat;								//! A detained crew member has sat down on the ground: too late to turn hostile
	bool m_bHostilePending;							//! A hostile roll succeeded: the crew turn after a short random delay
	bool m_bHostile;								//! The crew turned hostile: the car is stopped where it is, the crew get out armed and attack
	Faction m_HostileFaction;						//! The side the crew switched to
	bool m_bDetainRequested;						//! True once a player ordered this vehicle detained: it pulls over as soon as it reaches the stop line
	int m_iDetainSlot;								//! Detain bay (0-based) this vehicle parks in, -1 = none
	bool m_bParked;									//! True once the detained vehicle has stopped in its bay
	vector m_vParkPos;								//! Where it parked - used to notice a player moving it
	vector m_vRoadForward;							//! Direction of travel of the road at the detain bay (horizontal, unit)
	float m_fVergeDist;								//! Metres from the parked car, to the road's left, to where the crew sit (just off the road)
	ref array<IEntity> m_aCrew = {};					//! Crew characters that got out of the car
	ref array<IEntity> m_aRecruited = {};			//! Crew a player recruited with the vanilla commanding menu (their AI is switched back on)
	bool m_bPlayerClaimed;							//! True once a player got into or moved the parked car: its bay is freed and the car is never deleted
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
		m_bHeld = false;
		m_bOnRails = false;
		m_bCleared = false;
		m_bDetainRequested = false;
		m_bFleeRequested = false;
		m_bFleeing = false;
		m_bDriverDown = false;
		m_bSurrendered = false;
		m_fSurrenderTime = 0;
		m_iDetainSlot = -1;
		m_bParked = false;
		m_bPlayerClaimed = false;
		m_iRoute = 0;
	}
}

// ============================================================
// One feeder route: the road line from a Spawn marker to the checkpoint, chained across road pieces.
// Used to measure a car's road distance to the stop line (the script-takeover point) and, for the first
// route, to lay out the queue gates. Plain data.
// ============================================================
class EEF_CheckpointRoute
{
	vector m_vSpawn;							//! The Spawn marker position
	ref array<vector> m_aLine = {};				//! Road line from the spawn toward (and a bit past) the checkpoint
	ref array<float> m_aCum = {};				//! Cumulative horizontal distance along m_aLine
	float m_fStopArc;							//! Arc length at which the checkpoint projects onto the line
	float m_fLaneOffset;						//! Metres to the right of the road centre line where cars drive (from the road width)
	bool m_bValid = true;						//! False if the spawn check found a problem: no cars spawn on this route (see ValidateRoute)
	string m_sProblem;							//! What the spawn check found wrong
}

// ============================================================
// One scripted departure drive (see EEF_CheckpointComponent.StartScriptedDeparture). Plain data, NOT a
// component: the car's road-following path plus the controller's running state and last outputs.
// ============================================================
class EEF_ScriptedDriveRun
{
	EEF_CheckpointVehicleState m_State;
	VehicleWheeledSimulation m_Sim;
	int m_iKind;								//! 0 = approach (stop at the queue gate), 1 = departure (drive on to the despawn point)
	ref array<float> m_aCornerSpeed = {};		//! Safe speed (m/s) at each path point from the path's curvature (transit / departure only)
	float m_fGearTimer;							//! Seconds until the next gear change is allowed
	float m_fClutch = 1.0;						//! Clutch engagement the script wants (slipped on a heavy launch so the engine does not stall)
	float m_fDownTimer;						//! Seconds until the next downshift is allowed
	float m_fSinceUpshift = 99;				//! Seconds since the last upshift
	float m_fHuntBlock;						//! Seconds upshifts are held back after a gear that proved too tall
	float m_fHillTimer;						//! Seconds spent pulling away on a slope (the hill-hold brake fades over the first second)
	float m_fSlope;							//! Sine of the car's pitch: positive uphill, negative downhill
	float m_fLastEngineTry = -10;				//! Run time of the last engine restart attempt
	float m_fBypassOffset;						//! Flee only: sideways shift (m, + = left) of the steering target to get round a vehicle blocking the road
	float m_fNextObstacleCheck;					//! Flee only: run time (s) of the next obstacle query
	bool m_bFlee;							//! Departure only: a car bolting from the checkpoint (launches fast, then the AI takes over at flee speed)
	float m_fStopArc;							//! Approach only: arc length along m_aPath at which to stop (the queue gate)
	ref array<vector> m_aPath = {};				//! Lane-offset path, index 0 = where the drive starts
	ref array<float> m_aCum = {};				//! Cumulative horizontal distance along m_aPath
	int m_iSeg;									//! Path segment the car is currently on
	float m_fProgress;							//! Arc length of the car's projection onto the path
	float m_fElapsed;							//! Seconds since the drive began
	float m_fStuckTime;							//! Seconds spent nearly stopped after the first few seconds
	bool m_bStalled;							//! The drive gave up because the car stayed nearly stopped with nothing ahead of it (not a timeout)
	float m_fNoProgressTime;					//! Seconds the car has wanted to move without gaining LAUNCH_PROGRESS_STEP along the path (rocking in place counts)
	float m_fProgressAnchor;					//! Path progress (m) the no-progress timer measures from
	bool m_bLaunchAssist;						//! The car needed help pulling away: it keeps getting a push along its heading when it slows to a crawl
	float m_fLastLog;							//! m_fElapsed at the last diagnostic line
	float m_fThrottle;							//! Last computed outputs, re-applied every frame
	float m_fBrake;
	float m_fSteering;

	// Debug visuals (m_bDebugVisuals): what the controller saw on its last step.
	IEntity m_GhostEntity;						//! Local ghost car shown at the ideal pose on the path
	vector m_vTarget;							//! Look-ahead point being steered toward
	vector m_vNearest;							//! The car's projection onto the path (where it "should be")
	float m_fSpeedKmh;
	float m_fTargetSpeed;
	float m_fAheadGap;
	float m_fOffLane;
	float m_fHeadingErrDeg;
	vector m_vHorizonPoint;						//! The far end of the bend look-ahead (how far ahead the car is checking)
	vector m_vLimitPoint;						//! The bend that currently limits the speed
	float m_fLimitSpeed;						//! Speed (km/h) that bend allows right now
	float m_fFwdSpeedKmh;						//! Signed speed along the car's heading (negative = rolling backwards)
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
// A single calculated queue gate: where one queued car stops. Computed by
// ComputeQueueGates() by walking the road line back from the stop line, shifted
// onto the traffic lane. Index 0 is the front (stop line); higher indices step
// back toward the Spawn marker. m_Forward is the direction of travel at m_Point.
// ============================================================
class EEF_CheckpointQueueGate
{
	vector m_Point;
	vector m_Forward;
}

//------------------------------------------------------------------------------------------------
[ComponentEditorProps(category: "EEF/Checkpoint", description: "Checkpoint traffic orchestrator. Use the EEF_Checkpoint prefab: it carries this component plus Spawn and Despawn markers and a roadblock as children. Traffic spawns at the Spawn marker(s), the script drives it up to a queue at the stop line (this entity), holds the front car, then releases it through the roadblock to the Despawn marker.")]
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

	// A wreck (hostile car or a car whose driver was put down) holds its queue slot until a player moves it
	// this far (m) from where it stopped, measured from the vehicle origin (the centre-of-mass orb); then the line moves up.
	protected const float WRECK_MOVED_DISTANCE = 4.5;

	// A queue approach that stalls (stays nearly stopped with nothing ahead) this many metres or more short of its
	// gate is removed with its crew instead of being parked "held" in the wrong place.
	protected const float STALLED_APPROACH_CULL_DISTANCE = 6.0;

	// A car that wants to move (clear road ahead) but has not gained LAUNCH_PROGRESS_STEP metres along its path for
	// this long (a heavy truck that cannot pull away on a slope, rocking back and forth) is helped: first full throttle
	// with no hill-hold brake, then a small push along its heading. After LAUNCH_GIVE_UP seconds it counts as stalled.
	protected const float LAUNCH_PROGRESS_STEP = 0.75;
	protected const float LAUNCH_BOOST_AFTER = 2.5;
	protected const float LAUNCH_ASSIST_AFTER = 5.0;
	protected const float LAUNCH_GIVE_UP = 40.0;
	protected const float LAUNCH_ASSIST_SPEED_MS = 2.5;

	// A hostile crew member still in a seat after the exit order and forced lift-out is lifted out this many more
	// times, one second apart; after that they are killed.
	protected const int HOSTILE_EXIT_RETRIES = 3;

	// How long (in one-second passes) a hostile crew member is watched after the exit order, in case they get back in.
	protected const int HOSTILE_WATCH_PASSES = 20;


	// --------------------------------------------------------
	// SETTINGS
	//
	// Traffic + Checkpoint are what a mission maker adjusts. Tuning is fine adjustment, Debug is for testing.
	// There is no trigger zone and no entity-name setup: the Spawn / Despawn points are marker entities
	// (children of the EEF_Checkpoint prefab) and the distance at which the script takes a car over is
	// derived from the queue length (see GetTakeoverDistance).
	// --------------------------------------------------------

	// Vehicle pool: fixed list of checkpoint-ready vehicles (each carries the Clear / Detain actions at the driver's
	// door). The mission maker only chooses which are in play; one enabled vehicle is picked at random per spawn.
	[Attribute("1", UIWidgets.CheckBox, "Skoda 105 (car).", category: "Vehicles")]
	protected bool m_bUseS105;

	[Attribute("1", UIWidgets.CheckBox, "Skoda 1203 (van).", category: "Vehicles")]
	protected bool m_bUseS1203;

	[Attribute("0", UIWidgets.CheckBox, "T601 (Rally DLC car). Off by default: it has a 19 kW engine and cannot climb steep roads, even driven by a player.", category: "Vehicles")]
	protected bool m_bUseT601;

	[Attribute("1", UIWidgets.CheckBox, "UAZ-452 (van).", category: "Vehicles")]
	protected bool m_bUseUAZ452;

	[Attribute("1", UIWidgets.CheckBox, "UAZ-469 (covered jeep).", category: "Vehicles")]
	protected bool m_bUseUAZ469;

	[Attribute("1", UIWidgets.CheckBox, "Ural-4320 transport truck.", category: "Vehicles")]
	protected bool m_bUseUralTransport;

	[Attribute("1", UIWidgets.CheckBox, "Ural-4320 tanker.", category: "Vehicles")]
	protected bool m_bUseUralTanker;

	[Attribute("1", UIWidgets.CheckBox, "V2101 (truck).", category: "Vehicles")]
	protected bool m_bUseV2101;

	// The occupants are fixed (not mission-maker settings): an empty CIV group is spawned and 1-4 random
	// civilians (as many as the vehicle has seats) are added to it. The first one drives, the rest ride.
	static const ResourceName OCCUPANT_GROUP_PREFAB = "{EEF0C0DE000A0002}Prefabs/Checkpoint/EEF_Group_Transport.et";
	static const int OCCUPANTS_MAX = 4;

	[Attribute("1", UIWidgets.CheckBox, "Wait for a player to spawn into the world before any traffic appears. Cars spawned while every player is still on a menu could not get their crew seated. Untick for a Game Master only session.", category: "Traffic")]
	protected bool m_bWaitForPlayers;

	[Attribute("12.0", UIWidgets.EditBox, "Minimum seconds between spawns. The real gap is random between min and max so traffic arrives at varying intervals.", category: "Traffic")]
	protected float m_fSpawnIntervalMin;

	[Attribute("20.0", UIWidgets.EditBox, "Maximum seconds between spawns.", category: "Traffic")]
	protected float m_fSpawnIntervalMax;

	protected float m_fSpawnClearRadius = 10.0;

	protected float m_fSpawnStuckSeconds = 45.0;

	[Attribute("4", UIWidgets.EditBox, "Maximum vehicles alive at once. This is also the QUEUE LENGTH: the queue has one slot per vehicle, so the road before the stop line must be at least (this - 1) x slot spacing long.", category: "Traffic")]
	protected int m_iMaxConcurrent;

	protected bool m_bAutoStart = true;

	[Attribute("18.0", UIWidgets.EditBox, "Metres between queue slots, walking back from the stop line along the road. Must clear the longest vehicle in the pool plus a gap (e.g. a 7.5 m truck + 5 m).", category: "Checkpoint")]
	protected float m_fQueueSlotSpacing;

	protected float m_fHoldSeconds = 0.0;

	protected float m_fScriptedDriveLength = 0.0;

	protected float m_fRoadSpeedKmh = 60.0;

	protected float m_fCornerGrip = 3.0;

	protected bool m_bScriptShiftGears = true;

	[Attribute("3.0", UIWidgets.EditBox, "Detain: metres a detained car drives straight on past the stop line before it turns left across the road. Make it long enough to clear the roadblock pieces.", category: "Checkpoint")]
	protected float m_fPullOverDistance;

	[Attribute("60.0", UIWidgets.EditBox, "Detain: angle (degrees from the road direction) at which a detained car turns left across the opposing lane to its bay.", category: "Checkpoint")]
	protected float m_fPullOverAngle;

	[Attribute("0.0", UIWidgets.EditBox, "Detain: a detained car parks with its centre in the middle of the opposing lane. This moves it further left (positive) or back toward its own lane (negative), in metres.", category: "Checkpoint")]
	protected float m_fPullOverOffset;

	[Attribute("3", UIWidgets.EditBox, "Detain: number of detain bays = how many cars can be detained at once. Each bay is further along the road than the last.", category: "Checkpoint")]
	protected int m_iMaxDetained;

	[Attribute("7.0", UIWidgets.EditBox, "Detain: metres along the road between detain bays. Must clear an angled car (about 4-5 m).", category: "Checkpoint")]
	protected float m_fDetainSlotSpacing;

	protected float m_fCrewSitDistance = 2.5;

	protected bool m_bValidateSpawns = true;

	protected float m_fSingleLaneMaxWidth = 5.0;

	protected float m_fParallelAngle = 25.0;

	[Attribute("14.0", UIWidgets.EditBox, "Single-lane roads: metres along the road between detain bays. Must be longer than the longest car in the pool plus the move out to the verge (a 7.5 m truck needs about 14).", category: "Road")]
	protected float m_fParallelBaySpacing;

	protected float m_fParallelMargin = 0.6;

	[Attribute("180.0", UIWidgets.EditBox, "Detain: seconds a detained car and its crew stay before they are removed. 0 = never (they pile up). A car a player has taken over is left alone; only the crew are removed.", category: "Checkpoint")]
	protected float m_fDetainedLifetime;

	protected float m_fCruiseSpeedKmh = 14.0;

	protected float m_fTakeoverMargin = 30.0;

	protected float m_fSteerGain = 1.6;

	//! No-progress cull: a driven car that moves less than 3 m in this many seconds (not queueing, held or detained) is removed with its crew.
	protected float m_fNoProgressSeconds = 30.0;

	[Attribute("300.0", UIWidgets.EditBox, "Failsafe: force-despawn a vehicle alive this many seconds without finishing its route (stuck, piled up). 0 = never.", category: "Tuning")]
	protected float m_fMaxVehicleLifetime;

	[Attribute("0.05", UIWidgets.EditBox, "Run away: chance [0..1] that a car ordered detained bolts instead of pulling over. If every detain bay is already taken, the car always runs.", category: "Run away")]
	protected float m_fFleeChance;

	[Attribute("80.0", UIWidgets.EditBox, "Run away: speed (km/h) a fleeing car drives at once the AI has taken over.", category: "Run away")]
	protected float m_fFleeSpeedKmh;

	protected float m_fFleeLaunchSpeedKmh = 25.0;

	protected float m_fFleeLaunchLength = 80.0;

	[Attribute("0.03", UIWidgets.EditBox, "Hostile: chance [0..1], rolled when a car first takes the rails and again each time it reaches a queue gate, that its crew turns hostile. The crew switch to the side opposing the nearest player, get a rifle or pistol, climb out and attack. The car stays where it stopped as a blockage.", category: "Hostile")]
	protected float m_fHostileChance;

	protected float m_fHostileDetainedFactor = 0.4;

	protected float m_fHostileDetainedMin = 0.01;

	protected float m_fHostileDelayMin = 0.0;

	protected float m_fHostileDelayMax = 6.0;

	protected int m_iHostileMagazines = 3;

	[Attribute("0.25", UIWidgets.EditBox, "Contraband: chance [0..1] a spawned vehicle carries contraband (loose weapons, ammo or explosives in its inventory). The roll only happens while the detain bays can take it: at least one bay free, and more free bays than contraband cars already on the road that have not been detained.", category: "Contraband")]
	protected float m_fContrabandChance;

	[Attribute("", UIWidgets.Object, "Contraband: item prefabs to pick from (one random pick per item). Leave EMPTY to use the built-in vanilla list of rifles, pistols, magazines, grenades, mines and C4.", category: "Contraband")]
	protected ref array<ref EEF_CheckpointPrefabEntry> m_aContrabandPrefabs;

	protected int m_iContrabandItemsMin = 1;

	protected int m_iContrabandItemsMax = 3;

	[Attribute("1", UIWidgets.CheckBox, "Print debug lines to the console. Turn off in final missions.", category: "Debug")]
	protected bool m_bDebugLog;

	[Attribute("1", UIWidgets.CheckBox, "Draw the road path from each Spawn marker to the Despawn marker once when the checkpoint starts, and remove it after a minute (Workbench / client only). Green = the marker passed the spawn check, red = it failed. Arrows show the direction of travel; a label shows where the road chain ends if it stops short of the Despawn marker.", category: "Debug")]
	protected bool m_bDebugSpawnPath;

	[Attribute("0", UIWidgets.CheckBox, "Draw the scripted drive while playing (Workbench / client only): lane path, where each car should be (green dot + ghost car), the look-ahead target (red), the stop point (orange), the queue gates (blue) and a status label. The editor preview (stop line, markers) is separate and only exists in the World Editor.", category: "Debug")]
	protected bool m_bDebugVisuals;

	// --------------------------------------------------------
	// FIXED VALUES (formerly mission-maker settings that never needed changing)
	// --------------------------------------------------------
	protected ResourceName m_sWaypointPrefab = "{750A8D1695BD6998}Prefabs/AI/Waypoints/AIWaypoint_Move.et";	//! Move waypoint for the AI lead-in
	protected float m_fWaypointCompletionRadius = 15.0;	//! Generous, or the AI overshoots then reverses to hit the exact point
	protected float m_fArrivalRadius = 8.0;				//! Arrived-at-a-route-point distance
	protected float m_fArrivalPollInterval = 1.0;		//! Seconds between arrival polls
	protected float m_fApproachSpeedKmh = 20.0;			//! AI lead-in speed cap (km/h) - low enough to brake in first gear after takeover

	//! Every active checkpoint on the server, so the Clear action can find the checkpoint a vehicle belongs to.
	protected static ref array<EEF_CheckpointComponent> s_aInstances = new array<EEF_CheckpointComponent>();
	protected ref array<IEntity> m_aStandingCrew = new array<IEntity>();	//! Detainees a player told to stand up (so a move does not sit them back down)
	protected ref SCR_AIAnimation_Sitting m_SitAnimation;	//! Vanilla sit-on-the-ground loiter animation for the detained crew (lazily created)
	protected ref array<EEF_CheckpointVehicleState> m_aPassedStopLine = new array<EEF_CheckpointVehicleState>();	//! Cleared cars that just drove past the stop line (processed after each physics step)

	// --------------------------------------------------------
	// Internal state
	// --------------------------------------------------------
	protected ref array<ref EEF_CheckpointVehicleState> m_aVehicles = new array<ref EEF_CheckpointVehicleState>();
	protected bool m_bActive = false;
	protected bool m_bTickersStarted = false;
	protected ref array<IEntity> m_aSpawnPoints = new array<IEntity>();	//! Spawn markers (children of this entity); index = route index
	protected IEntity m_DespawnPoint;
	protected int m_iNextSpawn = 0;										//! Round-robin cursor over the spawn markers
	protected ref array<ref EEF_CheckpointRoute> m_aRoutes = new array<ref EEF_CheckpointRoute>();
	protected bool m_bRoutesBuilt = false;
	protected float m_fLastLineRoadWidth = 8.0;							//! Widest road in the last road line built (sets the lane offset)

	//! Calculated, ordered queue gates (index 0 = front / checkpoint stop line). Lazily computed from
	//! the road network by ComputeQueueGates(). (#23 redesign)
	protected ref array<ref EEF_CheckpointQueueGate> m_aQueueGates = new array<ref EEF_CheckpointQueueGate>();
	protected bool m_bQueueGatesComputed = false;

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

		if (s_aInstances.Find(this) == -1)
			s_aInstances.Insert(this);

		// Defer until the world is fully initialised so marker entities and the
		// prefab pools can be resolved. Mirrors EEF_PatrolComponent's deferred start.
		GetGame().GetCallqueue().CallLater(DeferredInit, 1000, false);
	}

	override void OnDelete(IEntity owner)
	{
		s_aInstances.RemoveItem(this);
		super.OnDelete(owner);
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

		if (m_bWaitForPlayers && !HasPlayerInWorld())
		{
			DebugLog("No player has spawned into the world yet - not spawning traffic.");
			return;
		}

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
	//! True once at least one player controls a character (has spawned in, rather than sitting on a menu).
	protected bool HasPlayerInWorld()
	{
		PlayerManager players = GetGame().GetPlayerManager();
		if (!players)
			return false;

		array<int> ids = {};
		players.GetPlayers(ids);
		foreach (int playerId : ids)
		{
			if (players.GetPlayerControlledEntity(playerId))
				return true;
		}

		return false;
	}

	//------------------------------------------------------------------------------------------------
	//! True if any vehicle (ours or not) sits within the clearance radius of the spawn point.
	protected bool IsSpawnBlocked(vector spawnPos)
	{
		if (m_fSpawnClearRadius <= 0)
			return false;

		m_aObstacleHits.Clear();
		GetGame().GetWorld().QueryEntitiesBySphere(spawnPos, m_fSpawnClearRadius, QueryObstacleCallback);
		return !m_aObstacleHits.IsEmpty();
	}

	//------------------------------------------------------------------------------------------------
	//! Spawn one vehicle at the spawn point, spawn its occupant group, roll contraband, and start
	//! the seat poll. Members spawn staggered, so SeatPoll keeps seating them as they appear and
	//! defers the drive waypoint until the whole crew is aboard.
	protected void SpawnVehicle()
	{
		ResourceName vehiclePrefab = PickVehiclePrefab();
		if (vehiclePrefab.IsEmpty())
		{
			DebugLog("No vehicle enabled in the Vehicles settings. Skipping spawn.");
			return;
		}

		ResourceName occupantPrefab = OCCUPANT_GROUP_PREFAB;

		// Round-robin over the Spawn markers; the vehicle remembers its route (spawn index).
		// Only markers that passed the spawn check are used (see ValidateRoute).
		int routeIndex = PickSpawnRoute();
		if (routeIndex < 0)
		{
			DebugLog("No Spawn marker passed the spawn check - skipping this spawn.");
			return;
		}

		vector spawnPos = m_aSpawnPoints[routeIndex].GetOrigin();

		// Never spawn onto a car that has not driven off yet.
		if (IsSpawnBlocked(spawnPos))
		{
			DebugLog("Spawn point is occupied by a vehicle - skipping this spawn.");
			return;
		}

		m_iNextSpawn = routeIndex + 1;
		vector checkpointPos = GetOwner().GetOrigin();

		// Spawn the vehicle facing ALONG THE ROAD (a point a little way down the route), so it can drive straight
		// off without turning; fall back to facing the checkpoint if the route is unknown.
		vector faceTarget = checkpointPos;
		EEF_CheckpointRoute spawnRoute = GetRoute(routeIndex);
		if (spawnRoute && spawnRoute.m_aLine.Count() >= 2)
		{
			// The road's own direction of travel where the car stands (a short stretch, so a winding track
			// does not skew it), not a line to a point far down the road.
			faceTarget = spawnPos + GetRouteDirectionAt(spawnRoute, 5.0) * 50.0;
			DebugLog(string.Format("Spawn heading: along the road at marker %1.", routeIndex));
		}

		IEntity vehicle = SpawnPrefabFacing(vehiclePrefab, spawnPos, faceTarget);
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

		int occupants = FillOccupants(group, vehicle, spawnPos, checkpointPos);
		if (occupants == 0)
		{
			DebugLog("Could not create any occupants. Deleting vehicle and group.");
			SCR_EntityHelper.DeleteEntityAndChildren(group);
			SCR_EntityHelper.DeleteEntityAndChildren(vehicle);
			return;
		}

		bool hasContraband = false;
		if (m_fContrabandChance > 0 && CanRollContraband(vehicle))
			hasContraband = Math.RandomFloat(0, 1) < m_fContrabandChance;

		float now = GetWorldTimeSeconds();
		EEF_CheckpointVehicleState state = new EEF_CheckpointVehicleState(vehicle, group, now, hasContraband);
		state.m_iRoute = routeIndex;
		state.m_vSpawnPos = spawnPos;
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

		// A hostile crew has climbed out for good: never snap them back into their seats.
		if (state.m_bHostile)
			return;

		array<AIAgent> agents = {};
		state.m_OccupantGroup.GetAgents(agents);
		int total = agents.Count();

		// Teleport any member not yet in a seat. Runs on every poll - including after dispatch - so a
		// member that spawns late (the roster grows slowly and unpredictably) is snapped into a seat
		// the instant it appears instead of walking to (or chasing) the vehicle on foot.
		SeatAllAgents(state, agents);

		// Seating a character gives the car that character's faction (CIV); clear it again so a player of any
		// faction can search the trunk while the civilians are still in the car (see MakeVehicleNeutral).
		MakeVehicleNeutral(state.m_Vehicle);

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
				Print(string.Format("[EEF Checkpoint] No driver could be seated after %1 polls (%2 of %3 crew in seats, car at %4) - removing the car.", attempt + 1, seated, total, state.m_Vehicle.GetOrigin()), LogLevel.WARNING);
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

		if (state.m_bHasContraband)
			LoadContraband(state);

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
		// The script drives the car from here (spawn to despawn). Only if the route is unusable does the AI drive.
		// A freshly spawned car only drives properly (clutch, gear) once the AI has pulled it away, so the AI
		// launches it a few metres along its route and the script takes over as soon as it is rolling.
		if (AssignLaunchWaypoint(state))
		{
			ApplyCruiseSpeed(state, m_fApproachSpeedKmh);
			DebugLog("Vehicle dispatched - the AI launches it, then the script drives it along its route to the checkpoint zone.");
			GetGame().GetCallqueue().CallLater(LaunchTick, 500, false, state, 0);
			return;
		}

		AssignMoveWaypoint(state.m_OccupantGroup, m_DespawnPoint.GetOrigin(), m_fWaypointCompletionRadius);

		// Govern the approach speed so vehicles don't come in hot toward the queue.
		ApplyCruiseSpeed(state, m_fApproachSpeedKmh);

		DebugLog("Vehicle dispatched - no scripted route available, the AI drives it toward the checkpoint zone.");
	}

	//------------------------------------------------------------------------------------------------
	//! Gives the AI a short move order about 40 m along the car's route (the launch). False if there is no route.
	protected bool AssignLaunchWaypoint(EEF_CheckpointVehicleState state)
	{
		EEF_CheckpointRoute launchRoute = GetRoute(state.m_iRoute);
		if (!launchRoute || launchRoute.m_aLine.Count() < 2)
			return false;

		vector launchPos = state.m_Vehicle.GetOrigin();
		int launchSeg = FindNearestSegment(launchRoute.m_aLine, launchPos);
		float walked = 0;
		vector launchTarget = launchRoute.m_aLine[Math.Min(launchSeg + 1, launchRoute.m_aLine.Count() - 1)];
		vector prev = launchPos;
		for (int li = launchSeg + 1; li < launchRoute.m_aLine.Count(); li++)
		{
			launchTarget = launchRoute.m_aLine[li];
			walked += vector.Distance(prev, launchTarget);
			prev = launchTarget;
			if (walked >= 40.0)
				break;
		}

		AssignMoveWaypoint(state.m_OccupantGroup, launchTarget, 6);
		return true;
	}

	//------------------------------------------------------------------------------------------------
	//! Waits (every half second) until the AI has the new car rolling, then hands it to the script. If the AI
	//! has not moved the car after 8 s and again after 14 s, the launch is re-issued (AI re-activated, car
	//! released, new waypoint). Gives up after 22 s and starts the script anyway.
	protected void LaunchTick(EEF_CheckpointVehicleState state, int ticks)
	{
		if (!state || !state.m_Vehicle || state.m_eState != EEF_ECheckpointVehicleState.APPROACHING || state.m_bHeld || state.m_bHostile)
			return;

		// Forward speed only: a car rolling backwards down the hill has not been launched.
		float speedKmh = GetForwardSpeedKmh(state.m_Vehicle);
		CarControllerComponent controller = CarControllerComponent.Cast(state.m_Vehicle.FindComponent(CarControllerComponent));

		// What the AI is doing with the car (every second): is it in gear, is it asking for throttle?
		if (controller && controller.GetSimulation() && ticks % 2 == 0)
		{
			VehicleWheeledSimulation launchSim = controller.GetSimulation();
			DebugLog(string.Format("Launch[%1]: fwd=%2 km/h gear=%3 (controller now %4, next %5, automatic %6) clutch=%7 rpm=%8", ticks, speedKmh, launchSim.GetGear(), controller.GetCurrentGear(), controller.GetFutureGear(), controller.HasAutomaticGearbox(), launchSim.GetClutch(), launchSim.EngineGetRPM()));
			DebugLog(string.Format("Launch[%1]:   throttle=%2 brake=%3 handbrake=%4 engineOn=%5 groupAI waypoints set", ticks, launchSim.GetThrottle(), launchSim.GetBrake(), launchSim.IsHandbrakeOn(), launchSim.EngineIsOn()));
		}

		if (speedKmh < 1.0 && (ticks == 16 || ticks == 28))
		{
			DebugLog(string.Format("Launch: the AI has not moved the car after %1 s - re-issuing the launch.", ticks * 0.5));
			SCR_PhysicsHelper.ChangeSimulationState(state.m_Vehicle, SimulationState.SIMULATION);
			if (controller)
			{
				controller.SetPersistentHandBrake(false);
				controller.StartEngine();
			}
			SetGroupAIActive(state, false);
			SetGroupAIActive(state, true);
			AssignLaunchWaypoint(state);
		}

		if (speedKmh >= 8.0 || ticks >= 44)
		{
			if (StartScriptedTransit(state))
				DebugLog(string.Format("Rails: script takes over from the AI launch at %1 km/h.", speedKmh));
			else
				DebugLog("Rails: no scripted route after the launch - the AI keeps driving.");
			return;
		}

		GetGame().GetCallqueue().CallLater(LaunchTick, 500, false, state, ticks + 1);
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
	//! True if the driver seat holds a character who is dead or unconscious.
	protected bool IsDriverDown(IEntity vehicle)
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
			IEntity occupant = null;
			if (slot)
				occupant = slot.GetOccupant();
			if (!occupant)
				continue;

			CharacterControllerComponent controller = CharacterControllerComponent.Cast(occupant.FindComponent(CharacterControllerComponent));
			if (controller && controller.GetLifeState() != ECharacterLifeState.ALIVE)
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

		// Keep the queue packed: any empty slot ahead of a waiting vehicle is filled.
		MonitorQueueSlots();

		for (int i = m_aVehicles.Count() - 1; i >= 0; i--)
		{
			EEF_CheckpointVehicleState state = m_aVehicles[i];

			if (!state || !state.m_Vehicle)
			{
				m_aVehicles.Remove(i);
				continue;
			}

			// Driver killed or knocked out (held, AI-driven or fleeing): the passengers get out and surrender.
			if (!state.m_bDriverDown && state.m_bSeated && !state.m_bParked && IsDriverDown(state.m_Vehicle))
				HandleDriverDown(state);

			// Surrendered crew stay for the detained lifetime, then they and the car are removed.
			if (state.m_bSurrendered || (state.m_bHostile && state.m_fSurrenderTime > 0))
			{
				// The wreck blocks its queue slot until a player moves it off the road; then the line closes up.
				if (state.m_iQueueSlot >= 0 && vector.Distance(state.m_Vehicle.GetOrigin(), state.m_vParkPos) > WRECK_MOVED_DISTANCE)
				{
					DebugLog(string.Format("The wreck was moved - freeing queue slot %1, the line moves up.", state.m_iQueueSlot));
					state.m_iQueueSlot = -1;
					PromoteQueue();
				}

				if (m_fDetainedLifetime > 0 && now - state.m_fSurrenderTime > m_fDetainedLifetime)
				{
					DebugLog("Surrendered crew timed out - removing the car and its crew.");
					DespawnVehicle(i, true);
				}
				continue;
			}

			// Failsafe: cull vehicles that never complete their route (stuck / piled up).
			if (m_fMaxVehicleLifetime > 0 && state.m_eState != EEF_ECheckpointVehicleState.DETAINED && now - state.m_fSpawnTime > m_fMaxVehicleLifetime)
			{
				DebugLog("Vehicle exceeded max lifetime - force despawning.");
				DespawnVehicle(i, true);
				continue;
			}

			vector vehiclePos = state.m_Vehicle.GetOrigin();

			// Stuck at spawn: the car never left the spawn point (crew bailed out, wedged, damaged). Remove it
			// with its crew so the spawn point frees up.
			if (m_fSpawnStuckSeconds > 0 && (state.m_eState == EEF_ECheckpointVehicleState.APPROACHING || state.m_eState == EEF_ECheckpointVehicleState.SPAWNING) && now - state.m_fSpawnTime > m_fSpawnStuckSeconds
				&& vector.Distance(vehiclePos, state.m_vSpawnPos) < m_fSpawnClearRadius)
			{
				DebugLog("Vehicle never left the spawn point - removing it and its crew.");
				DespawnVehicle(i, true);
				continue;
			}

			// No progress: a scripted car that stalls on the road (a hill it cannot climb) blocks everything behind it.
			// Cars queueing near the checkpoint, held, detained or claimed by a player are legitimately still and are skipped.
			bool driven = state.m_bOnRails && !state.m_bHeld && !state.m_bDriverDown && !state.m_bPlayerClaimed
				&& (state.m_eState == EEF_ECheckpointVehicleState.APPROACHING || state.m_eState == EEF_ECheckpointVehicleState.DEPARTING);
			if (driven && state.m_eState == EEF_ECheckpointVehicleState.APPROACHING && IsInTakeoverRange(state, vehiclePos))
				driven = false;

			if (m_fNoProgressSeconds > 0 && driven)
			{
				if (state.m_fProgressTime < 0 || vector.Distance(vehiclePos, state.m_vProgressPos) > 3.0)
				{
					state.m_vProgressPos = vehiclePos;
					state.m_fProgressTime = now;
				}
				else if (now - state.m_fProgressTime > m_fNoProgressSeconds)
				{
					DebugLog("Vehicle made no progress for " + m_fNoProgressSeconds + " s - removing it and its crew.");
					DespawnVehicle(i, true);
					continue;
				}
			}
			else
				state.m_fProgressTime = -1;

			switch (state.m_eState)
			{
				case EEF_ECheckpointVehicleState.APPROACHING:
				{
					// Wait until the vehicle crosses into the trigger sphere, then slot it - but only the
					// vehicle nearest the checkpoint may take a freed slot. Otherwise a rear car could be
					// slotted first and then sit blocked behind a front car that is still waiting.
					if (IsInTakeoverRange(state, vehiclePos))
					{
						if (HasQueueWaiterAhead(state, vehiclePos))
						{
							if (!state.m_bHeld)
								HoldVehicle(state);
						}
						else
						{
							EnterQueue(state);
						}
					}
					break;
				}

				case EEF_ECheckpointVehicleState.QUEUED:
				case EEF_ECheckpointVehicleState.AT_FRONT:
				{
					// Scripted rails: the run itself stops the vehicle at its gate.
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

				case EEF_ECheckpointVehicleState.DETAINED:
				{
					// A player got into the parked car or moved it: free its bay and never delete the car.
					// Its bay stays reserved while the car is still near it, so the next detainee never
					// parks on top of it; the bay is freed once the car is well away.
					if (!state.m_bPlayerClaimed && IsDetainedCarDisturbed(state))
					{
						state.m_bPlayerClaimed = true;
						DebugLog("Detained vehicle was moved or entered by a player - it is left alone from now on.");
					}

					if (state.m_bPlayerClaimed && state.m_iDetainSlot >= 0 && vector.Distance(vehiclePos, state.m_vParkPos) > 10.0)
					{
						DebugLog(string.Format("Claimed vehicle left its bay - bay %1 is free again.", state.m_iDetainSlot));
						state.m_iDetainSlot = -1;
					}

					ActivateRecruitedCrew(state);

					// Parked at the roadside with its crew out: remove it after the configured time.
					if (m_fDetainedLifetime > 0 && now - state.m_fStateEnterTime > m_fDetainedLifetime)
					{
						if (state.m_bPlayerClaimed)
						{
							DebugLog("Detained crew timed out - removing the crew, leaving the car a player took.");
							state.m_Vehicle = null;
						}
						else
						{
							DebugLog("Detained vehicle timed out - removing it and its crew.");
						}
						DespawnVehicle(i, true);
					}
					break;
				}

				// HELD: parked at the front, waiting to be released - nothing to poll.
				// SPAWNING / DESPAWNED: handled elsewhere (seat poll / removal).
			}

		}
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

	//! True if another vehicle is waiting inside the zone for a queue slot and is closer to the checkpoint
	//! than this one (so it should be slotted first).
	protected bool HasQueueWaiterAhead(EEF_CheckpointVehicleState state, vector myPos)
	{
		float myDist = GetDistanceToStop(state, myPos);
		float takeover = GetTakeoverDistance();

		foreach (EEF_CheckpointVehicleState other : m_aVehicles)
		{
			if (!other || other == state || !other.m_Vehicle)
				continue;

			if (other.m_eState != EEF_ECheckpointVehicleState.APPROACHING || other.m_bDriverDown)
				continue;

			float otherDist = GetDistanceToStop(other, other.m_Vehicle.GetOrigin());
			if (otherDist > takeover)
				continue;

			if (otherDist < myDist)
				return true;
		}

		return false;
	}

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

		// Scripted rails: take the vehicle off the AI right here and drive it to its gate from script.
		if (!StartScriptedApproach(state))
		{
			// No usable road path yet: give the slot back, stay put and try again on the next tick.
			state.m_iQueueSlot = -1;
			HoldVehicle(state);

			// A car the script is already driving must not be left rolling without a run: carry on along its route.
			if (state.m_bHeld && state.m_bOnRails && !FindRunForState(state))
				StartScriptedTransit(state);
			return;
		}

		if (slot == 0)
		{
			SetState(state, EEF_ECheckpointVehicleState.AT_FRONT);
			DebugLog("Vehicle entered the zone into the FRONT slot - rails to the stop line.");
		}
		else
		{
			SetState(state, EEF_ECheckpointVehicleState.QUEUED);
			DebugLog(string.Format("Vehicle entered the zone into queue slot %1 - rails.", slot));
		}

		// First hostile roll: the script has just taken the car over.
		RollHostile(state);
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

	//! SLOT MONITOR (runs every arrival poll): for every vehicle waiting in the lane, if the slot directly
	//! ahead of it is empty, advance the queue so it moves up into that slot. PromoteQueue normally runs when
	//! the front vehicle is released; this also catches any other way a slot can become free (a vehicle
	//! despawned or aborted mid-queue, a missed event), so a gap can never persist.
	protected void MonitorQueueSlots()
	{
		foreach (EEF_CheckpointVehicleState state : m_aVehicles)
		{
			if (!state || state.m_iQueueSlot <= 0)
				continue;

			if (state.m_eState != EEF_ECheckpointVehicleState.QUEUED
				&& state.m_eState != EEF_ECheckpointVehicleState.AT_FRONT
				&& state.m_eState != EEF_ECheckpointVehicleState.HELD)
				continue;

			if (!IsSlotOccupied(state.m_iQueueSlot - 1))
			{
				DebugLog(string.Format("Slot monitor: slot %1 is empty - moving the queue up.", state.m_iQueueSlot - 1));
				PromoteQueue();
				return;
			}
		}
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

			// A wreck keeps its place in the line (it blocks the lane) but is never driven.
			if (state.m_bDriverDown)
				continue;

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

	//! Get a queued vehicle moving up after a promotion: a fresh scripted drive to its new (closer) gate.
	protected void ResumeTowardFront(EEF_CheckpointVehicleState state)
	{
		// A wreck with a dead driver goes nowhere.
		if (state.m_bDriverDown)
			return;

		// A cleared car never stops at the gates: it keeps rolling (a departure run follows the car ahead).
		if (state.m_bCleared)
		{
			if (!FindRunForState(state))
				StartScriptedDeparture(state);
			return;
		}

		// A fresh scripted drive to the vehicle's new (closer) gate.
		StartScriptedApproach(state);
	}

	//------------------------------------------------------------------------------------------------
	// CLEARANCE (player interaction)
	//
	// Every vehicle waiting in the queue carries a small interaction point at its driver's door
	// (EEF_ClearancePoint, replicated). A player uses "Clear vehicle" on it; the server then clears THAT vehicle.
	// A cleared vehicle never stops at a gate again: it keeps rolling behind the car ahead and, once the car
	// ahead is gone, drives straight through the roadblock. Clear all four and the whole queue flows.
	//------------------------------------------------------------------------------------------------

	//! Called by EEF_ClearVehicleAction on the server. True if a checkpoint accepted the vehicle.
	static bool ClearVehicleEntity(IEntity vehicle)
	{
		if (!vehicle)
			return false;

		foreach (EEF_CheckpointComponent checkpoint : s_aInstances)
		{
			if (checkpoint && checkpoint.ClearByVehicle(vehicle))
				return true;
		}
		return false;
	}

	protected bool ClearByVehicle(IEntity vehicle)
	{
		foreach (EEF_CheckpointVehicleState state : m_aVehicles)
		{
			if (state && state.m_Vehicle == vehicle)
			{
				ClearVehicle(state);
				return true;
			}
		}
		return false;
	}

	//! Clear one waiting vehicle to pass through the roadblock. Only vehicles that hold a queue slot qualify.
	void ClearVehicle(EEF_CheckpointVehicleState state)
	{
		if (!state || !state.m_Vehicle || state.m_bDriverDown || state.m_bCleared || state.m_bDetainRequested || state.m_bFleeRequested || state.m_iQueueSlot < 0)
			return;

		if (state.m_eState != EEF_ECheckpointVehicleState.QUEUED
			&& state.m_eState != EEF_ECheckpointVehicleState.AT_FRONT
			&& state.m_eState != EEF_ECheckpointVehicleState.HELD)
			return;

		state.m_bCleared = true;
		DebugLog(string.Format("Vehicle in slot %1 CLEARED.", state.m_iQueueSlot));

		// At the stop line: go now.
		if (state.m_eState == EEF_ECheckpointVehicleState.HELD)
		{
			ReleaseVehicle(state);
			return;
		}

		// Still moving up or waiting at a gate: switch to a departure run so it never stops at the gates ahead
		// (it still waits behind any car in front of it). If that cannot start now, it is retried on promotion.
		if (!StartScriptedDeparture(state))
			DebugLog("Cleared vehicle could not start its departure run yet - will retry when the queue advances.");
	}

	protected EEF_ScriptedDriveRun FindRunForState(EEF_CheckpointVehicleState state)
	{
		foreach (EEF_ScriptedDriveRun run : m_aScriptedRuns)
		{
			if (run && run.m_State == state)
				return run;
		}
		return null;
	}

	//! A cleared car that has driven past the stop line stops counting as queued: free its slot, move the
	//! queue up. Done after the physics step, never from inside the run loop.
	protected void ProcessStopLinePassed()
	{
		if (m_aPassedStopLine.IsEmpty())
			return;

		array<EEF_CheckpointVehicleState> passed = {};
		passed.Copy(m_aPassedStopLine);
		m_aPassedStopLine.Clear();

		foreach (EEF_CheckpointVehicleState state : passed)
		{
			if (!state || m_aVehicles.Find(state) == -1 || state.m_iQueueSlot < 0)
				continue;

			state.m_iQueueSlot = -1;
			state.m_bReleasePending = false;
			SetState(state, EEF_ECheckpointVehicleState.DEPARTING);
			DebugLog("Cleared vehicle passed the stop line - departing.");
			PromoteQueue();
		}
	}

	//------------------------------------------------------------------------------------------------
	// DETAIN (player interaction)
	//
	// "Detain vehicle" at the driver's door orders a queued car off the road instead of through the
	// roadblock. Like clearing it is tracked per vehicle: a car still rolling up to the stop line is
	// flagged and pulls over the moment it gets there. It drives straight on a short way, then turns LEFT
	// across the opposing lane at m_fPullOverAngle and parks angled on the far verge, handbrake on, engine
	// off. There are m_iMaxDetained parking bays (one per car, spaced m_fDetainSlotSpacing along the road).
	// The crew get out, move to the verge in front of their car and sit on the ground. The car leaves the
	// queue as soon as it starts to move, so the cars behind move up.
	//
	// If a player gets into a detained car or moves it, the car is "claimed": its bay is freed and it is no
	// longer deleted with its crew.
	//------------------------------------------------------------------------------------------------

	//! Called by EEF_DetainVehicleAction on the server. True if a checkpoint accepted the vehicle.
	static bool DetainVehicleEntity(IEntity vehicle)
	{
		if (!vehicle)
			return false;

		foreach (EEF_CheckpointComponent checkpoint : s_aInstances)
		{
			if (checkpoint && checkpoint.DetainByVehicle(vehicle))
				return true;
		}
		return false;
	}

	protected bool DetainByVehicle(IEntity vehicle)
	{
		foreach (EEF_CheckpointVehicleState state : m_aVehicles)
		{
			if (state && state.m_Vehicle == vehicle)
			{
				DetainVehicle(state);
				return true;
			}
		}
		return false;
	}

	//! Cars ordered detained or parked (not counting ones a player has taken over).
	protected int CountDetainedCars()
	{
		int count = 0;
		foreach (EEF_CheckpointVehicleState state : m_aVehicles)
		{
			if (state && state.m_bDetainRequested && !state.m_bPlayerClaimed)
				count++;
		}
		return count;
	}

	//------------------------------------------------------------------------------------------------
	// OCCUPANTS
	//------------------------------------------------------------------------------------------------

	//! Seats the crew can use: the driver seat plus the passenger seats (turret seats only if there are no cargo seats).
	protected int CountVehicleSeats(IEntity vehicle)
	{
		SCR_BaseCompartmentManagerComponent compMgr = SCR_BaseCompartmentManagerComponent.Cast(
			vehicle.FindComponent(SCR_BaseCompartmentManagerComponent)
		);
		if (!compMgr)
			return 0;

		int pilots = CountCompartments(compMgr, ECompartmentType.PILOT);
		int cargo = CountCompartments(compMgr, ECompartmentType.CARGO);
		if (cargo == 0)
			cargo = CountCompartments(compMgr, ECompartmentType.TURRET);

		return pilots + cargo;
	}

	//! The civilian characters the occupants are picked from. All carry the detainee interactions.
	//! Full GUID resource names: a path-only name loads with GUID {0000000000000000}, and in a multiplayer
	//! (replicated) session deleting an entity spawned that way crashed the game.
	protected void GetOccupantPrefabs(out array<ResourceName> prefabs)
	{
		prefabs = {
			"{EEF0C0DE000B0001}Prefabs/Checkpoint/EEF_Detainee_CIV_CottonShirt_1.et",
			"{EEF0C0DE000B0002}Prefabs/Checkpoint/EEF_Detainee_CIV_CottonShirt_2.et",
			"{EEF0C0DE000B0003}Prefabs/Checkpoint/EEF_Detainee_CIV_CottonShirt_3.et",
			"{EEF0C0DE000B0004}Prefabs/Checkpoint/EEF_Detainee_CIV_CottonShirt_4.et",
			"{EEF0C0DE000B0005}Prefabs/Checkpoint/EEF_Detainee_CIV_CottonShirt_5.et",
			"{EEF0C0DE000B0006}Prefabs/Checkpoint/EEF_Detainee_CIV_CottonShirt_6.et",
			"{EEF0C0DE000B0007}Prefabs/Checkpoint/EEF_Detainee_CIV_DenimJacket_1.et",
			"{EEF0C0DE000B0008}Prefabs/Checkpoint/EEF_Detainee_CIV_DenimJacket_2.et",
			"{EEF0C0DE000B0009}Prefabs/Checkpoint/EEF_Detainee_CIV_Turtleneck_1.et",
			"{EEF0C0DE000B0010}Prefabs/Checkpoint/EEF_Detainee_CIV_Turtleneck_2.et",
			"{EEF0C0DE000B0011}Prefabs/Checkpoint/EEF_Detainee_CIV_Turtleneck_3.et",
			"{EEF0C0DE000B0012}Prefabs/Checkpoint/EEF_Detainee_CIV_Businessman_1.et",
			"{EEF0C0DE000B0013}Prefabs/Checkpoint/EEF_Detainee_CIV_Businessman_2.et",
			"{EEF0C0DE000B0014}Prefabs/Checkpoint/EEF_Detainee_CIV_Businessman_3.et",
			"{EEF0C0DE000B0015}Prefabs/Checkpoint/EEF_Detainee_CIV_ConstructionWorker_1.et",
			"{EEF0C0DE000B0016}Prefabs/Checkpoint/EEF_Detainee_CIV_ConstructionWorker_2.et",
			"{EEF0C0DE000B0017}Prefabs/Checkpoint/EEF_Detainee_CIV_ConstructionWorker_3.et",
			"{EEF0C0DE000B0018}Prefabs/Checkpoint/EEF_Detainee_CIV_ConstructionWorker_4.et",
			"{EEF0C0DE000B0019}Prefabs/Checkpoint/EEF_Detainee_CIV_ConstructionWorker_5.et",
			"{EEF0C0DE000B0020}Prefabs/Checkpoint/EEF_Detainee_CIV_Dockworker_1.et",
			"{EEF0C0DE000B0021}Prefabs/Checkpoint/EEF_Detainee_CIV_Dockworker_2.et",
			"{EEF0C0DE000B0022}Prefabs/Checkpoint/EEF_Detainee_CIV_Dockworker_3.et",
			"{EEF0C0DE000B0023}Prefabs/Checkpoint/EEF_Detainee_CIV_Dockworker_4.et",
			"{EEF0C0DE000B0024}Prefabs/Checkpoint/EEF_Detainee_CIV_Dockworker_5.et",
			"{EEF0C0DE000B0025}Prefabs/Checkpoint/EEF_Detainee_CIV_Dockworker_6.et"
		};
	}

	//! Spawn 1 to OCCUPANTS_MAX random civilians (never more than the vehicle has seats) and add them to the
	//! group. Returns how many were created.
	protected int FillOccupants(SCR_AIGroup group, IEntity vehicle, vector spawnPos, vector checkpointPos)
	{
		int seats = CountVehicleSeats(vehicle);
		if (seats < 1)
		{
			DebugLog("Vehicle has no driver or passenger seats - cannot add occupants.");
			return 0;
		}

		array<ResourceName> prefabs;
		GetOccupantPrefabs(prefabs);

		int wanted = Math.RandomIntInclusive(1, Math.Min(OCCUPANTS_MAX, seats));
		int created = 0;

		for (int i = 0; i < wanted; i++)
		{
			IEntity character = SpawnPrefabFacing(prefabs.GetRandomElement(), spawnPos, checkpointPos);
			SCR_ChimeraCharacter chimera = SCR_ChimeraCharacter.Cast(character);
			if (!chimera)
			{
				if (character)
					SCR_EntityHelper.DeleteEntityAndChildren(character);
				continue;
			}

			// The vanilla group call: switches the character's AI on (a spawned character's AI is off, so it
			// would never drive) and adds it to the group.
			if (!group.AddAIEntityToGroup(character))
			{
				DebugLog("Occupant could not be added to the group (no AI agent) - removed.");
				SCR_EntityHelper.DeleteEntityAndChildren(character);
				continue;
			}

			created++;
		}

		DebugLog(string.Format("Occupants: %1 civilian(s) for %2 seat(s) (wanted %3).", created, seats, wanted));
		return created;
	}

	//------------------------------------------------------------------------------------------------
	// CONTRABAND
	//
	// A vehicle may carry contraband: loose weapons, ammo or explosives put into its own inventory so a
	// player can find and loot them. The roll is made at spawn, but only while the detain bays could take
	// it, so there are never more contraband cars heading for the checkpoint than free bays.
	//------------------------------------------------------------------------------------------------

	//! Contraband cars on the road that a player could still detain (not detained, cleared or leaving).
	protected int CountUndetainedContrabandCars()
	{
		int count = 0;
		foreach (EEF_CheckpointVehicleState state : m_aVehicles)
		{
			if (!state || !state.m_bHasContraband)
				continue;
			if (state.m_bDetainRequested || state.m_bCleared)
				continue;
			if (state.m_eState == EEF_ECheckpointVehicleState.DETAINED || state.m_eState == EEF_ECheckpointVehicleState.DEPARTING || state.m_eState == EEF_ECheckpointVehicleState.DESPAWNED)
				continue;
			count++;
		}
		return count;
	}

	//! True if this new vehicle may be rolled for contraband: it has an inventory to hold the cargo, at
	//! least one detain bay is free, and the contraband cars already on the road would not fill every free bay.
	protected bool CanRollContraband(IEntity vehicle)
	{
		if (!vehicle || !vehicle.FindComponent(SCR_UniversalInventoryStorageComponent))
		{
			DebugLog("Contraband skipped: this vehicle has no inventory storage.");
			return false;
		}

		int freeBays = m_iMaxDetained - CountDetainedCars();
		if (freeBays < 1)
		{
			DebugLog("Contraband skipped: no detain bay is free.");
			return false;
		}

		int pending = CountUndetainedContrabandCars();
		if (pending >= freeBays)
		{
			DebugLog(string.Format("Contraband skipped: %1 contraband car(s) already on the road for %2 free bay(s).", pending, freeBays));
			return false;
		}

		return true;
	}

	//! The prefabs contraband is picked from: the mission maker's list, or the built-in vanilla list when it is empty.
	protected void GetContrabandPrefabs(out array<ResourceName> prefabs)
	{
		prefabs = {};

		if (m_aContrabandPrefabs)
		{
			foreach (EEF_CheckpointPrefabEntry entry : m_aContrabandPrefabs)
			{
				if (entry && !entry.m_sPrefab.IsEmpty())
					prefabs.Insert(entry.m_sPrefab);
			}
		}

		if (!prefabs.IsEmpty())
			return;

		// Full GUID resource names (see GetOccupantPrefabs for why a path-only name is not safe).
		// Rifles and pistols
		prefabs.Insert("{FA5C25BF66A53DCF}Prefabs/Weapons/Rifles/AK74/Rifle_AK74.et");
		prefabs.Insert("{BFEA719491610A45}Prefabs/Weapons/Rifles/AKS74U/Rifle_AKS74U.et");
		prefabs.Insert("{3E413771E1834D2F}Prefabs/Weapons/Rifles/M16/Rifle_M16A2.et");
		prefabs.Insert("{1353C6EAD1DCFE43}Prefabs/Weapons/Handguns/M9/Handgun_M9.et");
		prefabs.Insert("{C0F7DD85A86B2900}Prefabs/Weapons/Handguns/PM/Handgun_PM.et");
		// Magazines
		prefabs.Insert("{BBB50A815A2F916B}Prefabs/Weapons/Magazines/Magazine_545x39_AK_30rnd_Ball.et");
		prefabs.Insert("{2EBF60EF24B108FC}Prefabs/Weapons/Magazines/Magazine_556x45_STANAG_30rnd_M855_Ball.et");
		prefabs.Insert("{9C05543A503DB80E}Prefabs/Weapons/Magazines/Magazine_9x19_M9_15rnd_Ball.et");
		prefabs.Insert("{8B853CDD11BA916E}Prefabs/Weapons/Magazines/Magazine_9x18_PM_8rnd_Ball.et");
		// Explosives: grenades, mines and C4
		prefabs.Insert("{E8F00BF730225B00}Prefabs/Weapons/Grenades/Grenade_M67.et");
		prefabs.Insert("{645C73791ECA1698}Prefabs/Weapons/Grenades/Grenade_RGD5.et");
		prefabs.Insert("{E4C9F0A4090CFE4D}Prefabs/Weapons/Explosives/Mine_M14/Mine_M14.et");
		prefabs.Insert("{B05A816C0BF50802}Prefabs/Weapons/Explosives/Mine_PMN4/Mine_PMN4.et");
		prefabs.Insert("{33CBDE73AB48172A}Prefabs/Weapons/Explosives/DemoBlock_M112/DemoBlock_M112.et");
	}

	//! Put 1-3 random items into the vehicle's inventory, through a crew member's inventory manager (the
	//! vehicle has none of its own). If nothing fits, the car is not contraband after all.
	protected void LoadContraband(EEF_CheckpointVehicleState state)
	{
		SCR_UniversalInventoryStorageComponent storage;
		if (state.m_Vehicle)
			storage = SCR_UniversalInventoryStorageComponent.Cast(state.m_Vehicle.FindComponent(SCR_UniversalInventoryStorageComponent));

		SCR_InventoryStorageManagerComponent manager;
		if (state.m_OccupantGroup)
		{
			array<AIAgent> agents = {};
			state.m_OccupantGroup.GetAgents(agents);
			foreach (AIAgent agent : agents)
			{
				IEntity member = agent.GetControlledEntity();
				if (member)
					manager = SCR_InventoryStorageManagerComponent.Cast(member.FindComponent(SCR_InventoryStorageManagerComponent));
				if (manager)
					break;
			}
		}

		array<ResourceName> prefabs;
		GetContrabandPrefabs(prefabs);

		if (!storage || !manager || prefabs.IsEmpty())
		{
			state.m_bHasContraband = false;
			DebugLog("Contraband cancelled: no storage, no crew inventory manager or no item list.");
			return;
		}

		int minItems = Math.Max(m_iContrabandItemsMin, 1);
		int maxItems = Math.Max(m_iContrabandItemsMax, minItems);
		int wanted = Math.RandomIntInclusive(minItems, maxItems);
		int loaded = 0;

		for (int i = 0; i < wanted; i++)
		{
			ResourceName itemPrefab = prefabs.GetRandomElement();
			Resource resource = Resource.Load(itemPrefab);
			if (!resource || !resource.IsValid())
			{
				DebugLog(string.Format("Contraband item prefab could not be loaded: %1", itemPrefab));
				continue;
			}

			// Spawn the item ourselves, then insert it, so a failure is seen here (and the item deleted)
			// instead of being silently dropped by an asynchronous spawn-into-storage request.
			EntitySpawnParams itemParams = new EntitySpawnParams();
			itemParams.TransformMode = ETransformMode.WORLD;
			state.m_Vehicle.GetWorldTransform(itemParams.Transform);
			IEntity item = GetGame().SpawnEntityPrefab(resource, GetGame().GetWorld(), itemParams);
			if (!item)
				continue;

			if (manager.TryInsertItemInStorage(item, storage))
			{
				loaded++;
				DebugLog(string.Format("Contraband insert accepted: %1, in storage now: %2", itemPrefab, storage.Contains(item)));
				GetGame().GetCallqueue().CallLater(CheckContrabandItem, 1500, false, item, storage, itemPrefab);
			}
			else
			{
				DebugLog(string.Format("Contraband item did not fit: %1", itemPrefab));
				SCR_EntityHelper.DeleteEntityAndChildren(item);
			}
		}

		if (loaded == 0)
		{
			state.m_bHasContraband = false;
			DebugLog("Contraband cancelled: none of the items fitted in the vehicle.");
			return;
		}

		DebugLog(string.Format("Contraband loaded: %1 of %2 item(s) in the vehicle.", loaded, wanted));
	}

	//! Diagnostic: is a contraband item still inside the vehicle storage a moment after it was inserted?
	protected void CheckContrabandItem(IEntity item, SCR_UniversalInventoryStorageComponent storage, ResourceName itemPrefab)
	{
		if (!item || !storage)
		{
			DebugLog(string.Format("Contraband check: %1 - the item or the storage no longer exists.", itemPrefab));
			return;
		}

		array<IEntity> items = {};
		storage.GetAll(items);
		DebugLog(string.Format("Contraband check: %1 in storage: %2 (storage holds %3 item(s)).", itemPrefab, storage.Contains(item), items.Count()));

		// List every storage on the vehicle and what each holds, to see which one the trunk action opens.
		IEntity vehicle = storage.GetOwner();
		array<Managed> allStorages = {};
		vehicle.FindComponents(BaseInventoryStorageComponent, allStorages);
		DebugLog(string.Format("Contraband check: vehicle has %1 inventory storage component(s).", allStorages.Count()));
		foreach (Managed managed : allStorages)
		{
			BaseInventoryStorageComponent other = BaseInventoryStorageComponent.Cast(managed);
			if (!other)
				continue;

			array<IEntity> otherItems = {};
			other.GetAll(otherItems);
			string names = "";
			foreach (IEntity otherItem : otherItems)
			{
				if (otherItem)
					names = names + " " + otherItem.GetPrefabData().GetPrefabName();
			}
			DebugLog(string.Format("  storage %1 (ours: %2): %3 slot(s), %4 item(s):%5", other.Type().ToString(), other == storage, other.GetSlotsCount(), otherItems.Count(), names));
		}
	}

	//! Lowest parking bay no detained car is using, or -1.
	protected int FindFreeDetainSlot()
	{
		for (int slot = 0; slot < m_iMaxDetained; slot++)
		{
			bool taken = false;
			foreach (EEF_CheckpointVehicleState other : m_aVehicles)
			{
				if (other && other.m_iDetainSlot == slot)
				{
					taken = true;
					break;
				}
			}

			if (!taken)
				return slot;
		}
		return -1;
	}

	//! Order one waiting vehicle detained. Only vehicles that hold a queue slot qualify.
	void DetainVehicle(EEF_CheckpointVehicleState state)
	{
		if (!state || !state.m_Vehicle || state.m_bDriverDown || state.m_bCleared || state.m_bDetainRequested || state.m_bFleeRequested || state.m_iQueueSlot < 0)
			return;

		if (state.m_eState != EEF_ECheckpointVehicleState.QUEUED
			&& state.m_eState != EEF_ECheckpointVehicleState.AT_FRONT
			&& state.m_eState != EEF_ECheckpointVehicleState.HELD)
			return;

		// Run away: a low random chance, and certain if no detain bay is free. The car bolts when it
		// reaches the stop line (at once if it is already there). It never takes a bay.
		bool bays = CountDetainedCars() < m_iMaxDetained;
		if (!bays || (m_fFleeChance > 0 && Math.RandomFloat(0, 1) < m_fFleeChance))
		{
			state.m_bFleeRequested = true;
			if (bays)
				DebugLog(string.Format("Vehicle in slot %1: detain order - the driver will RUN (roll < %2).", state.m_iQueueSlot, m_fFleeChance));
			else
				DebugLog(string.Format("Vehicle in slot %1: detain order but all %2 detain bays are in use - the driver will RUN.", state.m_iQueueSlot, m_iMaxDetained));

			if (state.m_eState == EEF_ECheckpointVehicleState.HELD)
				StartFlee(state);
			return;
		}

		state.m_bDetainRequested = true;
		DebugLog(string.Format("Vehicle in slot %1 ordered DETAINED.", state.m_iQueueSlot));

		// The crew may still turn hostile (after the usual short delay), even though they were just ordered over.
		RollHostile(state, false, true);

		// At the stop line: pull over now. Otherwise it pulls over when it gets there (see BeginHold).
		if (state.m_eState == EEF_ECheckpointVehicleState.HELD)
			BeginPullOver(state);
	}

	//! The front vehicle leaves the queue to pull over into a free bay. False (and the order is dropped) if
	//! every bay is taken.
	protected bool BeginPullOver(EEF_CheckpointVehicleState state)
	{
		if (!state || !state.m_Vehicle)
			return false;

		int bay = FindFreeDetainSlot();
		if (bay < 0)
		{
			// Bays filled up in the meantime: the driver runs.
			state.m_bDetainRequested = false;
			state.m_bFleeRequested = true;
			DebugLog("No free detain bay - the driver RUNS.");
			StartFlee(state);
			return true;
		}

		state.m_iDetainSlot = bay;
		state.m_iQueueSlot = -1;
		state.m_bReleasePending = false;
		SetState(state, EEF_ECheckpointVehicleState.DETAINED);

		if (!StartScriptedPullOver(state))
		{
			DebugLog("Rails: no pull-over path yet - holding and retrying.");
			GetGame().GetCallqueue().CallLater(RetryPullOver, 1000, false, state, 1);
		}

		// Everyone behind moves up now that the front slot is free.
		PromoteQueue();
		return true;
	}

	//! The driver bolts: leave the queue and drive out through the roadblock gap on the same departure
	//! path as a cleared car, but fast. The script launches the car, then the AI takes over at flee speed.
	protected void StartFlee(EEF_CheckpointVehicleState state)
	{
		if (!state || !state.m_Vehicle || state.m_bFleeing)
			return;

		state.m_bFleeing = true;
		DebugLog("The driver is running away!");
		ReleaseVehicle(state);
	}

	protected void RetryPullOver(EEF_CheckpointVehicleState state, int attempt)
	{
		if (!state || !state.m_Vehicle || state.m_bHostile || m_aVehicles.Find(state) == -1)
			return;

		// Already moving over (or no longer detained): nothing to do.
		if (state.m_eState != EEF_ECheckpointVehicleState.DETAINED || FindRunForState(state))
			return;

		if (StartScriptedPullOver(state))
			return;

		if (attempt >= 5)
		{
			DebugLog("Rails: still no pull-over path after 5 tries - detaining the vehicle where it stands.");
			FinishPullOver(state);
			return;
		}

		GetGame().GetCallqueue().CallLater(RetryPullOver, 1000, false, state, attempt + 1);
	}

	//! Reshape a lane path into the pull-over: straight for `lead` metres, then veering LEFT at angleDeg to
	//! the road direction (the path keeps going at that angle). Returns the arc length (on the reshaped
	//! path) at which the car has moved leftDistance metres sideways - where it stops.
	//! parallel = single-lane parking: instead of a straight angled line the car eases out along a smooth S-curve and
	//! ends up level with the road, leftDistance metres to the left of the lane, where it stops a little way along.
	protected float ShapePullOverPath(EEF_ScriptedDriveRun run, float lead, float leftDistance, float angleDeg, bool parallel = false)
	{
		int count = run.m_aPath.Count();
		if (count < 2)
			return 0;

		float tanA = Math.Max(Math.Tan(angleDeg * Math.DEG2RAD), 0.2);

		// Length of the S-curve: its steepest slope is 1.5 x leftDistance / length, which is made to match the angle.
		float curveLength = 1.5 * leftDistance / tanA;

		array<float> baseArc = {};
		baseArc.Copy(run.m_aCum);

		array<vector> shifted = {};
		bool recordedForward = false;
		for (int i = 0; i < count; i++)
		{
			vector dir;
			if (i < count - 1)
				dir = run.m_aPath[i + 1] - run.m_aPath[i];
			else
				dir = run.m_aPath[i] - run.m_aPath[i - 1];
			dir[1] = 0;
			if (dir.LengthSq() < 0.0001)
				dir = Vector(0, 0, 1);
			dir.Normalize();

			// The road direction where the car turns off: used later to put the crew beside the road.
			if (!recordedForward && baseArc[i] >= lead)
			{
				run.m_State.m_vRoadForward = dir;
				recordedForward = true;
			}

			// Left-hand normal (the opposite of the right-hand one the lane offset uses).
			vector left = Vector(-dir[2], 0, dir[0]);
			float lateral = Math.Max(0, baseArc[i] - lead) * tanA;
			if (parallel)
			{
				float t = Math.Clamp((baseArc[i] - lead) / curveLength, 0, 1);
				lateral = leftDistance * t * t * (3.0 - 2.0 * t);
			}
			shifted.Insert(run.m_aPath[i] + left * lateral);
		}

		run.m_aPath.Copy(shifted);

		run.m_aCum.Clear();
		run.m_aCum.Insert(0.0);
		for (int i = 1; i < run.m_aPath.Count(); i++)
		{
			float dx = run.m_aPath[i][0] - run.m_aPath[i - 1][0];
			float dz = run.m_aPath[i][2] - run.m_aPath[i - 1][2];
			run.m_aCum.Insert(run.m_aCum[i - 1] + Math.Sqrt(dx * dx + dz * dz));
		}

		// Where on the original path has the car moved leftDistance sideways?
		float targetBase = lead + leftDistance / tanA;
		if (parallel)
			targetBase = lead + curveLength + 2.5;	// a little way into the straight run, so the car is level when it stops
		for (int i = 1; i < count; i++)
		{
			if (baseArc[i] >= targetBase)
			{
				float span = baseArc[i] - baseArc[i - 1];
				float frac = 0;
				if (span > 0.001)
					frac = (targetBase - baseArc[i - 1]) / span;
				return run.m_aCum[i - 1] + (run.m_aCum[i] - run.m_aCum[i - 1]) * frac;
			}
		}

		return run.m_aCum[count - 1] - 1.0;
	}

	//! Drive the detained car on and across to its bay. False if anything needed is missing (retried).
	protected bool StartScriptedPullOver(EEF_CheckpointVehicleState state)
	{
		if (!state || !state.m_Vehicle || !state.m_bHeld || !m_DespawnPoint)
			return false;

		CarControllerComponent controller = CarControllerComponent.Cast(state.m_Vehicle.FindComponent(CarControllerComponent));
		if (!controller)
			return false;

		VehicleWheeledSimulation sim = controller.GetSimulation();
		if (!sim)
			return false;

		RemoveRunForState(state);

		EEF_ScriptedDriveRun run = new EEF_ScriptedDriveRun();
		run.m_State = state;
		run.m_Sim = sim;
		run.m_iKind = SCRIPTED_KIND_PULLOVER;

		// Each bay is further along the road, so later cars drive past the ones already parked.
		// Single-lane road: park parallel on the left verge, with longer bays; otherwise the angled bays across the
		// opposing lane.
		bool parallel = IsSingleLaneRoad();
		float baySpacing = m_fDetainSlotSpacing;
		if (parallel)
			baySpacing = m_fParallelBaySpacing;

		float lead = Math.Max(m_fPullOverDistance, 0) + Math.Max(state.m_iDetainSlot, 0) * baySpacing;

		float parallelCurve = 0;
		float parallelSide = 0;
		float parallelHalfWidth = 0;
		if (parallel)
		{
			vector bmin, bmax;
			state.m_Vehicle.GetBounds(bmin, bmax);
			parallelHalfWidth = (bmax[0] - bmin[0]) * 0.5;
			parallelSide = GetCheckpointRoadWidth() * 0.5 + parallelHalfWidth + m_fParallelMargin + m_fPullOverOffset;
			parallelCurve = 1.5 * parallelSide / Math.Max(Math.Tan(m_fParallelAngle * Math.DEG2RAD), 0.2);
		}

		// The path must run past the end of the move plus a stretch to settle in.
		if (!BuildLanePath(state.m_Vehicle, m_DespawnPoint.GetOrigin(), lead + parallelCurve + 40.0, run))
		{
			DebugLog("Rails: could not build a pull-over path.");
			return false;
		}

		float leftDistance;
		if (parallel)
		{
			// Traffic drives the middle of the road: ease out to just beyond the left edge, then straighten up.
			leftDistance = parallelSide;
			run.m_fStopArc = ShapePullOverPath(run, lead, leftDistance, m_fParallelAngle, true);

			// The crew sit on the far side of the car from the road, off the road.
			state.m_fVergeDist = parallelHalfWidth + m_fCrewSitDistance;
		}
		else
		{
			// From the traffic lane (right of centre) across to the middle of the opposing lane: the lanes sit
			// GetLaneOffset() either side of the road centre line.
			leftDistance = GetLaneOffset() * 2.0 + m_fPullOverOffset;
			run.m_fStopArc = ShapePullOverPath(run, lead, leftDistance, m_fPullOverAngle);

			// The car ends in the middle of the opposing lane; the crew sit this far further left, just off the road.
			float laneToEdge = Math.Max(m_fLastLineRoadWidth * 0.5 - GetLaneOffset() - m_fPullOverOffset, 0);
			state.m_fVergeDist = laneToEdge + m_fCrewSitDistance;
		}

		state.m_bOnRails = true;

		SCR_PhysicsHelper.ChangeSimulationState(state.m_Vehicle, SimulationState.SIMULATION);
		controller.SetPersistentHandBrake(false);
		if (!sim.EngineIsOn())
		{
			sim.EngineStart();
			controller.StartEngine();
		}

		m_aScriptedRuns.Insert(run);
		UpdateEventMask();

		SpawnDebugGhost(run);

		DebugLog(string.Format("Rails: PULL OVER bay %1 - straight %2 m, then %3 deg left to %4 m across (stop at %5 m).", state.m_iDetainSlot, lead, m_fPullOverAngle, leftDistance, run.m_fStopArc));
		return true;
	}

	//! The detained car has stopped in its bay: park it, engine off, and order the crew out.
	protected void FinishPullOver(EEF_CheckpointVehicleState state)
	{
		if (!state || !state.m_Vehicle)
			return;

		state.m_bOnRails = false;
		state.m_vParkPos = state.m_Vehicle.GetOrigin();
		state.m_bParked = true;

		CarControllerComponent controller = CarControllerComponent.Cast(state.m_Vehicle.FindComponent(CarControllerComponent));
		if (controller)
		{
			controller.SetPersistentHandBrake(true);
			controller.StopEngine(false);
		}

		DebugLog("Detained vehicle parked in its bay - ordering the crew out.");

		// One last check, with no delay: the crew turn hostile the moment the car stops, instead of getting out.
		if (RollHostile(state, true, true))
			return;

		ExitCrew(state);
	}

	//! The driver is dead or out cold. The script stops driving, the car rolls to a halt, then the surviving
	//! passengers get out and sit down beside it (surrender). The car takes no detain bay.
	protected void HandleDriverDown(EEF_CheckpointVehicleState state)
	{
		if (!state || !state.m_Vehicle || state.m_bDriverDown)
			return;

		state.m_bDriverDown = true;	// also marks a hostile car: from here on it is a blockage that is never driven again
		DebugLog("Driver is down - the drive stops, the car coasts to a halt and the passengers will surrender.");

		// Take the car off the rails and out of any AI hold; it is a free-rolling body now.
		RemoveRunForState(state);
		state.m_bOnRails = false;
		SetGroupAIActive(state, false);
		if (state.m_bHeld)
			ResumeVehicle(state);

		CarControllerComponent controller = CarControllerComponent.Cast(state.m_Vehicle.FindComponent(CarControllerComponent));
		if (controller)
		{
			VehicleWheeledSimulation sim = controller.GetSimulation();
			if (sim)
			{
				sim.SetThrottle(0);
				sim.SetBreak(0, false);
				sim.SetSteering(0);
			}
		}

		WaitWreckStopped(state, 0);
	}

	//! Wait (up to ~15 s) for the car to stop rolling, park it, then order the passengers out.
	protected void WaitWreckStopped(EEF_CheckpointVehicleState state, int attempt)
	{
		if (!state || !state.m_Vehicle || m_aVehicles.Find(state) == -1)
			return;

		float speedKmh = 0;
		Physics phys = state.m_Vehicle.GetPhysics();
		if (phys)
			speedKmh = phys.GetVelocity().Length() * 3.6;

		if (speedKmh > 1.0 && attempt < 30)
		{
			GetGame().GetCallqueue().CallLater(WaitWreckStopped, 500, false, state, attempt + 1);
			return;
		}

		CarControllerComponent controller = CarControllerComponent.Cast(state.m_Vehicle.FindComponent(CarControllerComponent));
		if (controller)
		{
			controller.SetPersistentHandBrake(true);
			controller.StopEngine(false);
		}

		// Sit beside the car: to the car's left, level with it.
		vector forward = state.m_Vehicle.GetTransformAxis(2);
		forward[1] = 0;
		if (forward.LengthSq() < 0.01)
			forward = Vector(0, 0, 1);
		forward.Normalize();
		state.m_vRoadForward = forward;
		state.m_fVergeDist = 4.0;

		state.m_fSurrenderTime = GetWorldTimeSeconds();
		state.m_vParkPos = state.m_Vehicle.GetOrigin();

		state.m_bSurrendered = true;
		DebugLog("Wrecked car stopped - ordering the surviving passengers out to surrender.");
		ExitCrew(state);
	}

	//------------------------------------------------------------------------------------------------
	// HOSTILE CREW
	//
	// A small chance, rolled when a car takes the rails and at every gate, that the crew turns on the
	// players: they change to the side opposing the nearest player, get a weapon, climb out and attack.
	// The car itself is treated like a wreck (stopped, never driven again, blocks its slot until moved).
	//------------------------------------------------------------------------------------------------

	//! The faction opposing the nearest player (read from the faction manager), or null if there is no player
	//! with a faction or no enemy faction.
	protected Faction FindHostileFaction()
	{
		PlayerManager players = GetGame().GetPlayerManager();
		FactionManager factionManager = GetGame().GetFactionManager();
		if (!players || !factionManager)
			return null;

		array<int> ids = {};
		players.GetPlayers(ids);

		vector centre = GetOwner().GetOrigin();
		Faction playerFaction;
		float bestDistSq = float.MAX;
		foreach (int playerId : ids)
		{
			IEntity playerEntity = players.GetPlayerControlledEntity(playerId);
			if (!playerEntity)
				continue;

			float distSq = vector.DistanceSq(playerEntity.GetOrigin(), centre);
			if (distSq >= bestDistSq)
				continue;

			Faction candidate = SCR_FactionManager.SGetPlayerFaction(playerId);
			if (!candidate)
				candidate = SCR_FactionManager.SGetFaction(playerEntity);
			if (!candidate)
				continue;

			playerFaction = candidate;
			bestDistSq = distSq;
		}

		if (!playerFaction)
			return null;

		array<Faction> factions = {};
		factionManager.GetFactionsList(factions);

		// Prefer a playable enemy faction (the opposing army), else any enemy.
		Faction anyEnemy;
		foreach (Faction other : factions)
		{
			if (!other || other == playerFaction || !playerFaction.IsFactionEnemy(other))
				continue;

			SCR_Faction scripted = SCR_Faction.Cast(other);
			if (scripted && scripted.IsPlayable())
				return other;

			if (!anyEnemy)
				anyEnemy = other;
		}

		return anyEnemy;
	}

	//! True while the car can still turn hostile (on the road, in the lane, nothing else decided for it).
	protected bool CanTurnHostile(EEF_CheckpointVehicleState state)
	{
		if (!state || !state.m_Vehicle || !state.m_OccupantGroup)
			return false;

		// A detain order does NOT rule it out: the crew can still turn while pulling over, parked, or climbing
		// out - right up to the moment the first one sits down on the ground.
		// Only contraband cars have a reason to fight.
		if (!state.m_bHasContraband)
			return false;

		if (!state.m_bSeated || state.m_bHostile || state.m_bHostilePending || state.m_bDriverDown || state.m_bSurrendered || state.m_bFleeRequested || state.m_bFleeing
			|| state.m_bCleared || state.m_bCrewSat || state.m_bPlayerClaimed)
			return false;

		return state.m_eState == EEF_ECheckpointVehicleState.QUEUED
			|| state.m_eState == EEF_ECheckpointVehicleState.AT_FRONT
			|| state.m_eState == EEF_ECheckpointVehicleState.HELD
			|| state.m_eState == EEF_ECheckpointVehicleState.DETAINED;
	}

	//! The chance for one roll. During the detain process it is the base chance times m_fHostileDetainedFactor,
	//! but never below m_fHostileDetainedMin (so it is never zero while hostile crews are enabled at all). A base
	//! chance of 0 switches hostile crews off completely.
	protected float GetHostileChance(bool detained)
	{
		if (m_fHostileChance <= 0)
			return 0;

		if (!detained)
			return m_fHostileChance;

		float scaled = m_fHostileChance * m_fHostileDetainedFactor;
		return Math.Min(Math.Max(scaled, m_fHostileDetainedMin), m_fHostileChance);
	}

	//! One roll. True if the crew turned hostile.
	//! immediate = the crew turn at once (no delay) and true is returned if they did; used for the single
	//! check when a detained car stops in its bay, where a delay would let the crew sit down first.
	//! detained = a roll during the detain process: it uses a lower chance (see GetHostileChance).
	protected bool RollHostile(EEF_CheckpointVehicleState state, bool immediate = false, bool detained = false)
	{
		float chance = GetHostileChance(detained);
		if (chance <= 0 || !CanTurnHostile(state))
			return false;

		if (Math.RandomFloat(0, 1) >= chance)
			return false;

		if (immediate)
			return TurnHostile(state);

		// The crew do not turn at once: the car keeps going for a few seconds, then it happens.
		state.m_bHostilePending = true;
		float delay = Math.RandomFloat(Math.Max(m_fHostileDelayMin, 0), Math.Max(m_fHostileDelayMax, m_fHostileDelayMin));
		DebugLog(string.Format("Hostile roll succeeded - the crew turns in %1 s.", delay));
		GetGame().GetCallqueue().CallLater(TurnHostileDelayed, delay * 1000, false, state);
		return false;
	}

	protected void TurnHostileDelayed(EEF_CheckpointVehicleState state)
	{
		if (!state || m_aVehicles.Find(state) == -1)
			return;

		state.m_bHostilePending = false;
		if (!CanTurnHostile(state))
		{
			DebugLog("Hostile turn cancelled - the car was cleared, detained or disabled in the meantime.");
			return;
		}

		TurnHostile(state);
	}

	protected bool TurnHostile(EEF_CheckpointVehicleState state)
	{
		Faction enemy = FindHostileFaction();
		if (!enemy)
		{
			DebugLog("Hostile roll succeeded but there is no player with a faction or no enemy faction - skipping.");
			return false;
		}

		state.m_bHostile = true;
		state.m_HostileFaction = enemy;
		state.m_bDriverDown = true;	// from here on it is a blockage that is never driven again
		DebugLog(string.Format("The crew turns hostile (side %1) - the car is stopped dead and the crew get out armed.", enemy.GetFactionKey()));

		// Off the rails, and stopped dead the same way a held car is: the physics body leaves dynamic
		// simulation. (Switching the AI off and braking does NOT stop an AI-driven car - it kept going
		// through the checkpoint.) The car is unfrozen again once the crew are out.
		RemoveRunForState(state);
		state.m_bOnRails = false;

		CarControllerComponent controller = CarControllerComponent.Cast(state.m_Vehicle.FindComponent(CarControllerComponent));
		if (controller)
		{
			VehicleWheeledSimulation sim = controller.GetSimulation();
			if (sim)
			{
				sim.SetThrottle(0);
				sim.SetBreak(1, false);
				sim.SetSteering(0);
			}
			controller.SetPersistentHandBrake(true);
		}

		HoldVehicle(state);

		// HoldVehicle does nothing for a car that is already frozen, so make sure it really is dead still.
		Physics hostilePhys = state.m_Vehicle.GetPhysics();
		if (hostilePhys)
		{
			hostilePhys.SetVelocity(vector.Zero);
			hostilePhys.SetAngularVelocity(vector.Zero);
		}

		vector forward = state.m_Vehicle.GetTransformAxis(2);
		forward[1] = 0;
		if (forward.LengthSq() < 0.01)
			forward = Vector(0, 0, 1);
		forward.Normalize();
		state.m_vRoadForward = forward;
		state.m_fVergeDist = 4.0;
		state.m_fSurrenderTime = GetWorldTimeSeconds();
		state.m_vParkPos = state.m_Vehicle.GetOrigin();

		StartHostileCrew(state);
		return true;
	}

	//! Crew are out: put the (frozen) car back into simulation as a dead, braked, engine-off blockage that a
	//! player can still push or drive off the road.
	protected void HostileSettleCar(EEF_CheckpointVehicleState state)
	{
		if (!state || !state.m_Vehicle || m_aVehicles.Find(state) == -1 || !state.m_bHeld)
			return;

		SCR_PhysicsHelper.ChangeSimulationState(state.m_Vehicle, SimulationState.SIMULATION);
		state.m_bHeld = false;

		CarControllerComponent controller = CarControllerComponent.Cast(state.m_Vehicle.FindComponent(CarControllerComponent));
		if (controller)
		{
			// The engine is left running on purpose: an idling car with an empty driver's seat tells players something is up.
			controller.SetPersistentHandBrake(true);
		}
	}

	//! Switch each living crew member to the hostile side, arm them, and send them out one after another.
	protected void StartHostileCrew(EEF_CheckpointVehicleState state)
	{
		if (!state || !state.m_OccupantGroup)
			return;

		ClearWaypoints(state.m_OccupantGroup);

		// The crew's AI must not be able to board the car again once they are out.
		ReleaseVehicleFromGroup(state);

		array<AIAgent> agents = {};
		state.m_OccupantGroup.GetAgents(agents);

		int delayMs = 200;
		foreach (AIAgent agent : agents)
		{
			if (!agent || !agent.GetControlledEntity() || !IsCharacterAlive(agent.GetControlledEntity()))
				continue;

			IEntity character = agent.GetControlledEntity();
			state.m_aCrew.Insert(character);

			SetCharacterFaction(character, state.m_HostileFaction);
			ArmCharacter(character, state.m_HostileFaction);

			GetGame().GetCallqueue().CallLater(HostileExitMember, delayMs, false, state, character, agent);
			delayMs += 400;
		}

		GetGame().GetCallqueue().CallLater(HostileSettleCar, delayMs + 3500, false, state);
	}

	protected void SetCharacterFaction(IEntity character, Faction faction)
	{
		FactionAffiliationComponent affiliation = FactionAffiliationComponent.Cast(character.FindComponent(FactionAffiliationComponent));
		if (affiliation && faction)
			affiliation.SetAffiliatedFaction(faction);
	}

	//! Put a rifle or a pistol (50/50) of the given side plus spare magazines in the character's inventory.
	protected void ArmCharacter(IEntity character, Faction faction)
	{
		InventoryStorageManagerComponent inventory = InventoryStorageManagerComponent.Cast(character.FindComponent(InventoryStorageManagerComponent));
		if (!inventory)
		{
			DebugLog("Hostile crew member has no inventory manager - cannot arm.");
			return;
		}

		bool western = (faction && faction.GetFactionKey() == "US");
		bool rifle = Math.RandomFloat(0, 1) < 0.5;

		ResourceName weapon;
		ResourceName magazine;
		if (western && rifle)
		{
			weapon = "{3E413771E1834D2F}Prefabs/Weapons/Rifles/M16/Rifle_M16A2.et";
			magazine = "{2EBF60EF24B108FC}Prefabs/Weapons/Magazines/Magazine_556x45_STANAG_30rnd_M855_Ball.et";
		}
		else if (western)
		{
			weapon = "{1353C6EAD1DCFE43}Prefabs/Weapons/Handguns/M9/Handgun_M9.et";
			magazine = "{9C05543A503DB80E}Prefabs/Weapons/Magazines/Magazine_9x19_M9_15rnd_Ball.et";
		}
		else if (rifle)
		{
			weapon = "{FA5C25BF66A53DCF}Prefabs/Weapons/Rifles/AK74/Rifle_AK74.et";
			magazine = "{BBB50A815A2F916B}Prefabs/Weapons/Magazines/Magazine_545x39_AK_30rnd_Ball.et";
		}
		else
		{
			weapon = "{C0F7DD85A86B2900}Prefabs/Weapons/Handguns/PM/Handgun_PM.et";
			magazine = "{8B853CDD11BA916E}Prefabs/Weapons/Magazines/Magazine_9x18_PM_8rnd_Ball.et";
		}

		bool gunOk = inventory.TrySpawnPrefabToStorage(weapon, null, -1, EStoragePurpose.PURPOSE_ANY);
		bool magsOk = true;
		if (m_iHostileMagazines > 0)
			magsOk = inventory.TrySpawnPrefabToStorage(magazine, null, -1, EStoragePurpose.PURPOSE_ANY, null, m_iHostileMagazines);

		DebugLog(string.Format("Hostile crew member armed: %1 (weapon accepted: %2, magazines accepted: %3).", weapon, gunOk, magsOk));
	}

	//! One hostile crew member climbs out; a moment later their AI wakes up and they head for the checkpoint.
	protected void HostileExitMember(EEF_CheckpointVehicleState state, IEntity character, AIAgent agent)
	{
		if (!state || !character || m_aVehicles.Find(state) == -1)
			return;

		SCR_CompartmentAccessComponent access = SCR_CompartmentAccessComponent.Cast(character.FindComponent(SCR_CompartmentAccessComponent));
		if (access && access.GetCompartment())
		{
			bool animatedOut = access.GetOutVehicle(EGetOutType.ANIMATED, -1, ECloseDoorAfterActions.INVALID, false);
			DebugLog(string.Format("Hostile crew member exits the car (animated exit accepted: %1).", animatedOut));
			if (!animatedOut)
				access.GetOutVehicle(EGetOutType.TELEPORT, -1, ECloseDoorAfterActions.INVALID, false);
		}
		else
			DebugLog("Hostile crew member has no compartment to exit from.");

		GetGame().GetCallqueue().CallLater(HostileEngage, 1500, false, state, character, agent);

		// If they are somehow STILL in a seat after the forced lift-out, retry and finally kill them (see HostileEnsureOut).
		GetGame().GetCallqueue().CallLater(HostileEnsureOut, 3500, false, state, character, 0);

		// Once the crew are out the car belongs to nobody (see MakeVehicleNeutral).
		GetGame().GetCallqueue().CallLater(MakeVehicleNeutral, 2500, false, state.m_Vehicle);
	}

	protected void HostileEngage(EEF_CheckpointVehicleState state, IEntity character, AIAgent agent)
	{
		if (!state || !character || !agent || m_aVehicles.Find(state) == -1 || !IsCharacterAlive(character))
			return;

		// Still sitting in the car (the animated exit did not happen, e.g. the car was frozen): lift them out.
		SCR_CompartmentAccessComponent seatAccess = SCR_CompartmentAccessComponent.Cast(character.FindComponent(SCR_CompartmentAccessComponent));
		if (seatAccess && seatAccess.GetCompartment())
		{
			DebugLog("Hostile crew member is still in the car - forcing them out.");
			seatAccess.GetOutVehicle(EGetOutType.TELEPORT, -1, ECloseDoorAfterActions.INVALID, false);
		}

		agent.ActivateAI();

		// Advance on the checkpoint so the fight starts; the AI engages anything hostile it sees on the way.
		if (state.m_OccupantGroup)
			AssignMoveWaypoint(state.m_OccupantGroup, GetOwner().GetOrigin(), 15);
	}

	//! Last line of defence for a hostile crew member: a hostile car is a frozen blockage, so nobody may stay inside it.
	//! Checked after the exit order and the forced lift-out. Still in a seat: lift them out again (up to
	//! HOSTILE_EXIT_RETRIES times, one second apart), and if that fails too they die rather than ride the wreck.
	protected void HostileEnsureOut(EEF_CheckpointVehicleState state, IEntity character, int attempt)
	{
		if (!state || !character || m_aVehicles.Find(state) == -1)
			return;

		if (!IsCharacterAlive(character))
		{
			DebugLog("Hostile crew member is dead or down - nothing more to check for them.");
			return;
		}

		// "In the car" means any of: in a compartment, in the middle of getting out (an exit that never finishes
		// leaves GetCompartment() empty while the character is still inside), or parented to the vehicle.
		SCR_CompartmentAccessComponent access = SCR_CompartmentAccessComponent.Cast(character.FindComponent(SCR_CompartmentAccessComponent));
		bool inCompartment = access && access.GetCompartment();
		bool gettingOut = access && access.IsGettingOut();
		IEntity vehicleIn = CompartmentAccessComponent.GetVehicleIn(character);
		bool inCar = inCompartment || gettingOut || vehicleIn;

		if (inCar)
		{
			// Never let the crew's AI climb back in.
			ReleaseVehicleFromGroup(state);

			if (attempt < HOSTILE_EXIT_RETRIES)
			{
				DebugLog(string.Format("Hostile crew member is STILL in the car (compartment %1, getting out %2, vehicle %3) - lifting them out again (retry %4 of %5).", inCompartment, gettingOut, vehicleIn != null, attempt + 1, HOSTILE_EXIT_RETRIES));
				if (access)
					access.GetOutVehicle(EGetOutType.TELEPORT, -1, ECloseDoorAfterActions.INVALID, false);
			}
			else
			{
				CharacterControllerComponent controller = CharacterControllerComponent.Cast(character.FindComponent(CharacterControllerComponent));
				if (controller)
				{
					DebugLog("Hostile crew member could not leave the car - they die in their seat.");
					controller.ForceDeath();
				}
				return;
			}
		}
		else if (attempt == 0)
			DebugLog("Hostile crew member is out of the car.");

		// Keep watching for a while: an AI that got out can try to climb back in.
		if (attempt < HOSTILE_WATCH_PASSES)
			GetGame().GetCallqueue().CallLater(HostileEnsureOut, 1000, false, state, character, attempt + 1);
	}

	//! Take the car off the occupant group's list of usable vehicles, so the crew's AI no longer tries to board it
	//! (the AI logged "Stuck trying to get into vehicle" after the crew had been lifted out).
	protected void ReleaseVehicleFromGroup(EEF_CheckpointVehicleState state)
	{
		if (!state || !state.m_Vehicle || !state.m_OccupantGroup)
			return;

		SCR_AIGroupUtilityComponent utility = state.m_OccupantGroup.GetGroupUtilityComponent();
		if (!utility || !utility.m_VehicleMgr)
			return;

		IEntity vehicleRef = state.m_Vehicle;
		SCR_AIVehicleUsageComponent usage = SCR_AIVehicleUsageComponent.FindOnNearestParent(state.m_Vehicle, vehicleRef);
		if (!usage || utility.m_VehicleMgr.FindVehicle(usage) == null)
			return;

		utility.m_VehicleMgr.RemoveVehicle(usage);
		DebugLog("The hostile car was taken off its crew's list of usable vehicles.");
	}

	//! World-query callback for LogStuckBlockers: remember vehicles and characters.
	protected ref array<IEntity> m_aBlockerHits = new array<IEntity>();

	protected bool QueryBlockerCallback(IEntity entity)
	{
		if (Vehicle.Cast(entity) || ChimeraCharacter.Cast(entity))
			m_aBlockerHits.Insert(entity);

		return true;
	}

	//! Diagnostic for a drive that gave up as stuck: what is within 9 m of the car (vehicles, characters), and whether
	//! the checkpoint tracks it.
	protected void LogStuckBlockers(IEntity vehicle)
	{
		if (!m_bDebugLog || !vehicle)
			return;

		m_aBlockerHits.Clear();
		GetGame().GetWorld().QueryEntitiesBySphere(vehicle.GetOrigin(), 9.0, QueryBlockerCallback);

		int shown = 0;
		foreach (IEntity hit : m_aBlockerHits)
		{
			if (!hit || hit == vehicle || hit.GetParent() == vehicle)
				continue;

			string what = "character";
			if (Vehicle.Cast(hit))
				what = "vehicle";

			string tracking = "not tracked by the checkpoint";
			foreach (EEF_CheckpointVehicleState other : m_aVehicles)
			{
				if (other && other.m_Vehicle == hit)
				{
					tracking = string.Format("tracked: state %1, hostile %2, slot %3, held %4", typename.EnumToString(EEF_ECheckpointVehicleState, other.m_eState), other.m_bHostile, other.m_iQueueSlot, other.m_bHeld);
					break;
				}
			}

			DebugLog(string.Format("Rails: stuck car - %1 %2 m away (%3).", what, vector.Distance(vehicle.GetOrigin(), hit.GetOrigin()), tracking));
			shown++;
			if (shown >= 12)
				break;
		}

		DebugLog(string.Format("Rails: stuck car - %1 vehicle(s)/character(s) listed within 9 m.", shown));
	}

	//! True if the character is not dead or unconscious.
	protected bool IsCharacterAlive(IEntity character)
	{
		if (!character)
			return false;
		CharacterControllerComponent controller = CharacterControllerComponent.Cast(character.FindComponent(CharacterControllerComponent));
		return !controller || controller.GetLifeState() == ECharacterLifeState.ALIVE;
	}

	//! Crew gets out one after another (a short gap so they do not all use the same door at once).
	protected void ExitCrew(EEF_CheckpointVehicleState state)
	{
		if (!state || !state.m_OccupantGroup)
			return;

		// The AI stays OFF (it was switched off at the hold) and the drive order is dropped, so nobody
		// climbs back in and drives away.
		ClearWaypoints(state.m_OccupantGroup);

		array<AIAgent> agents = {};
		state.m_OccupantGroup.GetAgents(agents);

		int total = agents.Count();
		int delayMs = 300;
		for (int i = 0; i < total; i++)
		{
			if (!agents[i] || !agents[i].GetControlledEntity() || !IsCharacterAlive(agents[i].GetControlledEntity()))
				continue;

			state.m_aCrew.Insert(agents[i].GetControlledEntity());
			GetGame().GetCallqueue().CallLater(ExitCrewMember, delayMs, false, state, agents[i].GetControlledEntity(), i, total);
			delayMs += 700;
		}
	}

	protected void ExitCrewMember(EEF_CheckpointVehicleState state, IEntity character, int index, int total)
	{
		if (!state || !character || state.m_bHostile || m_aVehicles.Find(state) == -1)
			return;

		SCR_CompartmentAccessComponent access = SCR_CompartmentAccessComponent.Cast(character.FindComponent(SCR_CompartmentAccessComponent));
		if (!access || !access.GetCompartment())
			return;

		// Walk out through the door; if that is refused, step out on the spot.
		if (!access.GetOutVehicle(EGetOutType.ANIMATED, -1, ECloseDoorAfterActions.INVALID, false))
		{
			DebugLog("Crew member could not climb out - teleporting out.");
			access.GetOutVehicle(EGetOutType.TELEPORT, -1, ECloseDoorAfterActions.INVALID, false);
		}

		GetGame().GetCallqueue().CallLater(SitCrewMember, 3500, false, state, character, index, total, 0);

		// Once the crew are out the car belongs to nobody (see MakeVehicleNeutral).
		GetGame().GetCallqueue().CallLater(MakeVehicleNeutral, 2500, false, state.m_Vehicle);
	}

	//! A vehicle takes the faction of whoever is inside it, so a car full of civilians is a CIV car. The CIV
	//! faction lists US as friendly but the US faction does not list CIV, so a US player is treated as hostile
	//! to the car and cannot see its trunk. The checkpoint cars are kept free of any faction instead, so a
	//! player of any faction can search a car (queued or detained) before deciding what to do with it.
	protected void MakeVehicleNeutral(IEntity vehicle)
	{
		if (!vehicle)
			return;

		SCR_VehicleFactionAffiliationComponent affiliation = SCR_VehicleFactionAffiliationComponent.Cast(vehicle.FindComponent(SCR_VehicleFactionAffiliationComponent));
		if (!affiliation)
			return;

		Faction before = affiliation.GetAffiliatedFaction();
		if (!before)
			return;

		affiliation.ClearAffiliatedFaction();
		DebugLog(string.Format("Vehicle cleared of its faction (%1) so any player can search it. Faction now: %2", before.GetFactionKey(), affiliation.GetAffiliatedFaction()));
	}

	//! Put a crew member on the verge in front of their car, facing it, sitting on the ground.
	protected void SitCrewMember(EEF_CheckpointVehicleState state, IEntity character, int index, int total, int attempt)
	{
		if (!state || !character || !state.m_Vehicle || state.m_bHostile || m_aVehicles.Find(state) == -1)
			return;

		// Still climbing out? Give it a little longer.
		SCR_CompartmentAccessComponent access = SCR_CompartmentAccessComponent.Cast(character.FindComponent(SCR_CompartmentAccessComponent));
		if (access && access.GetCompartment())
		{
			if (attempt < 3)
				GetGame().GetCallqueue().CallLater(SitCrewMember, 2000, false, state, character, index, total, attempt + 1);
			return;
		}

		// Off the road, level with the car and a little ahead of it, spread out along the verge.
		vector roadForward = state.m_vRoadForward;
		roadForward[1] = 0;
		if (roadForward.LengthSq() < 0.01)
			roadForward = Vector(0, 0, 1);
		roadForward.Normalize();
		vector roadLeft = Vector(-roadForward[2], 0, roadForward[0]);
		vector carPos = state.m_Vehicle.GetOrigin();

		float spread = (index - (total - 1) * 0.5) * 1.6;
		vector spot = carPos + roadLeft * state.m_fVergeDist + roadForward * (1.5 + spread);
		spot[1] = GetGame().GetWorld().GetSurfaceY(spot[0], spot[2]);

		vector toCar = carPos - spot;
		toCar[1] = 0;
		vector angles = toCar.VectorToAngles();

		vector mat[4];
		Math3D.AnglesToMatrix(angles, mat);
		mat[3] = spot;

		// From here the crew are sitting detainees: too late for the crew to turn hostile.
		state.m_bCrewSat = true;

		character.SetWorldTransform(mat);
		character.Update();

		if (!m_SitAnimation)
			m_SitAnimation = new SCR_AIAnimation_Sitting();

		if (!m_SitAnimation.StartAnimation(character, mat))
			DebugLog("Could not start the sit-down animation on a crew member.");
	}

	//------------------------------------------------------------------------------------------------
	// DIRECTING DETAINEES (player interaction on the crew member themselves, see EEF_DetaineeActions.c)
	//
	// Stand / sit and send back to the roadside while they are detained; "Take into squad" swaps their side
	// and puts them in the player's AI squad so the vanilla commanding menu can move them; "Drop unit"
	// removes them.
	//------------------------------------------------------------------------------------------------

	static const int CREW_COMMAND_TOGGLE_SIT = 0;		//! Stand up if sitting, sit down if standing
	static const int CREW_COMMAND_TO_ROADSIDE = 1;		//! Back to the sitting spot beside the road
	static const int CREW_COMMAND_RECRUIT = 5;			//! Swap sides and take into the player squad (the vanilla commanding menu then works on them)
	static const int CREW_COMMAND_DROP = 6;				//! Remove from the squad and delete the unit

	//! Called by the detainee actions on the server. True if the character is a detained crew member here.
	static bool DirectCrewEntity(IEntity character, int command, IEntity user)
	{
		if (!character)
			return false;

		foreach (EEF_CheckpointComponent checkpoint : s_aInstances)
		{
			if (checkpoint && checkpoint.DirectCrew(character, command, user))
				return true;
		}

		// A unit already in the player's squad outlives the detained car it came from (the car's record is
		// removed when it times out or despawns), so "Drop unit" must work without that record.
		if (command == CREW_COMMAND_DROP && !s_aInstances.IsEmpty() && s_aInstances[0])
		{
			s_aInstances[0].DropDetainee(null, character, user);
			return true;
		}
		return false;
	}

	protected bool DirectCrew(IEntity character, int command, IEntity user)
	{
		foreach (EEF_CheckpointVehicleState state : m_aVehicles)
		{
			if (!state || state.m_bHostile || (state.m_eState != EEF_ECheckpointVehicleState.DETAINED && !state.m_bSurrendered))
				continue;

			AIAgent agent = FindCrewAgent(state, character);
			if (!agent && state.m_aCrew.Find(character) == -1)
				continue;

			int total = 1;
			if (state.m_OccupantGroup)
				total = Math.Max(state.m_OccupantGroup.GetAgentsCount(), 1);

			int index = 0;
			if (state.m_OccupantGroup)
			{
				array<AIAgent> agents = {};
				state.m_OccupantGroup.GetAgents(agents);
				if (agent)
					index = Math.Max(agents.Find(agent), 0);
			}

			ApplyCrewCommand(state, character, agent, index, total, command, user);
			return true;
		}
		return false;
	}

	//! Take a detainee into the player's squad: swap their side to the player's, switch their AI on and add
	//! them to the player's AI (slave) group. From then on the vanilla commanding menu (move, follow, stop...)
	//! works on them like any recruited soldier.
	protected void RecruitDetainee(EEF_CheckpointVehicleState state, IEntity character, IEntity user, int attempt)
	{
		SCR_ChimeraCharacter target = SCR_ChimeraCharacter.Cast(character);
		SCR_ChimeraCharacter commander = SCR_ChimeraCharacter.Cast(user);
		if (!target || !commander || target.IsRecruited())
			return;

		int playerId = GetGame().GetPlayerManager().GetPlayerIdFromControlledEntity(user);
		SCR_GroupsManagerComponent groupsManager = SCR_GroupsManagerComponent.GetInstance();
		if (playerId <= 0 || !groupsManager)
			return;

		SCR_AIGroup group = groupsManager.GetPlayerGroup(playerId);
		if (!group)
		{
			// Squad commanding needs the player to be in a group: put them in one (as the Groups menu would).
			if (attempt < 5)
			{
				if (attempt == 0)
				{
					JoinPlayerToNewGroup(groupsManager, playerId, commander);
				}
				GetGame().GetCallqueue().CallLater(RecruitDetainee, 300, false, state, character, user, attempt + 1);
			}
			else
			{
				DebugLog("Cannot take the detainee into the squad - the player could not be put in a group.");
			}
			return;
		}

		SCR_AIGroup slave = group.GetSlave();
		if (!slave && attempt == 0)
		{
			CreateSlaveGroup(groupsManager, group);
			slave = group.GetSlave();
		}

		if (!slave)
		{
			if (attempt < 5)
				GetGame().GetCallqueue().CallLater(RecruitDetainee, 200, false, state, character, user, attempt + 1);
			else
				DebugLog("Cannot take the detainee into the squad - no AI squad group could be created.");
			return;
		}

		SCR_PlayerControllerGroupComponent groupController = SCR_PlayerControllerGroupComponent.GetPlayerControllerComponent(playerId);
		if (!groupController)
			return;

		// Change sides so the squad (and the commanding menu) treat them as friendly.
		FactionAffiliationComponent affiliation = FactionAffiliationComponent.Cast(character.FindComponent(FactionAffiliationComponent));
		Faction faction = commander.GetFaction();
		if (affiliation && faction)
			affiliation.SetAffiliatedFaction(faction);

		// Up off the ground, no more sitting pose.
		m_aStandingCrew.RemoveItem(character);
		if (!m_SitAnimation)
			m_SitAnimation = new SCR_AIAnimation_Sitting();
		m_SitAnimation.StopAnimation(character, true);

		AIAgent agent = FindCrewAgent(state, character);
		if (!agent)
		{
			AIControlComponent control = target.GetAIControlComponent();
			if (control)
				agent = control.GetControlAIAgent();
		}
		if (agent)
			agent.ActivateAI();

		groupController.AddAIToSlaveGroup(target, group);
		target.SetRecruited(true);
		if (state.m_aRecruited.Find(character) == -1)
			state.m_aRecruited.Insert(character);

		DebugLog("Detainee changed sides and joined the player's squad.");
	}

	//! Squad commanding needs the player to have a faction and a group. Worlds with a free-roam game mode
	//! (like the test world) give neither, so give the player their character's faction and a fresh group.
	protected void JoinPlayerToNewGroup(SCR_GroupsManagerComponent groupsManager, int playerId, SCR_ChimeraCharacter commander)
	{
		PlayerController controller = GetGame().GetPlayerManager().GetPlayerController(playerId);
		Faction faction = commander.GetFaction();
		if (!controller || !faction)
		{
			DebugLog("Cannot create a group for the player - no player controller or faction.");
			return;
		}

		SCR_PlayerFactionAffiliationComponent playerFaction = SCR_PlayerFactionAffiliationComponent.Cast(controller.FindComponent(SCR_PlayerFactionAffiliationComponent));
		if (playerFaction && !playerFaction.GetAffiliatedFaction())
		{
			playerFaction.SetFaction_S(faction);
			DebugLog(string.Format("The player had no faction - set to %1.", faction.GetFactionKey()));
		}

		SCR_AIGroup newGroup = groupsManager.CreateNewPlayableGroup(faction);
		SCR_PlayerControllerGroupComponent groupController = SCR_PlayerControllerGroupComponent.GetPlayerControllerComponent(playerId);
		if (!newGroup || !groupController)
		{
			DebugLog("Could not create a playable group for the player.");
			return;
		}

		groupController.RPC_AskJoinGroup(newGroup.GetGroupID());
		DebugLog("The player was not in a group - created one and joined it.");
	}

	//! Spawn the player's AI squad group when they have none yet (the same steps the commanding menu takes).
	protected void CreateSlaveGroup(SCR_GroupsManagerComponent groupsManager, SCR_AIGroup group)
	{
		SCR_CommandingManagerComponent commanding = SCR_CommandingManagerComponent.GetInstance();
		if (!commanding)
			return;

		SCR_AIGroup slave = SCR_AIGroup.Cast(GetGame().SpawnEntityPrefab(Resource.Load(commanding.GetGroupPrefab())));
		if (!slave)
			return;

		slave.SetDeleteWhenEmpty(false);

		RplComponent groupRpl = RplComponent.Cast(group.FindComponent(RplComponent));
		RplComponent slaveRpl = RplComponent.Cast(slave.FindComponent(RplComponent));
		if (!groupRpl || !slaveRpl)
			return;

		groupsManager.RequestSetGroupSlave(groupRpl.Id(), slaveRpl.Id());
	}

	//! Remove the detainee from the player's squad (if in it) and delete the unit.
	protected void DropDetainee(EEF_CheckpointVehicleState state, IEntity character, IEntity user)
	{
		SCR_ChimeraCharacter target = SCR_ChimeraCharacter.Cast(character);
		if (!target)
			return;

		int playerId = GetGame().GetPlayerManager().GetPlayerIdFromControlledEntity(user);
		SCR_GroupsManagerComponent groupsManager = SCR_GroupsManagerComponent.GetInstance();
		if (target.IsRecruited() && playerId > 0 && groupsManager)
		{
			SCR_AIGroup group = groupsManager.GetPlayerGroup(playerId);
			SCR_PlayerControllerGroupComponent groupController = SCR_PlayerControllerGroupComponent.GetPlayerControllerComponent(playerId);
			if (group && groupController)
				groupController.RemoveAiFromSlaveGroup(target, group);
		}

		AIAgent agent;
		if (state)
			agent = FindCrewAgent(state, character);
		if (!agent)
		{
			AIControlComponent control = target.GetAIControlComponent();
			if (control)
				agent = control.GetControlAIAgent();
		}

		if (state)
		{
			state.m_aCrew.RemoveItem(character);
			state.m_aRecruited.RemoveItem(character);
		}
		m_aStandingCrew.RemoveItem(character);

		GetGame().GetCallqueue().CallLater(DeleteDetainee, 300, false, character, agent);
		DebugLog("Detainee dropped - unit removed.");
	}

	protected void DeleteDetainee(IEntity character, AIAgent agent)
	{
		if (agent)
			SCR_EntityHelper.DeleteEntityAndChildren(agent);
		if (character)
			SCR_EntityHelper.DeleteEntityAndChildren(character);
	}

	//! A detainee a player recruited through the vanilla commanding menu (look at them, open the menu,
	//! "Recruit") now belongs to the player's squad. Their AI was switched off while they sat, so switch it
	//! back on and stand them up: from here the normal squad commands (move, follow, stop...) apply.
	protected void ActivateRecruitedCrew(EEF_CheckpointVehicleState state)
	{
		if (!state.m_bParked)
			return;

		foreach (IEntity crew : state.m_aCrew)
		{
			SCR_ChimeraCharacter character = SCR_ChimeraCharacter.Cast(crew);
			if (!character || !character.IsRecruited() || state.m_aRecruited.Find(crew) != -1)
				continue;

			state.m_aRecruited.Insert(crew);
			m_aStandingCrew.RemoveItem(crew);
			if (!m_SitAnimation)
				m_SitAnimation = new SCR_AIAnimation_Sitting();
			m_SitAnimation.StopAnimation(crew, true);

			AIControlComponent control = character.GetAIControlComponent();
			AIAgent agent;
			if (control)
				agent = control.GetControlAIAgent();
			if (agent)
				agent.ActivateAI();

			DebugLog("Detainee recruited by a player - AI on, stands up.");
		}
	}

	//! The AI agent controlling a detained crew member (or null once a player has taken them into their squad).
	protected AIAgent FindCrewAgent(EEF_CheckpointVehicleState state, IEntity character)
	{
		array<AIAgent> agents = {};
		if (state.m_OccupantGroup)
			state.m_OccupantGroup.GetAgents(agents);

		foreach (AIAgent agent : agents)
		{
			if (agent && agent.GetControlledEntity() == character)
				return agent;
		}
		return null;
	}

	protected void ApplyCrewCommand(EEF_CheckpointVehicleState state, IEntity character, AIAgent agent, int index, int total, int command, IEntity user)
	{
		if (!m_SitAnimation)
			m_SitAnimation = new SCR_AIAnimation_Sitting();

		if (command == CREW_COMMAND_RECRUIT)
		{
			RecruitDetainee(state, character, user, 0);
			return;
		}

		if (command == CREW_COMMAND_DROP)
		{
			DropDetainee(state, character, user);
			return;
		}

		if (command == CREW_COMMAND_TOGGLE_SIT)
		{
			bool standing = m_aStandingCrew.Find(character) != -1;

			vector transform[4];
			character.GetWorldTransform(transform);

			if (standing)
			{
				m_aStandingCrew.RemoveItem(character);
				m_SitAnimation.StartAnimation(character, transform);
				DebugLog("Detainee ordered to sit down.");
			}
			else
			{
				m_aStandingCrew.Insert(character);
				m_SitAnimation.StopAnimation(character, false);
				DebugLog("Detainee ordered to stand up.");
			}
			return;
		}

		if (command == CREW_COMMAND_TO_ROADSIDE)
		{
			m_aStandingCrew.RemoveItem(character);
			m_SitAnimation.StopAnimation(character, true);
			GetGame().GetCallqueue().CallLater(SitCrewMember, 700, false, state, character, index, total, 0);
			DebugLog("Detainee ordered back to the roadside.");
		}
	}

	//! True if a player has taken a parked detained car: moved it from where it parked, or got into it.
	protected bool IsDetainedCarDisturbed(EEF_CheckpointVehicleState state)
	{
		if (!state || !state.m_Vehicle || !state.m_bParked)
			return false;

		if (vector.Distance(state.m_Vehicle.GetOrigin(), state.m_vParkPos) > 2.5)
			return true;

		array<IEntity> crew = {};
		if (state.m_OccupantGroup)
		{
			array<AIAgent> agents = {};
			state.m_OccupantGroup.GetAgents(agents);
			foreach (AIAgent agent : agents)
			{
				if (agent && agent.GetControlledEntity())
					crew.Insert(agent.GetControlledEntity());
			}
		}

		SCR_BaseCompartmentManagerComponent compMgr = SCR_BaseCompartmentManagerComponent.Cast(state.m_Vehicle.FindComponent(SCR_BaseCompartmentManagerComponent));
		if (!compMgr)
			return false;

		return HasOutsiderInSlots(compMgr, ECompartmentType.PILOT, crew)
			|| HasOutsiderInSlots(compMgr, ECompartmentType.CARGO, crew)
			|| HasOutsiderInSlots(compMgr, ECompartmentType.TURRET, crew);
	}

	protected bool HasOutsiderInSlots(SCR_BaseCompartmentManagerComponent compMgr, ECompartmentType type, array<IEntity> crew)
	{
		array<BaseCompartmentSlot> slots = {};
		compMgr.GetCompartmentsOfType(slots, type);
		foreach (BaseCompartmentSlot slot : slots)
		{
			if (!slot)
				continue;

			IEntity occupant = slot.GetOccupant();
			if (occupant && crew.Find(occupant) == -1)
				return true;
		}
		return false;
	}

	//------------------------------------------------------------------------------------------------
	// RELEASE GATE (Stage 2 #18 debug stand-in for #19)
	//------------------------------------------------------------------------------------------------

	//! Start the debug hold timer for a vehicle that just became HELD at the front. Stage 3 (#19)
	//! replaces this with the real player permit/deny; until then a timer auto-releases the vehicle.
	protected void BeginHold(EEF_CheckpointVehicleState state)
	{
		// Cleared while it was still rolling up: nothing to wait for.
		if (state.m_bCleared)
		{
			ReleaseVehicle(state);
			return;
		}

		// Ordered detained while it was still rolling up and rolled "run": bolt now.
		if (state.m_bFleeRequested)
		{
			StartFlee(state);
			return;
		}

		// Ordered detained while it was still rolling up: pull over now.
		if (state.m_bDetainRequested && BeginPullOver(state))
			return;

		if (m_fHoldSeconds <= 0)
		{
			DebugLog("Front vehicle held - waiting for a player to clear it.");
			return;
		}

		state.m_bReleasePending = true;
		GetGame().GetCallqueue().CallLater(ReleaseHeldVehicle, m_fHoldSeconds * 1000, false, state);
	}

	override void EOnFrame(IEntity owner, float timeSlice)
	{
		ApplyScriptedOutputs();
		DrawScriptedDebug();
		DrawSpawnPathDebug();
	}

	override void EOnFixedFrame(IEntity owner, float timeSlice)
	{
		StepScriptedRuns(timeSlice);
		ApplyScriptedOutputs();
	}

	//! Frame events are only enabled while a scripted drive is running.
	protected void UpdateEventMask()
	{
		if (!m_aScriptedRuns.IsEmpty() || m_bSpawnPathActive)
			SetEventMask(GetOwner(), EntityEvent.FRAME | EntityEvent.FIXEDFRAME);
		else
			ClearEventMask(GetOwner(), EntityEvent.FRAME | EntityEvent.FIXEDFRAME);
	}

	//------------------------------------------------------------------------------------------------
	// SCRIPTED RAILS (approach, queue and departure)
	//
	// A vehicle that enters the checkpoint zone is taken off the AI and
	// driven from script along the road line (lane offset kept): it stops at its queue gate, moves up
	// when the queue advances, and after release drives on to the despawn point. All of it is plain
	// throttle/brake/steering on the wheeled simulation, so it replicates like any server-driven car.
	//------------------------------------------------------------------------------------------------

	protected const int SCRIPTED_KIND_APPROACH = 0;
	protected const int SCRIPTED_KIND_DEPARTURE = 1;
	protected const int SCRIPTED_KIND_TRANSIT = 3;		//! Spawn to the checkpoint zone, driven from script along the validated route (see StartScriptedTransit)
	protected const float DEPARTURE_CLEAR_DISTANCE = 30.0;	//! A released car crawls this far (clear of the roadblock pieces), then speeds up to road speed
	protected const int SCRIPTED_KIND_PULLOVER = 2;		//! Detain: drive on while moving over to the roadside, stop there

	protected ref array<ref EEF_ScriptedDriveRun> m_aScriptedRuns = new array<ref EEF_ScriptedDriveRun>();

	//! Total horizontal length of a polyline.
	protected float GetPolylineLength(array<vector> line)
	{
		float total = 0;
		for (int i = 1; i < line.Count(); i++)
		{
			float dx = line[i][0] - line[i - 1][0];
			float dz = line[i][2] - line[i - 1][2];
			total += Math.Sqrt(dx * dx + dz * dz);
		}
		return total;
	}

	//! visited holds (first, last) point pairs of road pieces already used in a chain.
	protected bool IsRoadVisited(array<vector> visited, vector a, vector b)
	{
		for (int i = 0; i + 1 < visited.Count(); i += 2)
		{
			if (vector.DistanceSq(visited[i], a) < 0.25 && vector.DistanceSq(visited[i + 1], b) < 0.25)
				return true;
		}
		return false;
	}

	//! Continue a road polyline onto the next road piece: the unvisited road whose nearer end lies within
	//! 8 m of the line's end and which carries on in the same direction (ties favour the way toward
	//! towardPos). Appends that piece's points, oriented away from the junction. False if none.
	protected bool AppendNextRoad(RoadNetworkManager roadMgr, array<vector> line, vector towardPos, array<vector> visited, vector fallbackDir)
	{
		int count = line.Count();
		if (count < 2)
			return false;

		vector end = line[count - 1];
		vector dirEnd = vector.Zero;
		for (int k = count - 2; k >= 0; k--)
		{
			dirEnd = end - line[k];
			dirEnd[1] = 0;
			if (dirEnd.LengthSq() > 0.25)
				break;
		}

		// The line can be degenerate when the start lies at (or past) the end of its road piece - e.g. a car
		// standing in the gap a roadblock cuts into the road. Fall back to the road's own travel direction.
		if (dirEnd.LengthSq() < 0.0001)
			dirEnd = fallbackDir;
		if (dirEnd.LengthSq() < 0.0001)
			return false;
		dirEnd.Normalize();

		vector toToward = towardPos - end;
		toToward[1] = 0;
		if (toToward.LengthSq() > 0.0001)
			toToward.Normalize();

		array<BaseRoad> roads = {};
		roadMgr.GetRoadsInAABB(Vector(end[0] - 25, end[1] - 30, end[2] - 25), Vector(end[0] + 25, end[1] + 30, end[2] + 25), roads);

		array<vector> bestPts = null;
		bool bestFromStart = true;
		float bestNearDist = 0;
		float bestWidth = 0;
		float bestScore = -10;

		foreach (BaseRoad candidate : roads)
		{
			if (!candidate)
				continue;

			array<vector> pts = {};
			candidate.GetPoints(pts);
			int n = pts.Count();
			if (n < 2)
				continue;

			vector a = pts[0];
			vector b = pts[n - 1];
			if (IsRoadVisited(visited, a, b))
				continue;

			float dxA = a[0] - end[0];
			float dzA = a[2] - end[2];
			float dxB = b[0] - end[0];
			float dzB = b[2] - end[2];
			float distA = Math.Sqrt(dxA * dxA + dzA * dzA);
			float distB = Math.Sqrt(dxB * dxB + dzB * dzB);
			bool fromStart = distA <= distB;
			float nearDist = Math.Min(distA, distB);
			if (nearDist > 15.0)
				continue;

			vector dirC;
			if (fromStart)
				dirC = pts[1] - pts[0];
			else
				dirC = pts[n - 2] - pts[n - 1];
			dirC[1] = 0;
			if (dirC.LengthSq() < 0.0001)
				continue;
			dirC.Normalize();

			float align = dirC[0] * dirEnd[0] + dirC[2] * dirEnd[2];
			if (align < 0.2)
				continue;

			float score = align + 0.5 * (dirC[0] * toToward[0] + dirC[2] * toToward[2]);
			if (candidate.GetWidth() < 4.0)
				score -= 0.4;	// prefer a real road over a narrow track
			if (score > bestScore)
			{
				bestScore = score;
				bestPts = pts;
				bestFromStart = fromStart;
				bestNearDist = nearDist;
				bestWidth = candidate.GetWidth();
			}
		}

		// No road continues the line from one of ITS ENDS - try a T-junction: a road that passes close by with the
		// line ending against its side (a side road or track joining a main road part-way along it).
		if (!bestPts && AppendJunctionRoad(roads, line, towardPos, visited, end))
			return true;

		if (!bestPts)
		{
			DebugLog(string.Format("RoadLine: chain stopped at %1 (heading %2) - %3 road(s) in range, none continues it:", end, dirEnd, roads.Count()));
			int shown = 0;
			foreach (BaseRoad c : roads)
			{
				if (!c || shown >= 8)
					continue;

				array<vector> p = {};
				c.GetPoints(p);
				if (p.Count() < 2)
					continue;

				shown++;
				DebugLog(string.Format("  candidate: width %1 m, %2 point(s), first %3, last %4, alreadyUsed=%5", c.GetWidth(), p.Count(), p[0], p[p.Count() - 1], IsRoadVisited(visited, p[0], p[p.Count() - 1])));
			}
			return false;
		}

		int m = bestPts.Count();
		visited.Insert(bestPts[0]);
		visited.Insert(bestPts[m - 1]);

		// Append the piece's points oriented away from the junction, skipping any point that lies BEHIND the
		// current end (a connector road can start back past where the previous piece ended - taking it
		// literally makes the path zigzag backwards).
		// A narrow connector must not shrink the lane offset: keep the widest road seen in the chain.
		m_fLastLineRoadWidth = Math.Max(m_fLastLineRoadWidth, bestWidth);

		int added = 0;
		if (bestFromStart)
		{
			for (int i = 0; i < m; i++)
			{
				vector pt = bestPts[i];
				if ((pt[0] - end[0]) * dirEnd[0] + (pt[2] - end[2]) * dirEnd[2] <= 0.3)
					continue;
				line.Insert(pt);
				added++;
			}
		}
		else
		{
			for (int i = m - 1; i >= 0; i--)
			{
				vector pt = bestPts[i];
				if ((pt[0] - end[0]) * dirEnd[0] + (pt[2] - end[2]) * dirEnd[2] <= 0.3)
					continue;
				line.Insert(pt);
				added++;
			}
		}

		return added > 0;
	}

	//! T-junction continuation: the line ends within 10 m of the SIDE of an unused road. Join that road at the
	//! nearest point and follow it whichever way heads more toward towardPos. False if no such road.
	protected bool AppendJunctionRoad(array<BaseRoad> roads, array<vector> line, vector towardPos, array<vector> visited, vector end)
	{
		vector toToward = towardPos - end;
		toToward[1] = 0;
		if (toToward.LengthSq() > 0.0001)
			toToward.Normalize();

		array<vector> bestPts = null;
		int bestSeg = 0;
		vector bestProj;
		bool bestForward = true;
		float bestScore = -10;
		float bestWidth = 0;

		foreach (BaseRoad candidate : roads)
		{
			if (!candidate)
				continue;

			array<vector> pts = {};
			candidate.GetPoints(pts);
			int n = pts.Count();
			if (n < 2 || IsRoadVisited(visited, pts[0], pts[n - 1]))
				continue;

			int seg = FindNearestSegment(pts, end);
			vector proj = ProjectOnSegment(end, pts[seg], pts[seg + 1]);
			float dx = proj[0] - end[0];
			float dz = proj[2] - end[2];
			if (Math.Sqrt(dx * dx + dz * dz) > 10.0)
				continue;

			// Which way along that road leads toward the destination?
			vector tangent = pts[seg + 1] - pts[seg];
			tangent[1] = 0;
			if (tangent.LengthSq() < 0.0001)
				continue;
			tangent.Normalize();
			float along = tangent[0] * toToward[0] + tangent[2] * toToward[2];
			bool forward = along >= 0;
			float score = Math.AbsFloat(along);
			if (candidate.GetWidth() < 4.0)
				score -= 0.4;	// prefer a real road over a narrow track

			if (score > bestScore)
			{
				bestScore = score;
				bestPts = pts;
				bestSeg = seg;
				bestProj = proj;
				bestForward = forward;
				bestWidth = candidate.GetWidth();
			}
		}

		if (!bestPts)
			return false;

		int m = bestPts.Count();
		visited.Insert(bestPts[0]);
		visited.Insert(bestPts[m - 1]);
		m_fLastLineRoadWidth = Math.Max(m_fLastLineRoadWidth, bestWidth);

		line.Insert(bestProj);
		if (bestForward)
		{
			for (int i = bestSeg + 1; i < m; i++)
				line.Insert(bestPts[i]);
		}
		else
		{
			for (int i = bestSeg; i >= 0; i--)
				line.Insert(bestPts[i]);
		}

		DebugLog(string.Format("RoadLine: joined a road part-way along it (T-junction) at %1.", bestProj));
		return true;
	}

	//! The road to follow near pos: not simply the nearest one (a narrow track can sit closer than the real
	//! road), but the one that is close AND runs the same way as the intended direction of travel, with a
	//! penalty for narrow roads. Falls back to the plain nearest road.
	protected BaseRoad FindBestRoad(RoadNetworkManager roadMgr, vector pos, vector towardPos)
	{
		vector hint = towardPos - pos;
		hint[1] = 0;
		bool haveHint = hint.LengthSq() > 1.0;
		if (haveHint)
			hint.Normalize();

		array<BaseRoad> roads = {};
		roadMgr.GetRoadsInAABB(Vector(pos[0] - 20, pos[1] - 30, pos[2] - 20), Vector(pos[0] + 20, pos[1] + 30, pos[2] + 20), roads);

		BaseRoad best = null;
		float bestScore = float.MAX;
		foreach (BaseRoad candidate : roads)
		{
			if (!candidate)
				continue;

			array<vector> pts = {};
			candidate.GetPoints(pts);
			if (pts.Count() < 2)
				continue;

			int seg = FindNearestSegment(pts, pos);
			vector proj = ProjectOnSegment(pos, pts[seg], pts[seg + 1]);
			float dx = pos[0] - proj[0];
			float dz = pos[2] - proj[2];
			float dist = Math.Sqrt(dx * dx + dz * dz);

			float score = dist;
			if (haveHint)
			{
				vector dir = pts[seg + 1] - pts[seg];
				dir[1] = 0;
				if (dir.LengthSq() > 0.0001)
				{
					dir.Normalize();
					score += (1.0 - Math.AbsFloat(dir[0] * hint[0] + dir[2] * hint[2])) * 15.0;
				}
			}
			if (candidate.GetWidth() < 4.0)
				score += 8.0;

			if (score < bestScore)
			{
				bestScore = score;
				best = candidate;
			}
		}

		if (best)
			return best;

		BaseRoad nearestRoad = null;
		float nearestDist = 0;
		roadMgr.GetClosestRoad(pos, nearestRoad, nearestDist);
		return nearestRoad;
	}

	protected bool m_bRoadScanLogged = false;

	//! One-time diagnostic: every road near the first position asked about (the checkpoint), with width,
	//! distance and how well it lines up with the direction of travel. Shows what the road network
	//! around the checkpoint really looks like.
	protected void LogRoadScanOnce(RoadNetworkManager roadMgr, vector pos, vector towardPos)
	{
		if (m_bRoadScanLogged || !m_bDebugLog)
			return;
		m_bRoadScanLogged = true;

		vector hint = towardPos - pos;
		hint[1] = 0;
		if (hint.LengthSq() > 1.0)
			hint.Normalize();

		array<BaseRoad> roads = {};
		roadMgr.GetRoadsInAABB(Vector(pos[0] - 25, pos[1] - 30, pos[2] - 25), Vector(pos[0] + 25, pos[1] + 30, pos[2] + 25), roads);
		DebugLog(string.Format("RoadScan around %1: %2 road(s) within 25 m.", pos, roads.Count()));

		int shown = 0;
		foreach (BaseRoad candidate : roads)
		{
			if (!candidate || shown >= 10)
				continue;

			array<vector> pts = {};
			candidate.GetPoints(pts);
			if (pts.Count() < 2)
				continue;

			shown++;
			int seg = FindNearestSegment(pts, pos);
			vector proj = ProjectOnSegment(pos, pts[seg], pts[seg + 1]);
			float dx = pos[0] - proj[0];
			float dz = pos[2] - proj[2];
			vector dir = pts[seg + 1] - pts[seg];
			dir[1] = 0;
			if (dir.LengthSq() > 0.0001)
				dir.Normalize();
			DebugLog(string.Format("  road %1: width %2 m, %3 point(s), nearest %4 m, alignment %5, first %6, last %7", shown, candidate.GetWidth(), pts.Count(), Math.Sqrt(dx * dx + dz * dz), dir[0] * hint[0] + dir[2] * hint[2], pts[0], pts[pts.Count() - 1]));
		}
	}

	//! Lateral distance (m) of the traffic lane from the road centre line: a quarter of the road width, to the
	//! right of the direction of travel (two-lane road, right-hand traffic).
	protected float GetLaneOffset()
	{
		// A single-lane road has no second lane: traffic drives down the middle.
		if (IsSingleLaneRoad())
			return 0;

		return Math.Clamp(m_fLastLineRoadWidth * 0.25, 1.0, 3.0);
	}

	protected float m_fCheckpointRoadWidth = -1;

	//! Width (m) of the road at the checkpoint itself, read from the road network once.
	protected float GetCheckpointRoadWidth()
	{
		if (m_fCheckpointRoadWidth > 0)
			return m_fCheckpointRoadWidth;

		SCR_AIWorld aiWorld = SCR_AIWorld.Cast(GetGame().GetAIWorld());
		if (!aiWorld)
			return m_fLastLineRoadWidth;

		RoadNetworkManager roadMgr = aiWorld.GetRoadNetworkManager();
		if (!roadMgr)
			return m_fLastLineRoadWidth;

		vector pos = GetOwner().GetOrigin();
		vector toward = pos;
		if (m_DespawnPoint)
			toward = m_DespawnPoint.GetOrigin();

		BaseRoad road = FindBestRoad(roadMgr, pos, toward);
		if (!road)
			return m_fLastLineRoadWidth;

		m_fCheckpointRoadWidth = road.GetWidth();
		DebugLog(string.Format("The road at the checkpoint is %1 m wide (single lane: %2, limit %3 m).", m_fCheckpointRoadWidth, IsSingleLaneRoad(), m_fSingleLaneMaxWidth));
		return m_fCheckpointRoadWidth;
	}

	//! True if the road at the checkpoint is narrower than m_fSingleLaneMaxWidth.
	protected bool IsSingleLaneRoad()
	{
		return GetCheckpointRoadWidth() < m_fSingleLaneMaxWidth;
	}

	//! Resample a polyline through its own points with a Catmull-Rom spline (about every step metres), so a
	//! sparse road line is followed as the curve it is instead of a polygon whose corners get cut.
	protected void SmoothPolyline(array<vector> input, float step, array<vector> output)
	{
		int n = input.Count();
		if (n < 3)
		{
			foreach (vector p : input)
				output.Insert(p);
			return;
		}

		for (int i = 0; i < n - 1; i++)
		{
			vector p1 = input[i];
			vector p2 = input[i + 1];
			vector p0 = p1;
			if (i > 0)
				p0 = input[i - 1];
			else
				p0 = p1 - (p2 - p1);
			vector p3 = p2;
			if (i + 2 < n)
				p3 = input[i + 2];
			else
				p3 = p2 + (p2 - p1);

			float dx = p2[0] - p1[0];
			float dz = p2[2] - p1[2];
			int steps = Math.Max(1, Math.Ceil(Math.Sqrt(dx * dx + dz * dz) / step));

			for (int k = 0; k < steps; k++)
			{
				float kf = k;
				float t = kf / steps;
				float t2 = t * t;
				float t3 = t2 * t;
				vector pt = (p1 * 2.0 + (p2 - p0) * t + (p0 * 2.0 - p1 * 5.0 + p2 * 4.0 - p3) * t2 + (p1 * 3.0 - p0 - p2 * 3.0 + p3) * t3) * 0.5;
				output.Insert(pt);
			}
		}

		output.Insert(input[n - 1]);
	}

	//! Road centre-line polyline: starts at startPos projected onto the nearest road, runs toward towardPos,
	//! and is chained across road pieces until it is at least minLength long (or the road ends).
	//! tryBothWays: build the line both ways along the starting road and keep the one that gets closest to
	//! towardPos. The plain choice (the road direction that points more toward the destination) goes wrong
	//! when the road first leads away and only loops round to the destination later.
	protected bool BuildRoadLine(vector startPos, vector towardPos, float minLength, array<vector> outLine, bool tryBothWays = false)
	{
		if (!tryBothWays)
			return BuildRoadLineDir(startPos, towardPos, minLength, outLine, 0);

		array<vector> lineA = {};
		array<vector> lineB = {};
		bool okA = BuildRoadLineDir(startPos, towardPos, minLength, lineA, 1);
		float widthA = m_fLastLineRoadWidth;
		bool okB = BuildRoadLineDir(startPos, towardPos, minLength, lineB, 2);
		float widthB = m_fLastLineRoadWidth;

		array<vector> chosen = lineA;
		m_fLastLineRoadWidth = widthA;
		if (!okA || (okB && GetLineDistanceTo(lineB, towardPos) < GetLineDistanceTo(lineA, towardPos)))
		{
			chosen = lineB;
			m_fLastLineRoadWidth = widthB;
		}

		if (!okA && !okB)
			return false;

		foreach (vector p : chosen)
			outLine.Insert(p);
		return outLine.Count() >= 2;
	}

	//! Smallest horizontal distance from a polyline to a point.
	protected float GetLineDistanceTo(array<vector> line, vector pos)
	{
		float best = float.MAX;
		for (int i = 0; i < line.Count() - 1; i++)
		{
			vector proj = ProjectOnSegment(pos, line[i], line[i + 1]);
			float dx = pos[0] - proj[0];
			float dz = pos[2] - proj[2];
			float dist = Math.Sqrt(dx * dx + dz * dz);
			if (dist < best)
				best = dist;
		}
		return best;
	}

	//! forceDirection: 0 = the way that points more toward towardPos, 1 = along the road's point order, 2 = against it.
	protected bool BuildRoadLineDir(vector startPos, vector towardPos, float minLength, array<vector> outLine, int forceDirection)
	{
		SCR_AIWorld aiWorld = SCR_AIWorld.Cast(GetGame().GetAIWorld());
		if (!aiWorld)
			return false;

		RoadNetworkManager roadMgr = aiWorld.GetRoadNetworkManager();
		if (!roadMgr)
			return false;

		BaseRoad road = FindBestRoad(roadMgr, startPos, towardPos);
		if (!road)
			return false;

		m_fLastLineRoadWidth = road.GetWidth();

		array<vector> points = {};
		road.GetPoints(points);
		if (points.Count() < 2)
			return false;

		int nearest = FindNearestSegment(points, startPos);
		vector proj = ProjectOnSegment(startPos, points[nearest], points[nearest + 1]);
		outLine.Insert(proj);

		vector segDir = points[nearest + 1] - points[nearest];
		vector toToward = towardPos - startPos;
		bool alongPoints = (segDir[0] * toToward[0] + segDir[2] * toToward[2] >= 0);
		if (forceDirection == 1)
			alongPoints = true;
		else if (forceDirection == 2)
			alongPoints = false;

		if (alongPoints)
		{
			for (int i = nearest + 1; i < points.Count(); i++)
				outLine.Insert(points[i]);
		}
		else
		{
			outLine.Insert(points[nearest]);
			for (int i = nearest - 1; i >= 0; i--)
				outLine.Insert(points[i]);
		}

		// Direction of travel along the chosen road (toward the destination), used when the line itself is
		// too short to tell which way it points.
		vector travelDir = segDir;
		travelDir[1] = 0;
		if (!alongPoints)
			travelDir = travelDir * -1;

		array<vector> visited = {};
		visited.Insert(points[0]);
		visited.Insert(points[points.Count() - 1]);

		int pieces = 1;
		for (int piece = 0; piece < 20; piece++)
		{
			if (GetPolylineLength(outLine) >= minLength)
				break;

			if (!AppendNextRoad(roadMgr, outLine, towardPos, visited, travelDir))
				break;

			pieces++;
		}

		DebugLog(string.Format("RoadLine: %1 point(s), %2 m over %3 road piece(s) (wanted %4 m), first road width %5 m.", outLine.Count(), GetPolylineLength(outLine), pieces, minLength, road.GetWidth()));
		return outLine.Count() >= 2;
	}

	//! Fill run.m_aPath / m_aCum: the road line from the vehicle toward towardPos, shifted sideways by the
	//! vehicle's own lateral offset from the road line (so it keeps the lane the AI had it in).
	protected bool BuildLanePath(IEntity vehicle, vector towardPos, float minLength, EEF_ScriptedDriveRun run)
	{
		vector vehPos = vehicle.GetOrigin();

		array<vector> rawLine = {};
		if (!BuildRoadLine(vehPos, towardPos, minLength, rawLine, true))
			return false;

		return FinishLanePath(rawLine, run);
	}

	//! Turn a raw road line into the run's lane path: smoothed, shifted to the lane, with cumulative distances.
	protected bool FinishLanePath(array<vector> rawLine, EEF_ScriptedDriveRun run)
	{
		if (rawLine.Count() < 2)
			return false;

		// Follow the road as the curve it is, not a polygon through its sparse points.
		array<vector> line = {};
		SmoothPolyline(rawLine, 2.5, line);
		if (line.Count() < 2)
			return false;

		vector proj = line[0];

		// Right-hand normal (Enfusion: forward +Z, right +X => right = (dz, 0, -dx)).
		vector firstDir = line[1] - line[0];
		firstDir[1] = 0;
		firstDir.Normalize();
		// Every car drives the same lane (right of the road centre line, a quarter of the road width out),
		// so a queue lines up nose to tail instead of wobbling with each car's starting offset. A car that
		// starts off-lane steers smoothly onto it.
		float laneOffset = GetLaneOffset();

		for (int i = 0; i < line.Count(); i++)
		{
			vector dir;
			if (i < line.Count() - 1)
				dir = line[i + 1] - line[i];
			else
				dir = line[i] - line[i - 1];
			dir[1] = 0;
			if (dir.LengthSq() < 0.0001)
				dir = firstDir;
			dir.Normalize();
			vector right = Vector(dir[2], 0, -dir[0]);
			run.m_aPath.Insert(line[i] + right * laneOffset);
		}

		run.m_aCum.Insert(0.0);
		for (int i = 1; i < run.m_aPath.Count(); i++)
		{
			float dx = run.m_aPath[i][0] - run.m_aPath[i - 1][0];
			float dz = run.m_aPath[i][2] - run.m_aPath[i - 1][2];
			run.m_aCum.Insert(run.m_aCum[i - 1] + Math.Sqrt(dx * dx + dz * dz));
		}

		float pathLength = run.m_aCum[run.m_aCum.Count() - 1];
		DebugLog(string.Format("Rails: lane path of %1 point(s), %2 m, lane offset %3 m from the road line.", run.m_aPath.Count(), pathLength, laneOffset));

		// A path of (almost) no length means no usable road was found from here - refuse it rather than
		// letting a drive "finish" instantly where it stands.
		if (pathLength < 1.0)
		{
			DebugLog("Rails: that path is too short to drive - rejecting it.");
			return false;
		}

		return true;
	}

	//! Arc length and sideways distance of a world position projected onto a run's path.
	protected void ProjectOnPath(EEF_ScriptedDriveRun run, vector pos, out float arc, out float lateral)
	{
		arc = 0;
		lateral = float.MAX;

		for (int i = 0; i < run.m_aPath.Count() - 1; i++)
		{
			vector proj = ProjectOnSegment(pos, run.m_aPath[i], run.m_aPath[i + 1]);
			float dx = pos[0] - proj[0];
			float dz = pos[2] - proj[2];
			float dist = Math.Sqrt(dx * dx + dz * dz);
			if (dist < lateral)
			{
				lateral = dist;
				float sx = proj[0] - run.m_aPath[i][0];
				float sz = proj[2] - run.m_aPath[i][2];
				arc = run.m_aCum[i] + Math.Sqrt(sx * sx + sz * sz);
			}
		}
	}

	//! Distance (centre to centre, along the path) to the nearest other checkpoint vehicle ahead of this
	//! run's vehicle on its path, or float.MAX if there is none.
	protected float GetGapToVehicleAhead(EEF_ScriptedDriveRun run)
	{
		float best = float.MAX;
		foreach (EEF_CheckpointVehicleState other : m_aVehicles)
		{
			if (!other || other == run.m_State || !other.m_Vehicle)
				continue;

			float arc, lateral;
			ProjectOnPath(run, other.m_Vehicle.GetOrigin(), arc, lateral);
			if (lateral > 3.5)
				continue;

			// A detained car parked at the roadside is not in anyone's way (only while it is still moving over).
			if (other.m_eState == EEF_ECheckpointVehicleState.DETAINED && !other.m_bOnRails && lateral > 1.8)
				continue;

			float gap = arc - run.m_fProgress;
			if (gap > 1.0 && gap < best)
				best = gap;
		}
		return best;
	}

	protected ref array<IEntity> m_aObstacleHits = new array<IEntity>();

	//! World-query callback: remember vehicles.
	protected bool QueryObstacleCallback(IEntity entity)
	{
		if (Vehicle.Cast(entity))
			m_aObstacleHits.Insert(entity);

		return true;
	}

	//! Look for a vehicle on the road ahead of a fleeing car and set the sideways shift that takes it round
	//! (to the side of the obstacle with more room: the opposing lane unless the obstacle already sits there).
	protected void UpdateFleeBypass(EEF_ScriptedDriveRun run, IEntity vehicle, int seg)
	{
		int segCount = run.m_aPath.Count() - 1;
		if (seg < 0 || seg >= segCount)
			return;

		vector pathDir = run.m_aPath[seg + 1] - run.m_aPath[seg];
		pathDir[1] = 0;
		if (pathDir.LengthSq() < 0.0001)
			return;
		pathDir.Normalize();
		vector leftDir = Vector(-pathDir[2], 0, pathDir[0]);

		// Search a sphere around a point 11 m ahead along the path.
		vector centre = GetPathPoint(run, run.m_fProgress + 11.0);
		m_aObstacleHits.Clear();
		GetGame().GetWorld().QueryEntitiesBySphere(centre, 11.0, QueryObstacleCallback);

		float bestArc = float.MAX;
		float bestSide = 0;
		foreach (IEntity hit : m_aObstacleHits)
		{
			if (!hit || hit == vehicle)
				continue;

			float arc, lateral;
			ProjectOnPath(run, hit.GetOrigin(), arc, lateral);
			float ahead = arc - run.m_fProgress;
			if (ahead < 1.5 || ahead > 22.0 || lateral > 3.2)
				continue;

			if (ahead < bestArc)
			{
				bestArc = ahead;
				// Signed: + = the obstacle is left of the path.
				vector rel = hit.GetOrigin() - GetPathPoint(run, arc);
				bestSide = rel[0] * leftDir[0] + rel[2] * leftDir[2];
			}
		}

		if (bestArc == float.MAX)
		{
			// Nothing in the way: ease back toward the path.
			run.m_fBypassOffset = run.m_fBypassOffset * 0.8;
			if (Math.AbsFloat(run.m_fBypassOffset) < 0.05)
				run.m_fBypassOffset = 0;
			return;
		}

		float offset;
		if (bestSide > 0.6)
			offset = bestSide - 3.6;		// obstacle sits left: go right of it
		else
			offset = bestSide + 3.6;		// centre or right: go left, round it
		run.m_fBypassOffset = Math.Clamp(offset, -6.0, 6.0);
	}

	//! Remove any running drive for this vehicle without side effects (a new one replaces it).
	protected void RemoveRunForState(EEF_CheckpointVehicleState state)
	{
		for (int i = m_aScriptedRuns.Count() - 1; i >= 0; i--)
		{
			if (m_aScriptedRuns[i] && m_aScriptedRuns[i].m_State == state)
			{
				DeleteDebugGhost(m_aScriptedRuns[i]);
				m_aScriptedRuns.Remove(i);
			}
		}
		UpdateEventMask();
	}

	//! Take a vehicle off the AI and drive it from script to its queue gate. Called when it enters the
	//! zone and again whenever the queue advances. False (caller falls back to the AI) if anything
	//! needed is missing.
	protected bool StartScriptedApproach(EEF_CheckpointVehicleState state)
	{
		if (!state || !state.m_Vehicle)
			return false;

		EEF_CheckpointQueueGate gate = GetQueueGate(state.m_iQueueSlot);
		if (!gate)
			return false;

		CarControllerComponent controller = CarControllerComponent.Cast(state.m_Vehicle.FindComponent(CarControllerComponent));
		if (!controller)
			return false;

		VehicleWheeledSimulation sim = controller.GetSimulation();
		if (!sim)
			return false;

		// A car the script is already driving keeps rolling: it must not be put through the physics re-entry below,
		// which kills its speed (that was the stop in place at the handover from the lead-up drive).
		bool wasRolling = state.m_bOnRails && FindRunForState(state) != null;

		RemoveRunForState(state);

		EEF_ScriptedDriveRun run = new EEF_ScriptedDriveRun();
		run.m_State = state;
		run.m_Sim = sim;
		run.m_iKind = SCRIPTED_KIND_APPROACH;

		vector checkpointPos = GetOwner().GetOrigin();
		float minLength = vector.Distance(state.m_Vehicle.GetOrigin(), checkpointPos) + 25.0;
		if (!BuildLanePath(state.m_Vehicle, checkpointPos, minLength, run))
		{
			DebugLog("Rails: could not build an approach path - will retry.");
			return false;
		}

		float arc, lateral;
		ProjectOnPath(run, gate.m_Point, arc, lateral);
		run.m_fStopArc = arc;

		// AI off (idempotent if the vehicle is already held), body in simulation.
		if (!state.m_bHeld)
		{
			SetGroupAIActive(state, false);
			state.m_bHeld = true;
		}
		state.m_bOnRails = true;

		if (!wasRolling)
		{
			SCR_PhysicsHelper.ChangeSimulationState(state.m_Vehicle, SimulationState.SIMULATION);
			controller.SetPersistentHandBrake(false);
			if (!sim.EngineIsOn())
			{
				sim.EngineStart();
				controller.StartEngine();
			}
		}

		m_aScriptedRuns.Insert(run);
		UpdateEventMask();

		SpawnDebugGhost(run);

		DebugLog(string.Format("Rails: APPROACH slot %1 - stop at %2 m along the path (gate %3 m off the path).", state.m_iQueueSlot, run.m_fStopArc, lateral));
		return true;
	}

	//! Drive a freshly seated car from the Spawn marker toward the checkpoint from script, along its validated
	//! route (the road line that was checked and drawn at start), so it can never take a wrong turn. The run
	//! ends when the queue logic takes the car over (EnterQueue). False (caller falls back to the AI) if the
	//! route or the vehicle controller is missing.
	protected bool StartScriptedTransit(EEF_CheckpointVehicleState state)
	{
		if (!state || !state.m_Vehicle)
			return false;

		EEF_CheckpointRoute route = GetRoute(state.m_iRoute);
		if (!route || route.m_aLine.Count() < 2)
			return false;

		CarControllerComponent controller = CarControllerComponent.Cast(state.m_Vehicle.FindComponent(CarControllerComponent));
		if (!controller)
			return false;

		VehicleWheeledSimulation sim = controller.GetSimulation();
		if (!sim)
			return false;

		RemoveRunForState(state);

		EEF_ScriptedDriveRun run = new EEF_ScriptedDriveRun();
		run.m_State = state;
		run.m_Sim = sim;
		run.m_iKind = SCRIPTED_KIND_TRANSIT;

		// The route line from the car's position onward.
		vector vehPos = state.m_Vehicle.GetOrigin();
		int seg = FindNearestSegment(route.m_aLine, vehPos);
		array<vector> rawLine = {};
		rawLine.Insert(ProjectOnSegment(vehPos, route.m_aLine[seg], route.m_aLine[seg + 1]));
		for (int i = seg + 1; i < route.m_aLine.Count(); i++)
			rawLine.Insert(route.m_aLine[i]);

		if (!FinishLanePath(rawLine, run))
		{
			DebugLog("Rails: could not build the transit path from the route.");
			return false;
		}

		// Where the checkpoint sits on this path: the car slows down toward its takeover point from here.
		float arc, lateral;
		ProjectOnPath(run, GetOwner().GetOrigin(), arc, lateral);
		run.m_fStopArc = arc;

		// The crew's AI stays off for good; the script drives.
		SetGroupAIActive(state, false);
		state.m_bHeld = true;
		state.m_bOnRails = true;

		SCR_PhysicsHelper.ChangeSimulationState(state.m_Vehicle, SimulationState.SIMULATION);
		controller.SetPersistentHandBrake(false);
		// A freshly spawned car never had its engine started by the AI: start it, and keep checking in the step.
		controller.StartEngine();
		sim.EngineStart();
		DebugLog(string.Format("Rails: TRANSIT engine on=%1 rpm=%2 gear=%3 gears=%4/%5.", sim.EngineIsOn(), sim.EngineGetRPM(), sim.GetGear(), sim.GearboxForwardGearsCount(), sim.GearboxGearsCount()));

		m_aScriptedRuns.Insert(run);
		UpdateEventMask();
		SpawnDebugGhost(run);

		DebugLog(string.Format("Rails: TRANSIT from marker %1 - %2 m of path, the checkpoint is %3 m along it, road speed %4 km/h.", state.m_iRoute, run.m_aCum[run.m_aCum.Count() - 1], run.m_fStopArc, m_fRoadSpeedKmh));
		return true;
	}

	//! Start a scripted departure for a held vehicle. Returns false (caller falls back to the normal
	//! AI release) if anything needed is missing.
	protected bool StartScriptedDeparture(EEF_CheckpointVehicleState state)
	{
		if (!state || !state.m_Vehicle || !state.m_bHeld || !m_DespawnPoint)
			return false;

		CarControllerComponent controller = CarControllerComponent.Cast(state.m_Vehicle.FindComponent(CarControllerComponent));
		if (!controller)
			return false;

		VehicleWheeledSimulation sim = controller.GetSimulation();
		if (!sim)
			return false;

		RemoveRunForState(state);

		EEF_ScriptedDriveRun run = new EEF_ScriptedDriveRun();
		run.m_State = state;
		run.m_Sim = sim;
		run.m_iKind = SCRIPTED_KIND_DEPARTURE;
		run.m_bFlee = state.m_bFleeing;

		vector despawnPos = m_DespawnPoint.GetOrigin();
		float minLength = vector.Distance(state.m_Vehicle.GetOrigin(), despawnPos) + 40.0;
		if (m_fScriptedDriveLength > 0)
			minLength = m_fScriptedDriveLength + 10.0;

		if (!BuildLanePath(state.m_Vehicle, despawnPos, minLength, run))
		{
			DebugLog("Rails: could not build a departure path - falling back to the AI release.");
			return false;
		}

		state.m_bOnRails = true;

		// A cleared car still counted as queued: note where the stop line is on this path so it can be
		// taken out of the queue the moment it drives past it.
		run.m_fStopArc = -1;
		if (state.m_iQueueSlot >= 0)
		{
			EEF_CheckpointQueueGate frontGate = GetQueueGate(0);
			if (frontGate)
			{
				float stopArc, stopLateral;
				ProjectOnPath(run, frontGate.m_Point, stopArc, stopLateral);
				run.m_fStopArc = stopArc;
			}
		}

		// Body back into simulation, AI stays OFF (it was switched off at the hold).
		SCR_PhysicsHelper.ChangeSimulationState(state.m_Vehicle, SimulationState.SIMULATION);
		controller.SetPersistentHandBrake(false);
		if (!sim.EngineIsOn())
		{
			sim.EngineStart();
			controller.StartEngine();
		}

		m_aScriptedRuns.Insert(run);
		UpdateEventMask();

		SpawnDebugGhost(run);

		DebugLog(string.Format("Rails: DEPARTURE - driving the road to the despawn point at %1 km/h (hand back to the AI after %2 m, 0 = never).", m_fCruiseSpeedKmh, m_fScriptedDriveLength));
		return true;
	}

	//! The departure path could not be built at release time. Try again every second (the car stays held,
	//! AI off); give up and hand it to the AI only after 5 tries.
	protected void RetryScriptedDeparture(EEF_CheckpointVehicleState state, int attempt)
	{
		if (!state || !state.m_Vehicle || m_aVehicles.Find(state) == -1)
			return;

		// Already moving (or no longer waiting to depart): nothing to do.
		if (state.m_eState != EEF_ECheckpointVehicleState.DEPARTING || !state.m_bHeld)
			return;

		if (StartScriptedDeparture(state))
		{
			DebugLog(string.Format("Rails: departure path found on retry %1.", attempt));
			return;
		}

		if (attempt >= 5)
		{
			DebugLog("Rails: still no departure path after 5 tries - handing the car to the AI.");
			ResumeVehicle(state);
			ApplyCruiseSpeed(state, GetDepartureAISpeed(state));
			return;
		}

		GetGame().GetCallqueue().CallLater(RetryScriptedDeparture, 1000, false, state, attempt + 1);
	}

	//! One fixed (physics-rate) step of every running scripted drive.
	protected void StepScriptedRuns(float timeSlice)
	{
		if (m_aScriptedRuns.IsEmpty())
			return;

		array<EEF_ScriptedDriveRun> finished = {};
		array<EEF_ScriptedDriveRun> driverDown = {};
		foreach (EEF_ScriptedDriveRun run : m_aScriptedRuns)
		{
			if (!run || !run.m_State || !run.m_State.m_Vehicle || !run.m_Sim || m_aVehicles.Find(run.m_State) == -1)
			{
				finished.Insert(run);
				continue;
			}

			// Driver killed or knocked out: the script stops driving (see HandleDriverDown).
			if (IsDriverDown(run.m_State.m_Vehicle))
			{
				driverDown.Insert(run);
				continue;
			}

			if (StepScriptedRun(run, timeSlice))
				finished.Insert(run);
		}

		foreach (EEF_ScriptedDriveRun downRun : driverDown)
		{
			DeleteDebugGhost(downRun);
			m_aScriptedRuns.RemoveItem(downRun);
			if (downRun.m_Sim)
			{
				downRun.m_Sim.SetThrottle(0);
				downRun.m_Sim.SetBreak(0, false);
				downRun.m_Sim.SetSteering(0);
			}
		}
		if (!driverDown.IsEmpty())
			UpdateEventMask();

		foreach (EEF_ScriptedDriveRun downRun2 : driverDown)
		{
			if (downRun2 && downRun2.m_State)
				HandleDriverDown(downRun2.m_State);
		}

		foreach (EEF_ScriptedDriveRun doneRun : finished)
			EndScriptedRun(doneRun);

		ProcessStopLinePassed();
	}

	//! Speed (km/h) along the direction the car is pointing: negative when it is rolling backwards.
	protected float GetForwardSpeedKmh(IEntity vehicle)
	{
		Physics physics = vehicle.GetPhysics();
		if (!physics)
			return 0;

		vector forward = vehicle.VectorToParent(Vector(0, 0, 1));
		vector velocity = physics.GetVelocity();
		return (velocity[0] * forward[0] + velocity[1] * forward[1] + velocity[2] * forward[2]) * 3.6;
	}

	//! Rise over run (positive = uphill) of the path from the car's progress to the point 'distance' metres on.
	protected float GetPathGradientAhead(EEF_ScriptedDriveRun run, float distance)
	{
		int n = run.m_aPath.Count();
		int i = Math.Max(run.m_iSeg, 0);
		if (i >= n - 1)
			return 0;

		float target = run.m_fProgress + distance;
		while (i < n - 1 && run.m_aCum[i] < target)
			i++;

		vector from = run.m_aPath[Math.Max(run.m_iSeg, 0)];
		vector to = run.m_aPath[i];
		float dx = to[0] - from[0];
		float dz = to[2] - from[2];
		float run2d = Math.Sqrt(dx * dx + dz * dz);
		if (run2d < 5.0)
			return 0;

		return (to[1] - from[1]) / run2d;
	}

	//! Automatic gearbox for a script-driven car at road speed: up a gear when the revs get near peak power, down
	//! when they fall too low. Gear index 2 is first (0 = reverse, 1 = neutral), the last forward gear is
	//! 1 + the number of forward gears. A short cooldown stops it hunting between gears.
	protected void UpdateScriptedGear(EEF_ScriptedDriveRun run, float speedKmh, float timeSlice)
	{
		VehicleWheeledSimulation sim = run.m_Sim;
		run.m_fGearTimer -= timeSlice;
		run.m_fDownTimer -= timeSlice;

		int gear = sim.GetGear();
		if (gear < 2)
		{
			sim.SetGear(2);	// neutral or reverse while driving forward: first gear
			run.m_fGearTimer = 0.8;
			return;
		}

		// Downshifts are never held back for long (an engine dropping out of its torque band must act now); only
		// upshifts wait for the cooldown.
		bool upAllowed = run.m_fGearTimer <= 0;

		int topGear = Math.Min(1 + sim.GearboxForwardGearsCount(), sim.GearboxGearsCount() - 1);
		float rpm = sim.EngineGetRPM();
		float peakPower = sim.EngineGetRPMPeakPower();
		float peakTorque = sim.EngineGetRPMPeakTorque();
		// The steeper of the slope under the car and the road gradient well ahead (about 5 s of driving, at least
		// 45 m): shift for the hill before it bites.
		float aheadDist = Math.Clamp(speedKmh / 3.6 * 5.0, 45.0, 110.0);
		float slope = Math.Max(run.m_fSlope, GetPathGradientAhead(run, aheadDist));

		// Going up a hill the engine needs its torque band, not its power band: change up later and drop a gear as
		// soon as it is labouring (below the torque peak, falling behind the target speed).
		float upRpm = peakPower * 0.9;
		if (slope > 0.04)
			upRpm = peakPower * 1.0;
		bool behind = (run.m_fTargetSpeed - speedKmh > 3.0) && run.m_fThrottle > 0.6;
		float downRpm = Math.Max(sim.EngineGetRPMIdle() * 1.8, peakTorque * 0.55);
		if (behind || slope > 0.04)
			downRpm = Math.Max(downRpm, peakTorque * 1.1);
		if (behind && slope > 0.08)
			downRpm = Math.Max(downRpm, peakTorque * 1.4);

		// Never sit on the rev limiter: above the power peak the engine gains nothing, so the next gear is always
		// better (a weak small engine at full throttle on a hill would otherwise stay in first at its redline).
		bool overRevving = rpm > peakPower * 1.02;

		run.m_fSinceUpshift += timeSlice;
		run.m_fHuntBlock -= timeSlice;

		if (rpm < downRpm && gear > 2 && run.m_fDownTimer <= 0 && !overRevving)
		{
			sim.SetGear(gear - 1);
			run.m_fGearTimer = 2.0;	// no upshift straight after
			run.m_fDownTimer = 0.4;
			// Dropping straight back after an upshift means the next gear is too tall for this hill: stay down a while.
			if (run.m_fSinceUpshift < 4.0)
				run.m_fHuntBlock = 10.0;
			DebugLog(string.Format("Rails: gear %1 -> %2 at %3 rpm, %4 km/h, slope %5.", gear, gear - 1, rpm, speedKmh, slope));
		}
		else if (gear < topGear && run.m_fThrottle > 0.1 && ((upAllowed && rpm > upRpm && run.m_fHuntBlock <= 0) || (overRevving && run.m_fGearTimer <= 1.0)))
		{
			sim.SetGear(gear + 1);
			run.m_fGearTimer = 2.0;
			run.m_fSinceUpshift = 0;
			run.m_fDownTimer = 1.0;	// no downshift straight after (the revs drop with the new ratio)
			DebugLog(string.Format("Rails: gear %1 -> %2 at %3 rpm, %4 km/h, slope %5 (top gear %6).", gear, gear + 1, rpm, speedKmh, slope, topGear));
		}
	}

	//! Safe speed (m/s) at every point of a run's path from its curvature: sqrt(grip x radius) from the turn
	//! across the neighbouring points, never below a crawl. Computed once per path.
	protected void ComputeCornerSpeeds(EEF_ScriptedDriveRun run)
	{
		run.m_aCornerSpeed.Clear();
		int n = run.m_aPath.Count();
		for (int i = 0; i < n; i++)
		{
			float v = 99.0;
			if (i >= 1 && i < n - 1)
			{
				int a = Math.Max(i - 2, 0);
				int c = Math.Min(i + 2, n - 1);
				vector d1 = run.m_aPath[i] - run.m_aPath[a];
				vector d2 = run.m_aPath[c] - run.m_aPath[i];
				d1[1] = 0;
				d2[1] = 0;
				float l1 = d1.Length();
				float l2 = d2.Length();
				if (l1 > 0.1 && l2 > 0.1)
				{
					float cosA = (d1[0] * d2[0] + d1[2] * d2[2]) / (l1 * l2);
					float angle = Math.Acos(Math.Clamp(cosA, -1, 1));
					if (angle > 0.03)
						v = Math.Sqrt(Math.Max(m_fCornerGrip, 0.5) * (l1 + l2) * 0.5 / angle);
				}
			}
			run.m_aCornerSpeed.Insert(Math.Max(v, 2.5));
		}
	}

	//! Highest speed (km/h) the car may have right now so that it can still brake (about 2 m/s2) to the safe
	//! speed of every bend within the look-ahead range, which grows with speed (the braking distance plus a margin).
	protected float GetCornerSpeedLimit(EEF_ScriptedDriveRun run, float speedKmh)
	{
		if (run.m_aCornerSpeed.Count() != run.m_aPath.Count())
			ComputeCornerSpeeds(run);

		// Plan for about 1.5 m/s2 of braking (the brake is applied in proportion, so it lags the plan), and look
		// at least 60 m ahead.
		float decel = 1.5;
		float speedMs = speedKmh / 3.6;
		float range = Math.Max(80.0, speedMs * speedMs / (2.0 * decel) * 1.5 + 40.0);

		float best = 99.0;
		int bestIdx = -1;
		int lastIdx = Math.Max(run.m_iSeg, 0);
		int n = run.m_aPath.Count();
		for (int i = Math.Max(run.m_iSeg, 0); i < n; i++)
		{
			float d = run.m_aCum[i] - run.m_fProgress;
			if (d > range)
				break;
			if (d < 0)
				d = 0;

			lastIdx = i;
			float v = Math.Sqrt(run.m_aCornerSpeed[i] * run.m_aCornerSpeed[i] + 2.0 * decel * d);
			if (v < best)
			{
				best = v;
				bestIdx = i;
			}
		}

		run.m_vHorizonPoint = run.m_aPath[lastIdx];
		run.m_fLimitSpeed = best * 3.6;
		// Only a bend that really limits the speed (below the road speed) is marked.
		if (bestIdx >= 0 && best * 3.6 < m_fRoadSpeedKmh - 1.0)
			run.m_vLimitPoint = run.m_aPath[bestIdx];
		else
			run.m_vLimitPoint = vector.Zero;

		return best * 3.6;
	}

	//! Pure-pursuit steering + speed profile for one run. Returns true when the run is complete.
	protected bool StepScriptedRun(EEF_ScriptedDriveRun run, float timeSlice)
	{
		run.m_fElapsed += timeSlice;

		IEntity vehicle = run.m_State.m_Vehicle;
		vector pos = vehicle.GetOrigin();
		int segCount = run.m_aPath.Count() - 1;

		// Progress: nearest point on the next few path segments.
		int bestSeg = run.m_iSeg;
		float bestDistSq = -1;
		vector bestProj = run.m_aPath[run.m_iSeg];
		int last = Math.Min(run.m_iSeg + 4, segCount);
		for (int i = run.m_iSeg; i < last; i++)
		{
			vector proj = ProjectOnSegment(pos, run.m_aPath[i], run.m_aPath[i + 1]);
			float dx = pos[0] - proj[0];
			float dz = pos[2] - proj[2];
			float distSq = dx * dx + dz * dz;
			if (bestDistSq < 0 || distSq < bestDistSq)
			{
				bestDistSq = distSq;
				bestSeg = i;
				bestProj = proj;
			}
		}
		run.m_iSeg = bestSeg;
		float sx = bestProj[0] - run.m_aPath[bestSeg][0];
		float sz = bestProj[2] - run.m_aPath[bestSeg][2];
		run.m_fProgress = run.m_aCum[bestSeg] + Math.Sqrt(sx * sx + sz * sz);

		float pathLength = run.m_aCum[run.m_aCum.Count() - 1];
		float speedKmh = run.m_Sim.GetSpeedKmh();

		// "approach" = a run that ends in a stop at m_fStopArc (queue gate or pull-over spot); "transit" = spawn to
		// the checkpoint zone; "fast" = a run at road speed (transit, or a normal departure - not a run-away launch).
		bool transit = (run.m_iKind == SCRIPTED_KIND_TRANSIT);
		bool approach = (run.m_iKind == SCRIPTED_KIND_APPROACH || run.m_iKind == SCRIPTED_KIND_PULLOVER);
		bool fast = (transit || (!approach && !run.m_bFlee));

		// Nobody shifts gears with the AI off. Slow runs stay in first (gear index 2) so slowing down never lugs the
		// engine in a high gear left over from before; road-speed runs shift from the engine revs.
		vector forwardWorld = vehicle.VectorToParent(Vector(0, 0, 1));
		run.m_fSlope = forwardWorld[1];
		run.m_fFwdSpeedKmh = GetForwardSpeedKmh(vehicle);

		// Launch: pulling away under load (a heavy truck on a hill) the clutch has to slip, or the engine is dragged
		// down to a stall. Slip it while the revs are below the torque peak, close it again as they recover.
		float engineRpm = run.m_Sim.EngineGetRPM();
		float torqueRpm = run.m_Sim.EngineGetRPMPeakTorque();
		if (speedKmh < 12.0 && run.m_fThrottle > 0.3 && run.m_Sim.GetGear() >= 2)
		{
			if (engineRpm < torqueRpm * 0.9)
				run.m_fClutch -= 1.5 * timeSlice;
			else if (engineRpm > torqueRpm * 1.3)
				run.m_fClutch += 1.0 * timeSlice;
			run.m_fClutch = Math.Clamp(run.m_fClutch, 0.4, 1.0);
		}
		else
			run.m_fClutch = Math.Min(run.m_fClutch + 2.0 * timeSlice, 1.0);

		if (fast && m_bScriptShiftGears)
			UpdateScriptedGear(run, speedKmh, timeSlice);
		else if (speedKmh < 16.0 && run.m_Sim.GetGear() > 2)
			run.m_Sim.SetGear(2);

		// Engine off (stalled, or never started): start it again once a second rather than driving a dead car.
		if (!run.m_Sim.EngineIsOn() && run.m_fElapsed - run.m_fLastEngineTry >= 1.0)
		{
			run.m_fLastEngineTry = run.m_fElapsed;
			CarControllerComponent engineController = CarControllerComponent.Cast(vehicle.FindComponent(CarControllerComponent));
			if (engineController)
				engineController.StartEngine();
			run.m_Sim.EngineStart();
			DebugLog("Rails: engine was off - starting it.");
		}

		float cruise = m_fCruiseSpeedKmh;
		if (run.m_bFlee)
			cruise = m_fFleeLaunchSpeedKmh;
		float stopRemaining = float.MAX;

		if (approach)
		{
			cruise = m_fCruiseSpeedKmh;
			stopRemaining = run.m_fStopArc - run.m_fProgress;

			if (stopRemaining <= 0.7 && speedKmh < 1.0)
			{
				DebugLog(string.Format("Rails: APPROACH arrived - slot %1, stopped %2 m from its gate after %3 s.", run.m_State.m_iQueueSlot, stopRemaining, run.m_fElapsed));
				return true;
			}

			if (run.m_fProgress >= pathLength - 1.0)
			{
				DebugLog("Rails: APPROACH ran out of road before the gate - stopping here.");
				return true;
			}
		}
		else if (transit)
		{
			cruise = m_fRoadSpeedKmh;

			if (run.m_fProgress >= pathLength - 1.0)
			{
				DebugLog("Rails: TRANSIT ran out of road before the checkpoint zone - handing the car to the AI.");
				return true;
			}
		}
		else
		{
			// A normal departure crawls clear of the roadblock pieces, then speeds up to road speed.
			if (!run.m_bFlee)
			{
				cruise = m_fCruiseSpeedKmh;
				if (run.m_fProgress >= DEPARTURE_CLEAR_DISTANCE)
					cruise = m_fRoadSpeedKmh;
			}

			// A cleared car still counted as queued leaves the queue once it is past the stop line.
			if (run.m_fStopArc >= 0 && run.m_State.m_iQueueSlot >= 0 && run.m_fProgress >= run.m_fStopArc && m_aPassedStopLine.Find(run.m_State) == -1)
				m_aPassedStopLine.Insert(run.m_State);

			if (run.m_bFlee && run.m_fProgress >= m_fFleeLaunchLength)
			{
				DebugLog(string.Format("Rails: FLEE launch done at %1 km/h - the AI takes over at %2 km/h.", speedKmh, m_fFleeSpeedKmh));
				return true;
			}

			if ((m_fScriptedDriveLength > 0 && run.m_fProgress >= m_fScriptedDriveLength) || run.m_fProgress >= pathLength - 1.0)
			{
				DebugLog(string.Format("Rails: DEPARTURE done after %1 m (%2 s), speed %3 km/h, %4 m off the lane line - handing back to the AI.", run.m_fProgress, run.m_fElapsed, speedKmh, Math.Sqrt(bestDistSq)));
				return true;
			}
		}

		// Safety: stuck or taking far too long. (An approach may legitimately wait behind a car ahead.)
		float stuckLimit = 6.0;
		if (approach || transit)
			stuckLimit = 25.0;

		// Waiting behind another car is not "stuck" (m_fAheadGap is from the previous step).
		float blockedGap = Math.Clamp(m_fQueueSlotSpacing - 1.0, 4.5, 7.0) + 1.0;
		if (run.m_fElapsed > 4.0 && speedKmh < 0.7 && run.m_fAheadGap > blockedGap)
			run.m_fStuckTime += timeSlice;
		else
			run.m_fStuckTime = 0;

		// (A transit may wait a long time behind the queue, so it has no overall time limit; a long departure at
		// road speed needs more than the 3 minutes of a short move.)
		float timeLimit = 180.0;
		if (transit || fast)
			timeLimit = 1800.0;

		bool stuckStill = run.m_fStuckTime > stuckLimit;
		bool noProgress = run.m_fNoProgressTime > LAUNCH_GIVE_UP;
		if (stuckStill || noProgress || run.m_fElapsed > timeLimit)
		{
			run.m_bStalled = stuckStill || noProgress;
			if (run.m_bStalled)
				LogStuckBlockers(vehicle);
			DebugLog(string.Format("Rails: ABORT (stuck %1 s, no progress %2 s, elapsed %3 s, progress %4 m).", run.m_fStuckTime, run.m_fNoProgressTime, run.m_fElapsed, run.m_fProgress));
			return true;
		}

		// Look-ahead point on the path.
		float speedMs = speedKmh / 3.6;
		float lookahead = Math.Clamp(3.0 + speedMs * 1.0, 4.0, 16.0);
		float targetArc = Math.Min(run.m_fProgress + lookahead, pathLength);
		int seg = bestSeg;
		while (seg < segCount - 1 && run.m_aCum[seg + 1] < targetArc)
			seg++;
		float segLen = run.m_aCum[seg + 1] - run.m_aCum[seg];
		float t = 0;
		if (segLen > 0.001)
			t = Math.Clamp((targetArc - run.m_aCum[seg]) / segLen, 0, 1);
		vector target = run.m_aPath[seg] + (run.m_aPath[seg + 1] - run.m_aPath[seg]) * t;

		// A car running away steers round any vehicle blocking its way (opposing lane or verge).
		bool bypassing = false;
		if (run.m_bFlee)
		{
			if (run.m_fElapsed >= run.m_fNextObstacleCheck)
			{
				run.m_fNextObstacleCheck = run.m_fElapsed + 0.2;
				UpdateFleeBypass(run, vehicle, bestSeg);
			}

			if (Math.AbsFloat(run.m_fBypassOffset) > 0.01)
			{
				bypassing = true;
				vector pathDir = run.m_aPath[seg + 1] - run.m_aPath[seg];
				pathDir[1] = 0;
				if (pathDir.LengthSq() > 0.0001)
				{
					pathDir.Normalize();
					vector leftDir = Vector(-pathDir[2], 0, pathDir[0]);
					target = target + leftDir * run.m_fBypassOffset;
				}
			}
		}

		// Steering: heading error to the look-ahead point. Local +X is right, +Z is forward, and a
		// positive steering input turns right (measured in the proof-of-life test).
		vector localTarget = vehicle.CoordToLocal(target);
		float headingError = Math.Atan2(localTarget[0], localTarget[2]);
		run.m_fSteering = Math.Clamp(m_fSteerGain * headingError, -1.0, 1.0);

		// Target speed: cruise, a smooth stop at the gate, slower in turns, and room behind the car ahead.
		float targetSpeed = cruise;
		if (approach)
		{
			float brakeDist = Math.Max(stopRemaining - 0.4, 0);
			float stopSpeed = Math.Sqrt(2.0 * 1.4 * brakeDist) * 3.6;
			if (stopRemaining > 1.2)
				stopSpeed = Math.Max(stopSpeed, 3.0);
			targetSpeed = Math.Min(targetSpeed, stopSpeed);
			if (stopRemaining <= 0.35)
				targetSpeed = 0;
		}
		targetSpeed = targetSpeed * (1.0 - 0.5 * Math.Clamp(Math.AbsFloat(headingError) / 0.8, 0, 1));

		if (fast)
		{
			// Slow for the bends ahead (the path's curvature, with room to brake).
			targetSpeed = Math.Min(targetSpeed, GetCornerSpeedLimit(run, speedKmh));

			// A transit eases down to approach speed by the time it reaches the queue's takeover point.
			if (transit)
			{
				float toTakeover = Math.Max(run.m_fStopArc - GetTakeoverDistance() - run.m_fProgress, 0);
				float approachMs = m_fCruiseSpeedKmh / 3.6;
				float slowSpeed = Math.Sqrt(approachMs * approachMs + 2.0 * 2.0 * toTakeover) * 3.6;
				targetSpeed = Math.Min(targetSpeed, slowSpeed);
			}
		}

		float aheadGap = GetGapToVehicleAhead(run);
		// Following distance (centre to centre) scales with the slot spacing, so a tight queue (e.g. 6 m)
		// is not blocked by a rule meant for a loose one: stop just inside a slot, creep a few metres out.
		float stopGap = Math.Clamp(m_fQueueSlotSpacing - 1.0, 4.5, 7.0);
		if (bypassing)
		{
			// Going round the blockage: slow, but never stop for the car being passed.
			targetSpeed = Math.Min(targetSpeed, 12.0);
		}
		else if (aheadGap < stopGap)
			targetSpeed = 0;
		else if (aheadGap < stopGap + 5.0)
			targetSpeed = Math.Min(targetSpeed, 4.0);

		// At road speed, keep enough room behind the car ahead to stop (about 3 m/s2 of braking).
		if (fast && !bypassing && aheadGap < 500.0)
		{
			// Braking plan of about 2 m/s2 plus a second of reaction room, so a fast car backs off the car ahead early.
			float reactionGap = speedKmh / 3.6;
			float followSpeed = Math.Sqrt(2.0 * 2.0 * Math.Max(aheadGap - stopGap - reactionGap, 0)) * 3.6;
			targetSpeed = Math.Min(targetSpeed, followSpeed);
		}

		// Keep what we computed for the debug visuals.
		run.m_vTarget = target;
		run.m_vNearest = bestProj;
		run.m_fSpeedKmh = speedKmh;
		run.m_fTargetSpeed = targetSpeed;
		run.m_fAheadGap = aheadGap;
		run.m_fOffLane = Math.Sqrt(bestDistSq);
		run.m_fHeadingErrDeg = headingError * Math.RAD2DEG;

		// Launch watch: the car wants to move (clear road, a real target speed) but is not gaining ground. Measured by
		// progress along the path, not by speed, so a truck rocking forward and back on a slope still counts as stuck.
		bool wantsToMove = targetSpeed > 1.5;
		if (!wantsToMove || run.m_fProgress > run.m_fProgressAnchor + LAUNCH_PROGRESS_STEP)
		{
			run.m_fProgressAnchor = Math.Max(run.m_fProgress, run.m_fProgressAnchor);
			run.m_fNoProgressTime = 0;
		}
		else if (run.m_fElapsed > 3.0)
		{
			run.m_fNoProgressTime += timeSlice;
		}
		bool launchBoost = run.m_fNoProgressTime > LAUNCH_BOOST_AFTER;

		float speedError = targetSpeed - speedKmh;
		if (targetSpeed <= 0.01)
		{
			run.m_fThrottle = 0;
			run.m_fBrake = Math.Clamp(0.15 + 0.05 * speedKmh, 0.15, 0.8);
		}
		else if (speedError >= -2.0)
		{
			// Full throttle is allowed at road speed, and uphill the car pushes harder to hold its speed.
			float maxThrottle = 0.8;
			if (fast || launchBoost)
				maxThrottle = 1.0;
			float hillBoost = Math.Clamp(run.m_fSlope * 2.0, 0, 0.4);
			run.m_fThrottle = Math.Clamp(0.30 + 0.12 * speedError + hillBoost, 0, maxThrottle);
			run.m_fBrake = 0;
		}
		else
		{
			run.m_fThrottle = 0;
			run.m_fBrake = Math.Clamp(0.08 * (-speedError - 2.0), 0, 0.6);
		}

		// Stopping at a gate: brake from the distance still to go (the deceleration needed to stop exactly there),
		// not only from the speed error - so a car rolling downhill or arriving fast still stops on its mark.
		if (approach && stopRemaining < 60.0)
		{
			float v = speedKmh / 3.6;
			float need = v * v / (2.0 * Math.Max(stopRemaining - 0.3, 0.05));
			if (need > 0.8)
			{
				run.m_fBrake = Math.Max(run.m_fBrake, Math.Clamp(need / 6.0, 0, 1.0));
				run.m_fThrottle = 0;
			}

			// Downhill the car wants to roll on: lean on the brake near the gate.
			if (stopRemaining < 6.0 && run.m_fSlope < -0.02)
				run.m_fBrake = Math.Max(run.m_fBrake, Math.Clamp(-run.m_fSlope * 4.0, 0, 0.6));

			// Parked on the mark: hold it.
			if (stopRemaining <= 0.35 && speedKmh < 3.0)
			{
				run.m_fThrottle = 0;
				run.m_fBrake = Math.Max(run.m_fBrake, 0.6);
			}
		}

		// Rolling backwards (it ran out of grunt on a hill): stand on the brake until it has stopped, then start again.
		if (run.m_fFwdSpeedKmh < -0.5 && targetSpeed > 0.01)
		{
			run.m_fThrottle = 0;
			run.m_fBrake = Math.Clamp(0.5 + 0.1 * -run.m_fFwdSpeedKmh, 0.5, 1.0);
			run.m_fHillTimer = 0;
		}
		// Hill hold: like a hill start in a real car. Stopped on a slope the brake stays on so the car cannot
		// roll back; pulling away, the brake is released gradually while the throttle and clutch bite.
		else if (speedKmh < 3.0 && Math.AbsFloat(run.m_fSlope) > 0.02)
		{
			if (run.m_fThrottle > 0.05)
			{
				run.m_fHillTimer += timeSlice;
				float holdLeft = 1.0 - run.m_fHillTimer / 1.0;
				// A car that cannot pull away gets no hill-hold brake: it only fights the engine.
				if (holdLeft > 0 && !launchBoost)
					run.m_fBrake = Math.Max(run.m_fBrake, 0.6 * holdLeft);
			}
			else
			{
				run.m_fHillTimer = 0;
				run.m_fBrake = Math.Max(run.m_fBrake, 0.6);
			}
		}
		else if (speedKmh >= 3.0)
		{
			run.m_fHillTimer = 0;
		}

		// Launch assist: boosting the throttle did not get the car moving. Give it a small push along its heading
		// (a rolling start) and keep doing so whenever it slows to a crawl for the rest of this run.
		if (run.m_fNoProgressTime > LAUNCH_ASSIST_AFTER && !run.m_bLaunchAssist)
		{
			run.m_bLaunchAssist = true;
			Print(string.Format("[EEF Checkpoint] A car could not pull away (no progress for %1 s, slope %2, speed %3 km/h, engine on %4) - giving it a push.", run.m_fNoProgressTime, run.m_fSlope, speedKmh, run.m_Sim.EngineIsOn()), LogLevel.WARNING);
		}

		if (run.m_bLaunchAssist && wantsToMove && !bypassing && Math.AbsFloat(headingError) < 0.6 && run.m_fFwdSpeedKmh < LAUNCH_ASSIST_SPEED_MS * 3.6 * 0.9)
		{
			Physics launchPhys = vehicle.GetPhysics();
			if (launchPhys)
				launchPhys.SetVelocity(forwardWorld * LAUNCH_ASSIST_SPEED_MS);
		}

		if (run.m_fElapsed - run.m_fLastLog >= 1.0)
		{
			run.m_fLastLog = run.m_fElapsed;
			DebugLog(string.Format("Rails[%1]: progress=%2 m speed=%3 km/h (target %4) offLane=%5 m headingErr=%6 deg steer=%7 throttle=%8 brake=%9",
				run.m_iKind, run.m_fProgress, speedKmh, targetSpeed, Math.Sqrt(bestDistSq), headingError * Math.RAD2DEG, run.m_fSteering, run.m_fThrottle, run.m_fBrake));
			DebugLog(string.Format("Rails[%1]:   stopRemaining=%2 aheadGap=%3 gear=%4 rpm=%5 engineOn=%6 clutch=%7 pathLength=%8", run.m_iKind, stopRemaining, aheadGap, run.m_Sim.GetGear(), run.m_Sim.EngineGetRPM(), run.m_Sim.EngineIsOn(), run.m_Sim.GetClutch(), pathLength));
			DebugLog(string.Format("Rails[%1]:   fwdSpeed=%2 km/h cornerLimit=%3 km/h slope=%4", run.m_iKind, run.m_fFwdSpeedKmh, run.m_fLimitSpeed, run.m_fSlope));
		}

		return false;
	}

	//------------------------------------------------------------------------------------------------
	// DEBUG VISUALS (m_bDebugVisuals) - local to the machine that renders, never replicated.
	//------------------------------------------------------------------------------------------------

	protected static const ResourceName DEBUG_GHOST_PREFAB = "{C4336BA903093340}Prefabs/Props/RallyDLC/GhostVehicle_Base.et";
	protected static const ResourceName DEBUG_GHOST_MATERIAL = "Assets/Ghost.emat";

	protected ref array<ref DebugTextWorldSpace> m_aDebugTexts = new array<ref DebugTextWorldSpace>();

	//! Spawn a local translucent copy of the car (the Rally DLC ghost technique: a ghost entity that takes
	//! the vehicle's mesh with every material remapped to the ghost material). Moved each frame to the
	//! ideal pose on the path. Without the DLC prefab it still falls back to an opaque mesh copy.
	protected void SpawnDebugGhost(EEF_ScriptedDriveRun run)
	{
		if (!m_bDebugVisuals || System.IsConsoleApp())
			return;

		if (!run || !run.m_State || !run.m_State.m_Vehicle)
			return;

		EntitySpawnParams params = new EntitySpawnParams();
		params.TransformMode = ETransformMode.WORLD;
		run.m_State.m_Vehicle.GetWorldTransform(params.Transform);

		IEntity ghost;
		Resource ghostRes = Resource.Load(DEBUG_GHOST_PREFAB);
		if (ghostRes && ghostRes.IsValid())
			ghost = GetGame().SpawnEntityPrefabLocal(ghostRes, GetGame().GetWorld(), params);
		else
			ghost = GetGame().SpawnEntity(GenericEntity, GetGame().GetWorld(), params);

		if (!ghost)
			return;

		ghost.SetFlags(EntityFlags.ACTIVE, true);

		VObject obj = run.m_State.m_Vehicle.GetVObject();
		if (obj)
		{
			string materials[256];
			string remap;
			int numMats = obj.GetMaterials(materials);
			for (int i = 0; i < numMats; i++)
				remap += string.Format("$remap '%1' '%2';", materials[i], DEBUG_GHOST_MATERIAL);

			ghost.SetObject(obj, remap);
		}

		ParametricMaterialInstanceComponent material = ParametricMaterialInstanceComponent.Cast(ghost.FindComponent(ParametricMaterialInstanceComponent));
		if (material)
			material.SetColor(ARGB(120, 90, 255, 130));

		run.m_GhostEntity = ghost;
	}

	protected void DeleteDebugGhost(EEF_ScriptedDriveRun run)
	{
		if (run && run.m_GhostEntity)
		{
			delete run.m_GhostEntity;
			run.m_GhostEntity = null;
		}
	}

	protected void AddDebugText(string text, vector pos, int color)
	{
		DebugTextWorldSpace label = DebugTextWorldSpace.Create(GetGame().GetWorld(), text, DebugTextFlags.CENTER | DebugTextFlags.FACE_CAMERA | DebugTextFlags.IN_WORLD, pos[0], pos[1], pos[2], 1.0, color, 0x88000000);
		if (label)
			m_aDebugTexts.Insert(label);
	}

	//! Point on a run's path at the given arc length.
	protected vector GetPathPoint(EEF_ScriptedDriveRun run, float arc)
	{
		int n = run.m_aPath.Count();
		if (n < 2)
			return vector.Zero;

		if (arc <= 0)
			return run.m_aPath[0];

		for (int i = 0; i < n - 1; i++)
		{
			if (run.m_aCum[i + 1] >= arc)
			{
				float len = run.m_aCum[i + 1] - run.m_aCum[i];
				float t = 0;
				if (len > 0.001)
					t = (arc - run.m_aCum[i]) / len;
				return run.m_aPath[i] + (run.m_aPath[i + 1] - run.m_aPath[i]) * t;
			}
		}

		return run.m_aPath[n - 1];
	}

	//! Redraw everything every frame: queue gates plus, for each car on rails, its path, ideal position,
	//! look-ahead target, stop point, ghost car and a status label.
	protected void DrawScriptedDebug()
	{
		if (!m_bDebugVisuals || System.IsConsoleApp())
			return;

		m_aDebugTexts.Clear();

		ShapeFlags lineFlags = ShapeFlags.ONCE | ShapeFlags.NOZBUFFER;
		ShapeFlags solidFlags = ShapeFlags.ONCE | ShapeFlags.NOZBUFFER | ShapeFlags.TRANSP | ShapeFlags.NOOUTLINE;
		vector lift = Vector(0, 0.25, 0);

		// Queue gates: where each slot stops (on the road centre line).
		for (int g = 0; g < m_aQueueGates.Count(); g++)
		{
			EEF_CheckpointQueueGate gate = m_aQueueGates[g];
			if (!gate)
				continue;

			Shape.CreateSphere(ARGB(170, 60, 120, 255), solidFlags, gate.m_Point + lift, 0.6);
			Shape.CreateArrow(gate.m_Point + lift, gate.m_Point + lift + gate.m_Forward * 3.0, 0.5, ARGB(255, 60, 120, 255), lineFlags);
			AddDebugText(string.Format("GATE %1", g), gate.m_Point + Vector(0, 2.0, 0), ARGB(255, 130, 180, 255));
		}

		foreach (EEF_ScriptedDriveRun run : m_aScriptedRuns)
		{
			if (!run || !run.m_State || !run.m_State.m_Vehicle)
				continue;

			int n = run.m_aPath.Count();
			if (n < 2)
				continue;

			vector vehPos = run.m_State.m_Vehicle.GetOrigin();

			// The lane path a little behind and well ahead of the car.
			int firstIdx = Math.Max(run.m_iSeg - 3, 0);
			int lastIdx = Math.Min(run.m_iSeg + 28, n - 1);
			vector pts[40];
			int count = 0;
			for (int i = firstIdx; i <= lastIdx && count < 40; i++)
			{
				pts[count] = run.m_aPath[i] + lift;
				count++;
			}
			if (count >= 2)
				Shape.CreateLines(ARGB(255, 0, 230, 255), lineFlags, pts, count);

			// Where the car SHOULD be: nearest point on the path (green).
			Shape.CreateSphere(ARGB(210, 40, 255, 40), solidFlags, run.m_vNearest + lift, 0.3);

			// What it is steering at: look-ahead point (red) and the line to it (yellow).
			Shape.CreateSphere(ARGB(210, 255, 40, 40), solidFlags, run.m_vTarget + lift, 0.4);
			vector aim[2] = { vehPos + lift, run.m_vTarget + lift };
			Shape.CreateLines(ARGB(255, 255, 230, 0), lineFlags, aim, 2);

			// How far ahead it is checking the bends (magenta, large) and the bend that limits its speed (white).
			if (run.m_vHorizonPoint != vector.Zero)
				Shape.CreateSphere(ARGB(200, 255, 40, 255), solidFlags, run.m_vHorizonPoint + lift, 0.9);
			if (run.m_vLimitPoint != vector.Zero)
				Shape.CreateSphere(ARGB(230, 255, 255, 255), solidFlags, run.m_vLimitPoint + lift, 0.7);

			// Where it will stop (approach only).
			bool approach = (run.m_iKind != SCRIPTED_KIND_DEPARTURE);
			if (approach)
			{
				vector stopPos = GetPathPoint(run, run.m_fStopArc);
				Shape.CreateSphere(ARGB(200, 255, 150, 0), solidFlags, stopPos + lift, 0.7);
				AddDebugText("STOP", stopPos + Vector(0, 2.0, 0), ARGB(255, 255, 170, 60));
			}

			// Ghost car on the ideal pose: at the nearest path point, facing along the path.
			if (run.m_GhostEntity)
			{
				int seg = Math.Min(run.m_iSeg, n - 2);
				vector dir = run.m_aPath[seg + 1] - run.m_aPath[seg];
				dir[1] = 0;
				if (dir.LengthSq() > 0.0001)
				{
					dir.Normalize();
					vector mat[4];
					mat[0] = Vector(dir[2], 0, -dir[0]);
					mat[1] = Vector(0, 1, 0);
					mat[2] = dir;
					mat[3] = run.m_vNearest;
					run.m_GhostEntity.SetTransform(mat);
				}
			}

			string kind = "DEPART";
			if (approach)
				kind = "SLOT " + run.m_State.m_iQueueSlot;

			string gapText = "-";
			if (run.m_fAheadGap < 500)
				gapText = Math.Round(run.m_fAheadGap).ToString() + " m";

			AddDebugText(string.Format("%1  %2/%3 km/h  gap %4  off %5 m  err %6 deg", kind, Math.Round(run.m_fSpeedKmh), Math.Round(run.m_fTargetSpeed), gapText, Math.Round(run.m_fOffLane * 100) / 100, Math.Round(run.m_fHeadingErrDeg)), vehPos + Vector(0, 3.0, 0), ARGB(255, 255, 255, 255));
		}
	}

	//! Write the latest computed outputs (called on both frame events, after the fixed step computes them).
	protected void ApplyScriptedOutputs()
	{
		foreach (EEF_ScriptedDriveRun run : m_aScriptedRuns)
		{
			if (!run || !run.m_Sim)
				continue;

			// With the AI off nothing manages the clutch: a freshly spawned car sits with it open and just revs.
			run.m_Sim.SetClutch(run.m_fClutch);
			run.m_Sim.SetThrottle(run.m_fThrottle);
			run.m_Sim.SetBreak(run.m_fBrake, false);
			run.m_Sim.SetSteering(run.m_fSteering);
		}
	}

	//! An approach drive reached its gate (or could go no further): stop dead, freeze the body like the
	//! old hold did, and run the usual queue logic (front vehicle becomes HELD and starts the hold timer).
	protected void FinishScriptedApproach(EEF_CheckpointVehicleState state)
	{
		// Hostile roll at every gate. A car that turns hostile is not held: it just stops where it is.
		if (RollHostile(state))
			return;

		Physics phys = state.m_Vehicle.GetPhysics();
		if (phys)
		{
			phys.SetVelocity(vector.Zero);
			phys.SetAngularVelocity(vector.Zero);
		}

		SCR_PhysicsHelper.ChangeSimulationState(state.m_Vehicle, SimulationState.COLLISION);
		state.m_bHeld = true;

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

	//! A scripted drive is over: neutral inputs, then either hold at the gate (approach) or wake the AI
	//! so it finishes the route (departure).
	protected void EndScriptedRun(EEF_ScriptedDriveRun run)
	{
		DeleteDebugGhost(run);
		m_aScriptedRuns.RemoveItem(run);

		if (run.m_Sim)
		{
			run.m_Sim.SetThrottle(0);
			run.m_Sim.SetBreak(0, false);
			run.m_Sim.SetSteering(0);
		}

		UpdateEventMask();

		EEF_CheckpointVehicleState state = run.m_State;
		if (!state || !state.m_Vehicle || m_aVehicles.Find(state) == -1)
			return;

		if (run.m_iKind == SCRIPTED_KIND_APPROACH)
		{
			// A car that stalled well short of its gate (could not pull away, wedged) must not pose as having arrived:
			// it would sit "held" far from the stop line and block everything behind it. Remove it like any other
			// stalled car so the queue closes up.
			float shortOfGate = run.m_fStopArc - run.m_fProgress;
			if (run.m_bStalled && shortOfGate > STALLED_APPROACH_CULL_DISTANCE)
			{
				Print(string.Format("[EEF Checkpoint] A car stalled %1 m short of its queue gate (slot %2) - removing it and its crew so the line keeps moving.", shortOfGate, state.m_iQueueSlot), LogLevel.WARNING);
				DespawnVehicleState(state, true);
				return;
			}

			FinishScriptedApproach(state);
			return;
		}

		if (run.m_iKind == SCRIPTED_KIND_PULLOVER)
		{
			FinishPullOver(state);
			return;
		}

		// Departure / transit over (path end, hand-back distance or the end of the launch): the AI takes the car on
		// to the Despawn marker, so give it the drive order it needs.
		if (m_DespawnPoint)
			AssignMoveWaypoint(state.m_OccupantGroup, m_DespawnPoint.GetOrigin(), m_fWaypointCompletionRadius);

		ResumeVehicle(state);
		ApplyCruiseSpeed(state, GetDepartureAISpeed(state));
	}

	//! Cruise speed the AI gets when it takes over a departing car: flee speed for a runaway.
	protected float GetDepartureAISpeed(EEF_CheckpointVehicleState state)
	{
		if (state && state.m_bFleeing)
			return m_fFleeSpeedKmh;
		return m_fApproachSpeedKmh;
	}

	//! Debug timer callback: release the vehicle if it is still the HELD front vehicle we scheduled.
	protected void ReleaseHeldVehicle(EEF_CheckpointVehicleState state)
	{
		if (!state || m_aVehicles.Find(state) == -1)
			return;

		if (state.m_eState != EEF_ECheckpointVehicleState.HELD)
			return;

		DebugLog("Auto-clear timer elapsed - clearing the front vehicle.");
		ClearVehicle(state);
	}

	//! Public permit signal: release the vehicle currently held at the front, if any. This is the
	//! seam Stage 3 (#19) drives instead of the debug timer.
	void ReleaseFrontVehicle()
	{
		EEF_CheckpointVehicleState front = GetFrontVehicle();
		if (front && front.m_eState == EEF_ECheckpointVehicleState.HELD)
			ClearVehicle(front);
	}

	//! Send a held vehicle on its way: leave the queue and start the scripted departure to the exit.
	protected void ReleaseVehicle(EEF_CheckpointVehicleState state)
	{
		if (!state || !state.m_Vehicle)
			return;

		state.m_iQueueSlot = -1;
		state.m_bReleasePending = false;
		state.m_bCleared = true;

		SetState(state, EEF_ECheckpointVehicleState.DEPARTING);

		// The script drives the whole way. If the road path is not available yet, stay put and retry
		// rather than handing the car to the AI (the AI reverses out of a standing start in a road gap).
		if (!StartScriptedDeparture(state))
		{
			DebugLog("Rails: no departure path yet - holding and retrying.");
			GetGame().GetCallqueue().CallLater(RetryScriptedDeparture, 1000, false, state, 1);
		}

		DebugLog("Vehicle released - departing toward the exit.");

		// Advance everyone behind it now that the front slot is free.
		PromoteQueue();
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
		// Build the road line the way traffic actually travels - from the spawn point toward the checkpoint,
		// chained across road pieces and across any gap a roadblock cuts into the road - then walk it
		// backwards from the checkpoint's projection. (Starting at the checkpoint instead can land on the
		// wrong road piece when the road is cut there, which left only one gate.)
		// The gates lie on the FIRST spawn marker's route (the main approach); other feeders must merge
		// onto it before the queue tail.
		EEF_CheckpointRoute mainRoute = GetMainRoute();
		if (!mainRoute || mainRoute.m_aLine.Count() < 2)
		{
			DebugLog("Could not build the road line from the first Spawn marker to the checkpoint - cannot compute queue gates. Is the Spawn marker on a road?");
			return;
		}
		array<vector> forwardLine = mainRoute.m_aLine;

		int forwardSeg = FindNearestSegment(forwardLine, checkpointPos);
		array<vector> backPoints = {};
		backPoints.Insert(ProjectOnSegment(checkpointPos, forwardLine[forwardSeg], forwardLine[forwardSeg + 1]));
		for (int bi = forwardSeg; bi >= 0; bi--)
			backPoints.Insert(forwardLine[bi]);

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

			// The gates mark where CARS stop, so they sit on the traffic lane (to the right of travel), not
			// on the road centre line the route is measured along. Stop arcs are unaffected (the shift is
			// sideways), so this only moves where the gate is drawn and which side of the road it is on.
			vector gateRight = Vector(gate.m_Forward[2], 0, -gate.m_Forward[0]);
			gate.m_Point = gate.m_Point + gateRight * mainRoute.m_fLaneOffset;
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

	//------------------------------------------------------------------------------------------------
	// ROUTES AND TAKEOVER (replaces the old trigger-sphere zone)
	//
	// Each Spawn marker has a route: the road line from that marker to the checkpoint. A car's distance to
	// the stop line is measured ALONG ITS ROUTE, so bends and side roads do not matter. The script takes
	// the car over once that distance is within the takeover distance = queue length + a margin.
	//------------------------------------------------------------------------------------------------

	//! Build the road line for every Spawn marker, once. Route index = Spawn marker index.
	protected void BuildRoutes()
	{
		if (m_bRoutesBuilt)
			return;

		if (!EnsurePointsResolved())
			return;

		m_bRoutesBuilt = true;
		m_aRoutes.Clear();

		vector checkpointPos = GetOwner().GetOrigin();

		SCR_AIWorld aiWorld = SCR_AIWorld.Cast(GetGame().GetAIWorld());
		if (aiWorld && aiWorld.GetRoadNetworkManager())
			LogRoadScanOnce(aiWorld.GetRoadNetworkManager(), checkpointPos, m_aSpawnPoints[0].GetOrigin());

		for (int r = 0; r < m_aSpawnPoints.Count(); r++)
		{
			EEF_CheckpointRoute route = new EEF_CheckpointRoute();
			route.m_vSpawn = m_aSpawnPoints[r].GetOrigin();

			float wantLength = vector.Distance(route.m_vSpawn, checkpointPos) * 1.3 + 40.0;
			if (BuildRoadLine(route.m_vSpawn, checkpointPos, wantLength, route.m_aLine, true) && route.m_aLine.Count() >= 2)
			{
				route.m_aCum.Insert(0.0);
				for (int i = 1; i < route.m_aLine.Count(); i++)
				{
					float dx = route.m_aLine[i][0] - route.m_aLine[i - 1][0];
					float dz = route.m_aLine[i][2] - route.m_aLine[i - 1][2];
					route.m_aCum.Insert(route.m_aCum[i - 1] + Math.Sqrt(dx * dx + dz * dz));
				}

				float lateral;
				route.m_fStopArc = ProjectOnRoute(route, checkpointPos, lateral);
				route.m_fLaneOffset = GetLaneOffset();
				DebugLog(string.Format("Route %1: from the Spawn marker at %2 - %3 m of road, stop line at %4 m (checkpoint %5 m off the road line).", r, route.m_vSpawn, route.m_aCum[route.m_aCum.Count() - 1], route.m_fStopArc, lateral));
			}
			else
			{
				route.m_aLine.Clear();
				DebugLog(string.Format("Route %1: could not build a road line from the Spawn marker at %2 to the checkpoint. Is that marker on a road?", r, route.m_vSpawn));
			}

			m_aRoutes.Insert(route);
		}

		if (m_bValidateSpawns)
		{
			int validCount = 0;
			for (int r = 0; r < m_aRoutes.Count(); r++)
			{
				ValidateRoute(r, m_aRoutes[r]);
				if (m_aRoutes[r].m_bValid)
					validCount++;
			}

			if (validCount == 0)
				Print("[EEF Checkpoint] SPAWN CHECK: NO Spawn marker passed - no traffic will spawn until the markers are fixed (see the messages above).", LogLevel.ERROR);
		}

		StartSpawnPathDebug();
	}

	// ------------------------------------------------------------------------------------------------
	// SPAWN PATH DEBUG: the road from each Spawn marker to the Despawn marker, drawn once for a minute.
	// ------------------------------------------------------------------------------------------------

	protected const float SPAWN_PATH_DEBUG_SECONDS = 60.0;

	protected ref array<ref EEF_CheckpointRoute> m_aSpawnPathDebug = new array<ref EEF_CheckpointRoute>();
	protected ref array<ref DebugTextWorldSpace> m_aSpawnPathTexts = new array<ref DebugTextWorldSpace>();
	protected float m_fSpawnPathUntil;
	protected bool m_bSpawnPathActive;

	protected void AddSpawnPathLabel(string text, vector pos, int color)
	{
		DebugTextWorldSpace label = DebugTextWorldSpace.Create(GetGame().GetWorld(), text, DebugTextFlags.CENTER | DebugTextFlags.FACE_CAMERA | DebugTextFlags.IN_WORLD, pos[0], pos[1], pos[2], 3.0, color, 0xAA000000);
		if (label)
			m_aSpawnPathTexts.Insert(label);
	}

	//! Build the road line from every Spawn marker toward the Despawn marker (the same road-chaining the cars
	//! use) and start drawing it. Where the chain stops short of the Despawn marker the label says so.
	protected void StartSpawnPathDebug()
	{
		if (!m_bDebugSpawnPath || System.IsConsoleApp() || !m_DespawnPoint)
			return;

		float savedWidth = m_fLastLineRoadWidth;
		vector despawnPos = m_DespawnPoint.GetOrigin();

		m_aSpawnPathDebug.Clear();
		m_aSpawnPathTexts.Clear();
		AddSpawnPathLabel("DESPAWN", despawnPos + Vector(0, 3.0, 0), ARGB(255, 255, 120, 120));

		for (int r = 0; r < m_aRoutes.Count(); r++)
		{
			EEF_CheckpointRoute path = new EEF_CheckpointRoute();
			path.m_vSpawn = m_aRoutes[r].m_vSpawn;
			path.m_bValid = m_aRoutes[r].m_bValid;

			float wantLength = vector.Distance(path.m_vSpawn, despawnPos) * 1.3 + 40.0;
			if (BuildRoadLine(path.m_vSpawn, despawnPos, wantLength, path.m_aLine, true) && path.m_aLine.Count() >= 2)
			{
				path.m_aCum.Insert(0.0);
				for (int i = 1; i < path.m_aLine.Count(); i++)
				{
					float dx = path.m_aLine[i][0] - path.m_aLine[i - 1][0];
					float dz = path.m_aLine[i][2] - path.m_aLine[i - 1][2];
					path.m_aCum.Insert(path.m_aCum[i - 1] + Math.Sqrt(dx * dx + dz * dz));
				}

				// Does the chain actually get to the Despawn marker?
				vector end = path.m_aLine[path.m_aLine.Count() - 1];
				float shortBy = Math.Sqrt(Math.Pow(end[0] - despawnPos[0], 2) + Math.Pow(end[2] - despawnPos[2], 2));
				if (shortBy > 40.0)
				{
					AddSpawnPathLabel(string.Format("MARKER %1: ROAD ENDS HERE - %2 m short of the Despawn", r, Math.Round(shortBy)), end + Vector(0, 3.0, 0), ARGB(255, 255, 200, 60));
					Print(string.Format("[EEF Checkpoint] SPAWN PATH marker %1: the road chain from this marker ends %2 m short of the Despawn marker (at %3).", r, Math.Round(shortBy), end), LogLevel.WARNING);
				}
			}
			else
			{
				path.m_aLine.Clear();
				Print(string.Format("[EEF Checkpoint] SPAWN PATH marker %1: no road line could be built from the marker toward the Despawn marker.", r), LogLevel.WARNING);
			}

			string status = "OK";
			int color = ARGB(255, 60, 255, 100);
			if (!path.m_bValid)
			{
				status = "FAILED the spawn check - see the log";
				color = ARGB(255, 255, 70, 70);
			}
			AddSpawnPathLabel(string.Format("SPAWN %1: %2", r, status), path.m_vSpawn + Vector(0, 3.0, 0), color);

			m_aSpawnPathDebug.Insert(path);
		}

		m_fLastLineRoadWidth = savedWidth;

		m_fSpawnPathUntil = GetWorldTimeSeconds() + SPAWN_PATH_DEBUG_SECONDS;
		m_bSpawnPathActive = true;
		UpdateEventMask();
		DebugLog(string.Format("Spawn path debug: drawing %1 path(s) for %2 s.", m_aSpawnPathDebug.Count(), SPAWN_PATH_DEBUG_SECONDS));
	}

	//! Redrawn every frame (the shapes live one frame) until the time is up, then everything is removed.
	protected void DrawSpawnPathDebug()
	{
		if (!m_bSpawnPathActive)
			return;

		if (GetWorldTimeSeconds() >= m_fSpawnPathUntil)
		{
			m_bSpawnPathActive = false;
			m_aSpawnPathDebug.Clear();
			m_aSpawnPathTexts.Clear();
			UpdateEventMask();
			return;
		}

		ShapeFlags lineFlags = ShapeFlags.ONCE | ShapeFlags.NOZBUFFER;
		ShapeFlags solidFlags = ShapeFlags.ONCE | ShapeFlags.NOZBUFFER | ShapeFlags.TRANSP | ShapeFlags.NOOUTLINE;
		vector lift = Vector(0, 0.6, 0);

		foreach (EEF_CheckpointRoute path : m_aSpawnPathDebug)
		{
			int color = ARGB(255, 60, 255, 100);
			if (!path.m_bValid)
				color = ARGB(255, 255, 70, 70);

			Shape.CreateSphere(color, solidFlags, path.m_vSpawn + lift, 1.2);

			int n = path.m_aLine.Count();
			if (n < 2)
				continue;

			for (int i = 0; i < n - 1; i++)
				Shape.Create(ShapeType.LINE, color, lineFlags, path.m_aLine[i] + lift, path.m_aLine[i + 1] + lift);

			// Direction arrows along the road.
			float total = path.m_aCum[n - 1];
			for (float arc = 15.0; arc < total; arc += 40.0)
			{
				vector at = GetRoutePointAt(path, arc) + lift;
				vector dir = GetRouteDirectionAt(path, arc);
				Shape.CreateArrow(at, at + dir * 5.0, 1.0, color, lineFlags);
			}
		}
	}

	protected const float SPAWN_MAX_ROAD_DISTANCE = 8.0;		//! A Spawn marker further than this from a road is not on a road
	protected const float SPAWN_MAX_CHECKPOINT_OFFSET = 15.0;	//! The road from a marker must pass the checkpoint within this

	//! Point on a route's road line at the given arc length from its start.
	protected vector GetRoutePointAt(EEF_CheckpointRoute route, float arc)
	{
		int n = route.m_aLine.Count();
		if (n < 2)
			return route.m_vSpawn;

		for (int i = 1; i < n; i++)
		{
			if (route.m_aCum[i] >= arc)
			{
				float span = route.m_aCum[i] - route.m_aCum[i - 1];
				float t = 0;
				if (span > 0.001)
					t = (arc - route.m_aCum[i - 1]) / span;
				return route.m_aLine[i - 1] + (route.m_aLine[i] - route.m_aLine[i - 1]) * t;
			}
		}

		return route.m_aLine[n - 1];
	}

	//! Horizontal unit direction of travel along a route at the given arc length.
	protected vector GetRouteDirectionAt(EEF_CheckpointRoute route, float arc)
	{
		vector a = GetRoutePointAt(route, Math.Max(arc - 3.0, 0));
		vector b = GetRoutePointAt(route, arc + 3.0);
		vector dir = b - a;
		dir[1] = 0;
		if (dir.LengthSq() < 0.0001)
			return Vector(0, 0, 1);
		dir.Normalize();
		return dir;
	}

	//! The spawn check for one Spawn marker's route. Sets m_bValid / m_sProblem and reports the verdict in the log.
	protected void ValidateRoute(int index, EEF_CheckpointRoute route)
	{
		route.m_bValid = true;
		route.m_sProblem = "";

		vector checkpointPos = GetOwner().GetOrigin();

		if (route.m_aLine.Count() < 2)
		{
			route.m_bValid = false;
			route.m_sProblem = "no road line could be built from this marker toward the checkpoint - the marker is not on a road, or the road does not connect.";
		}
		else
		{
			float markerOffRoad;
			ProjectOnRoute(route, route.m_vSpawn, markerOffRoad);

			float checkpointOffRoad;
			ProjectOnRoute(route, checkpointPos, checkpointOffRoad);

			// Road the queue needs between the marker and the stop line: the queue length plus the takeover margin.
			float needRoad = Math.Max(m_iMaxConcurrent - 1, 0) * m_fQueueSlotSpacing + m_fTakeoverMargin;

			if (markerOffRoad > SPAWN_MAX_ROAD_DISTANCE)
			{
				route.m_bValid = false;
				route.m_sProblem = string.Format("the marker is %1 m from the nearest road (limit %2 m). Move it onto the road.", Math.Round(markerOffRoad), SPAWN_MAX_ROAD_DISTANCE);
			}
			else if (checkpointOffRoad > SPAWN_MAX_CHECKPOINT_OFFSET)
			{
				route.m_bValid = false;
				route.m_sProblem = string.Format("the road under this marker does not lead to the checkpoint: the nearest it passes is %1 m away (limit %2 m). The marker is on a different road.", Math.Round(checkpointOffRoad), SPAWN_MAX_CHECKPOINT_OFFSET);
			}
			else if (route.m_fStopArc < needRoad)
			{
				route.m_bValid = false;
				route.m_sProblem = string.Format("only %1 m of road between the marker and the stop line, the queue needs at least %2 m. Move the marker further back along the road.", Math.Round(route.m_fStopArc), Math.Round(needRoad));
			}
			else if (m_DespawnPoint)
			{
				// Traffic must flow toward the Despawn marker: the road direction at the stop line has to point at its side.
				vector toDespawn = m_DespawnPoint.GetOrigin() - checkpointPos;
				toDespawn[1] = 0;
				vector roadDir = GetRouteDirectionAt(route, route.m_fStopArc);
				if (toDespawn.LengthSq() > 1.0 && vector.Dot(toDespawn.Normalized(), roadDir) < 0)
				{
					route.m_bValid = false;
					route.m_sProblem = "traffic from this marker would arrive from the Despawn marker's side of the checkpoint (the marker is on the wrong side). Move it to the other side of the checkpoint.";
				}
			}

			if (route.m_bValid)
			{
				vector markerForward = m_aSpawnPoints[index].GetTransformAxis(2);
				markerForward[1] = 0;
				float headingOff = 0;
				if (markerForward.LengthSq() > 0.01)
				{
					markerForward.Normalize();
					vector startDir = GetRouteDirectionAt(route, 5.0);
					headingOff = Math.Acos(Math.Clamp(vector.Dot(markerForward, startDir), -1, 1)) * Math.RAD2DEG;
				}

				DebugLog(string.Format("SPAWN CHECK marker %1: OK - %2 m from the road, the road reaches the checkpoint (%3 m off the line) with %4 m of road before the stop line; marker arrow %5 deg off the road direction (cars spawn along the road, so that does not matter).", index, Math.Round(markerOffRoad * 10) / 10, Math.Round(checkpointOffRoad * 10) / 10, Math.Round(route.m_fStopArc), Math.Round(headingOff)));
			}
		}

		if (!route.m_bValid)
			Print(string.Format("[EEF Checkpoint] SPAWN CHECK marker %1 at %2 FAILED - no cars will spawn there: %3", index, route.m_vSpawn, route.m_sProblem), LogLevel.ERROR);
	}

	//! The spawn marker to use next: round robin over the markers that passed the spawn check. -1 if none did.
	protected int PickSpawnRoute()
	{
		int count = m_aSpawnPoints.Count();
		for (int k = 0; k < count; k++)
		{
			int idx = (m_iNextSpawn + k) % count;
			EEF_CheckpointRoute route = GetRoute(idx);
			if (!m_bValidateSpawns || (route && route.m_bValid))
				return idx;
		}

		return -1;
	}

	//! The route the queue gates are laid out on: the first marker that passed the spawn check (else the first).
	protected EEF_CheckpointRoute GetMainRoute()
	{
		BuildRoutes();

		foreach (EEF_CheckpointRoute route : m_aRoutes)
		{
			if (route && route.m_bValid && route.m_aLine.Count() >= 2)
				return route;
		}

		return GetRoute(0);
	}

	protected EEF_CheckpointRoute GetRoute(int index)
	{
		BuildRoutes();

		if (index < 0 || index >= m_aRoutes.Count())
			return null;

		return m_aRoutes[index];
	}

	//! Arc length of pos projected onto a route's road line; lateral = how far pos is from the line.
	protected float ProjectOnRoute(EEF_CheckpointRoute route, vector pos, out float lateral)
	{
		float bestArc = 0;
		lateral = float.MAX;

		for (int i = 0; i < route.m_aLine.Count() - 1; i++)
		{
			vector proj = ProjectOnSegment(pos, route.m_aLine[i], route.m_aLine[i + 1]);
			float dx = pos[0] - proj[0];
			float dz = pos[2] - proj[2];
			float dist = Math.Sqrt(dx * dx + dz * dz);
			if (dist < lateral)
			{
				lateral = dist;
				float sx = proj[0] - route.m_aLine[i][0];
				float sz = proj[2] - route.m_aLine[i][2];
				bestArc = route.m_aCum[i] + Math.Sqrt(sx * sx + sz * sz);
			}
		}

		return bestArc;
	}

	//! Road distance from a vehicle to the stop line, along its route. Falls back to the straight-line
	//! distance to the checkpoint if it has no route or has left it by more than 25 m.
	protected float GetDistanceToStop(EEF_CheckpointVehicleState state, vector pos)
	{
		EEF_CheckpointRoute route = GetRoute(state.m_iRoute);
		if (route && route.m_aLine.Count() >= 2)
		{
			float lateral;
			float arc = ProjectOnRoute(route, pos, lateral);
			if (lateral < 25.0)
				return route.m_fStopArc - arc;
		}

		vector checkpointPos = GetOwner().GetOrigin();
		float dx = pos[0] - checkpointPos[0];
		float dz = pos[2] - checkpointPos[2];
		return Math.Sqrt(dx * dx + dz * dz);
	}

	//! Road distance (m) from the stop line at which the script takes a car over: the queue tail plus a margin.
	protected float GetTakeoverDistance()
	{
		ComputeQueueGates();

		int gates = m_aQueueGates.Count();
		if (gates < 1)
			gates = m_iMaxConcurrent;

		return Math.Max(gates - 1, 0) * m_fQueueSlotSpacing + m_fTakeoverMargin;
	}

	protected bool IsInTakeoverRange(EEF_CheckpointVehicleState state, vector pos)
	{
		return GetDistanceToStop(state, pos) <= GetTakeoverDistance();
	}

#ifdef WORKBENCH
	protected ref array<ref DebugTextWorldSpace> m_aEditorLabels = new array<ref DebugTextWorldSpace>();

	protected void AddEditorLabel(BaseWorld world, string text, vector pos, int color)
	{
		DebugTextWorldSpace label = DebugTextWorldSpace.Create(world, text, DebugTextFlags.CENTER | DebugTextFlags.FACE_CAMERA | DebugTextFlags.IN_WORLD, pos[0], pos[1], pos[2], 5.0, color, 0xAA000000);
		if (label)
			m_aEditorLabels.Insert(label);
	}

	//------------------------------------------------------------------------------------------------
	//! World Editor preview. Exists ONLY in the World Editor (never while playing, never in a packaged
	//! mod). The road network is not available in the editor, so the gates and the takeover point are a
	//! STRAIGHT-LINE approximation toward each Spawn marker - play with the debug visuals on for the real
	//! road-following ones. Shows:
	//!   - the STOP LINE (where the front car is held): a red ribbon across the road, a blue beacon, and a
	//!     yellow arrow in the direction of travel (+Z of this entity)
	//!   - the queue gates (blue balls) and the SCRIPT TAKEOVER point (orange beacon)
	//!   - a green line to every Spawn marker and a red line to the Despawn marker
	//!   - a warning label if a Spawn or Despawn marker is missing
	override void _WB_AfterWorldUpdate(IEntity owner, float timeSlice)
	{
		m_aEditorLabels.Clear();

		if (GetGame() && GetGame().InPlayMode())
			return;

		WorldEditor worldEditor = Workbench.GetModule(WorldEditor);
		if (!worldEditor)
			return;

		WorldEditorAPI api = worldEditor.GetApi();
		if (!api || !api.GetWorld())
			return;

		BaseWorld world = api.GetWorld();

		array<IEntity> spawns = {};
		IEntity despawn;
		CollectMarkers(owner, spawns, despawn);

		vector origin = owner.GetOrigin();
		ShapeFlags softFlags = ShapeFlags.ONCE | ShapeFlags.TRANSP | ShapeFlags.NOOUTLINE;
		ShapeFlags lineFlags = ShapeFlags.ONCE | ShapeFlags.NOZBUFFER;
		vector lift = Vector(0, 0.35, 0);
		int blue = ARGB(255, 60, 120, 255);
		int orange = ARGB(255, 255, 150, 0);

		// Direction of travel (+Z of this entity, flattened) and the road-crossing direction.
		vector fwd = owner.GetTransformAxis(2);
		fwd[1] = 0;
		if (fwd.LengthSq() < 0.0001)
			fwd = Vector(0, 0, 1);
		fwd.Normalize();
		vector right = Vector(fwd[2], 0, -fwd[0]);

		// STOP LINE: a ribbon 12 m wide across the road (both windings so it shows from either side).
		vector a = origin + right * 6.0 + lift;
		vector b = origin - right * 6.0 + lift;
		vector c = b + fwd * 0.7;
		vector d = a + fwd * 0.7;
		vector ribbon[12] = { a, b, c, a, c, d, c, b, a, d, c, a };
		Shape.CreateTris(ARGB(200, 255, 40, 40), softFlags, ribbon, 12);

		// Direction of travel.
		Shape.CreateArrow(origin + lift, origin + fwd * 12.0 + lift, 2.0, ARGB(255, 255, 220, 0), lineFlags);

		// Beacon so it can be found from far away.
		vector top = origin + Vector(0, 30.0, 0);
		Shape.Create(ShapeType.LINE, blue, lineFlags, origin, top);
		Shape.CreateSphere(ARGB(120, 60, 120, 255), softFlags, top, 2.5);
		AddEditorLabel(world, "STOP LINE / HOLD POINT", top + Vector(0, 5.0, 0), blue);

		int gateCount = Math.Max(m_iMaxConcurrent, 1);
		float takeover = (gateCount - 1) * m_fQueueSlotSpacing + m_fTakeoverMargin;

		foreach (int index, IEntity spawn : spawns)
		{
			vector spawnPos = spawn.GetOrigin();
			vector dir = origin - spawnPos;
			dir[1] = 0;
			if (dir.Length() < 1.0)
				continue;
			dir.Normalize();

			Shape.Create(ShapeType.LINE, ARGB(200, 40, 220, 60), lineFlags, spawnPos + lift, origin + lift);

			// Queue gates and their slot numbers: only along the first Spawn marker's line - that is the
			// route the real gates are laid out on.
			if (index == 0)
			{
				for (int g = 1; g < gateCount; g++)
				{
					vector gatePos = origin - dir * (g * m_fQueueSlotSpacing);
					Shape.CreateSphere(ARGB(170, 60, 120, 255), softFlags, gatePos + lift, 0.8);
					AddEditorLabel(world, "SLOT " + g, gatePos + Vector(0, 3.0, 0), blue);
				}
			}

			vector takeoverPos = origin - dir * takeover;
			Shape.CreateSphere(ARGB(170, 255, 150, 0), softFlags, takeoverPos + lift, 1.2);
			Shape.Create(ShapeType.LINE, orange, lineFlags, takeoverPos, takeoverPos + Vector(0, 15.0, 0));
			AddEditorLabel(world, "SCRIPT TAKEOVER (approx.)", takeoverPos + Vector(0, 19.0, 0), orange);
		}

		if (despawn)
			Shape.Create(ShapeType.LINE, ARGB(200, 230, 50, 50), lineFlags, origin + lift, despawn.GetOrigin() + lift);

		if (spawns.IsEmpty())
			AddEditorLabel(world, "NO SPAWN MARKER - add an EEF_CheckpointSpawnMarker as a child", origin + Vector(0, 8.0, 0), ARGB(255, 255, 60, 60));

		if (!despawn)
			AddEditorLabel(world, "NO DESPAWN MARKER - add an EEF_CheckpointDespawnMarker as a child", origin + Vector(0, 11.0, 0), ARGB(255, 255, 60, 60));
	}
#endif

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

		// (No waypoint movement-speed setting any more: it only governs characters on foot, not a car's
		// driver. Vehicle speed is set with the cruise speed - see ApplyCruiseSpeed.)
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

		// Crew that got out (detained) are no longer children of the vehicle: collect them so they are removed too.
		// Anyone still in the car (in a seat, or parented to it) goes with the vehicle, so they are left out here:
		// deleting them a second time after the vehicle is gone crashed the game.
		array<IEntity> crewOnFoot = {};
		if (state.m_OccupantGroup)
		{
			array<AIAgent> crewAgents = {};
			state.m_OccupantGroup.GetAgents(crewAgents);
			foreach (AIAgent crewAgent : crewAgents)
			{
				if (!crewAgent)
					continue;

				IEntity crewEntity = crewAgent.GetControlledEntity();
				if (!crewEntity)
					continue;

				if (state.m_Vehicle && IsCrewInsideVehicle(crewEntity, state.m_Vehicle))
					continue;

				crewOnFoot.Insert(crewEntity);
			}
		}

		// Crew on foot first (one that was still walking to, or boarding, the car points at its compartment, so the car
		// must outlive it), then the vehicle (seated occupants go with it as children), then the group.
		foreach (IEntity crewMember : crewOnFoot)
		{
			if (crewMember)
				SCR_EntityHelper.DeleteEntityAndChildren(crewMember);
		}

		if (state.m_Vehicle)
			SCR_EntityHelper.DeleteEntityAndChildren(state.m_Vehicle);

		if (state.m_OccupantGroup)
			SCR_EntityHelper.DeleteEntityAndChildren(state.m_OccupantGroup);

		state.m_Vehicle = null;
		state.m_OccupantGroup = null;
	}

	//! True if the character is in one of the vehicle's seats or sits anywhere under it in the entity hierarchy.
	protected bool IsCrewInsideVehicle(IEntity character, IEntity vehicle)
	{
		if (!character || !vehicle)
			return false;

		if (CompartmentAccessComponent.GetVehicleIn(character) == vehicle)
			return true;

		IEntity parent = character.GetParent();
		while (parent)
		{
			if (parent == vehicle)
				return true;
			parent = parent.GetParent();
		}

		return false;
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

	protected ref array<IEntity> m_aMarkerQueryHits = new array<IEntity>();

	//! World-query callback: remember marker entities.
	protected bool QueryMarkerCallback(IEntity entity)
	{
		if (EEF_CheckpointMarkerEntity.Cast(entity))
			m_aMarkerQueryHits.Insert(entity);

		return true;
	}

	//! Collect the Spawn markers and the (first) Despawn marker among an entity's descendants, by TYPE.
	protected void CollectMarkers(IEntity parent, array<IEntity> spawns, inout IEntity despawn)
	{
		IEntity child = parent.GetChildren();
		while (child)
		{
			if (EEF_CheckpointSpawnMarker.Cast(child))
				spawns.Insert(child);
			else if (!despawn && EEF_CheckpointDespawnMarker.Cast(child))
				despawn = child;

			CollectMarkers(child, spawns, despawn);
			child = child.GetSibling();
		}
	}

	//! Find and cache the marker entities (children of this checkpoint). Returns false (and logs) if the
	//! checkpoint has no Spawn marker or no Despawn marker - callers skip their cycle.
	protected bool EnsurePointsResolved()
	{
		if (!m_aSpawnPoints.IsEmpty() && m_DespawnPoint)
			return true;

		m_aSpawnPoints.Clear();
		m_DespawnPoint = null;
		CollectMarkers(GetOwner(), m_aSpawnPoints, m_DespawnPoint);

		if (m_aSpawnPoints.IsEmpty() || !m_DespawnPoint)
		{
			// Diagnostic: what does the entity hierarchy really contain at runtime?
			int childCount = 0;
			IEntity child = GetOwner().GetChildren();
			while (child)
			{
				childCount++;
				DebugLog(string.Format("  hierarchy child %1: class %2 at %3", childCount, child.ClassName(), child.GetOrigin()));
				child = child.GetSibling();
			}
			DebugLog(string.Format("Hierarchy lookup: %1 child(ren), %2 Spawn marker(s), Despawn marker found: %3.", childCount, m_aSpawnPoints.Count(), m_DespawnPoint != null));

			// Fallback: markers lying near this checkpoint in the world, in case they are not attached to
			// it in the runtime hierarchy. (Limitation: with several checkpoints within 1.5 km this could
			// pick up another checkpoint's markers - the hierarchy is the intended mechanism.)
			m_aMarkerQueryHits.Clear();
			GetGame().GetWorld().QueryEntitiesBySphere(GetOwner().GetOrigin(), 1500.0, QueryMarkerCallback);
			foreach (IEntity hit : m_aMarkerQueryHits)
			{
				if (EEF_CheckpointSpawnMarker.Cast(hit) && m_aSpawnPoints.Find(hit) == -1)
					m_aSpawnPoints.Insert(hit);
				else if (!m_DespawnPoint && EEF_CheckpointDespawnMarker.Cast(hit))
					m_DespawnPoint = hit;
			}
			DebugLog(string.Format("World query fallback: %1 marker entity(ies) within 1500 m -> %2 Spawn marker(s), Despawn marker found: %3.", m_aMarkerQueryHits.Count(), m_aSpawnPoints.Count(), m_DespawnPoint != null));
		}

		if (m_aSpawnPoints.IsEmpty())
		{
			DebugLog("No Spawn marker found - add an EEF_CheckpointSpawnMarker as a child of the checkpoint.");
			return false;
		}

		if (!m_DespawnPoint)
		{
			DebugLog("No Despawn marker found - add an EEF_CheckpointDespawnMarker as a child of the checkpoint.");
			return false;
		}

		DebugLog(string.Format("Found %1 Spawn marker(s) and a Despawn marker.", m_aSpawnPoints.Count()));
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

	//! Pick one of the enabled checkpoint vehicles at random, or ResourceName.Empty if none is enabled.
	protected ResourceName PickVehiclePrefab()
	{
		array<ResourceName> valid = {};
		if (m_bUseS105)
			valid.Insert("{0E6CE01791978179}Prefabs/Vehicles/Wheeled/S105/Checkpoint_S105_randomized.et");
		if (m_bUseS1203)
			valid.Insert("{EEF0C0DE000C2011}Prefabs/Vehicles/Wheeled/S1203/Checkpoint_S1203_cargo_randomized.et");
		if (m_bUseT601)
			valid.Insert("{EEF0C0DE000C2012}Prefabs/Vehicles/Wheeled/T601/Checkpoint_T601_base.et");
		if (m_bUseUAZ452)
			valid.Insert("{EEF0C0DE000C2013}Prefabs/Vehicles/Wheeled/UAZ452/Checkpoint_UAZ452_cargo_CIV_randomized.et");
		if (m_bUseUAZ469)
			valid.Insert("{EEF0C0DE000C2014}Prefabs/Vehicles/Wheeled/UAZ469/Checkpoint_UAZ469_covered_CIV_randomized.et");
		if (m_bUseUralTransport)
			valid.Insert("{EEF0C0DE000C2015}Prefabs/Vehicles/Wheeled/Ural4320/Checkpoint_Ural4320_transport_CIV_randomized.et");
		if (m_bUseUralTanker)
			valid.Insert("{EEF0C0DE000C2016}Prefabs/Vehicles/Wheeled/Ural4320/Checkpoint_Ural4320_tanker_CIV_randomized.et");
		if (m_bUseV2101)
			valid.Insert("{EEF0C0DE000C2017}Prefabs/Vehicles/Wheeled/V2101/Checkpoint_V2101_base.et");

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
			// A detained car sits at the roadside; it must not stop new traffic from spawning.
			if (state && state.m_eState != EEF_ECheckpointVehicleState.DESPAWNED && state.m_eState != EEF_ECheckpointVehicleState.DETAINED)
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
