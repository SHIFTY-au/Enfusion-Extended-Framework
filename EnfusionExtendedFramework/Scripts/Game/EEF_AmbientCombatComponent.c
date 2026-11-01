// ============================================================
// EEF_AmbientCombatComponent.c
// Enfusion Extended Framework - Ambient Combat Module
//
// Simulates a wider conflict outside the players' line of sight using
// positional audio only. Mission makers place named marker entities for two
// opposing sides (Side A / Side B) and attach this component to an anchor
// entity. The SERVER runs a small "battle director" that pairs an A point with
// a nearby B point into an engagement, and emits bursts (rifle / MG / launcher)
// and explosions (single or barrage) for it, scaled by an intensity value.
//
// SYNC MODEL
//   - The server is the only brain. It never plays audio itself (dedicated).
//   - Each burst / barrage is ONE unreliable broadcast RPC carrying: sound
//     class index, world position, shot count, shot interval, a seed and a
//     start time stamped slightly in the future (server timestamp + lead).
//   - Every client expands that RPC locally into individual shots against the
//     replicated clock, so all players hear the same thing at the same time.
//     Shot-to-shot jitter and scatter come from a tiny integer LCG seeded by the
//     RPC, so they are identical on every machine without engine RNG seeding.
//   - Clients hold no battle state, so late joiners simply hear what is
//     happening now. Distance attenuation / delay / reverb is left to the
//     engine's audio landscape.
//
// SOUND PLAYBACK
//   Each sound class names a local-only "emitter" prefab (an entity with a
//   SoundComponent whose File Names list the wanted vanilla ACP) and an event
//   name. One emitter per class is spawned lazily per client and SoundEventTransform()
//   is used to play at the shot position, so the emitter never has to move.
//   One emitter per ACP is deliberate: a SoundComponent plays the FIRST sound
//   whose name matches, so two ACPs sharing an event name (e.g. SOUND_SHOT in
//   both AK-74 and M16A2) cannot live on the same emitter.
//   See Docs/AmbientCombat.md for the Workbench setup.
//
// REQUIREMENTS
//   - The anchor entity needs an RplComponent (RPCs route through it).
//   - Marker entities are found by name: <prefix><number>, number from 1,
//     gaps tolerated (same convention as EEF_GarrisonComponent). Any entity
//     type works; only its position is used.
//
// ------------------------------------------------------------
// IMPLEMENTATION NOTES / ASSUMPTIONS FLAGGED FOR IN-EDITOR VERIFICATION
// ------------------------------------------------------------
// Written without access to a Workbench compile. These API usages are from
// documentation / memory and are the first things to check if it fails to
// compile or behaves oddly:
//   - ChimeraWorld.GetServerTimestamp() + WorldTimestamp.PlusMilliseconds() /
//     DiffMilliseconds()                       shared clock used for sync. (Replication.Time()
//     does not exist in current builds - confirmed by a Workbench compile error.)
//     A WorldTimestamp is passed as an RPC parameter; if that is rejected at runtime,
//     fall back to sending a relative lead in ms (loses per-client latency equalisation).
//   - RplSession.Mode() / RplMode.Dedicated    dedicated-server detection
//   - [RplRpc(RplChannel.Unreliable, RplRcver.Broadcast)] with Rpc()
//     Broadcast is assumed NOT to run on the sending authority, so the
//     director calls the handler locally when the host is not dedicated. If
//     Broadcast turns out to also run locally on a listen-server host, shots
//     will double-play there - remove the local call in BroadcastPlay().
//   - GetGame().SpawnEntityPrefabLocal()        local-only emitter spawn
//   - SoundComponent.SoundEventTransform(string, vector[4])
//   - Math.Round() returning an int-compatible value
//   - CameraManager.CurrentCamera()             audible-range cull
// Event names (default SOUND_SHOT) and any signals the vanilla ACPs expect
// have NOT been confirmed - see Docs/AmbientCombat.md.
// ============================================================

//------------------------------------------------------------------------------------------------
//! Which side's markers a sound class may be fired from.
enum EEF_EAmbientCombatSide
{
	A,		//! Only fired from Side A points.
	B,		//! Only fired from Side B points.
	BOTH	//! Fired from either side.
}

//------------------------------------------------------------------------------------------------
//! Broad category - drives how the director schedules the class.
enum EEF_EAmbientCombatSoundType
{
	SMALL_ARMS,		//! Rifle bursts, fired from side points.
	MACHINE_GUN,	//! Longer MG bursts, fired from side points.
	LAUNCHER,		//! Rare launcher shots, fired from side points.
	EXPLOSION		//! Impacts. Fired on a rate set by the component; lands near a side point.
}

//------------------------------------------------------------------------------------------------
//! One playable sound class. Mission maker configures a list of these on the component.
[BaseContainerProps()]
class EEF_AmbientCombatSoundClass
{
	[Attribute("", UIWidgets.ResourcePickerThumbnail, "Local-only emitter prefab: an entity with a SoundComponent whose File Names list the ACP this class uses. Must NOT have an RplComponent.", "et")]
	ResourceName m_sEmitterPrefab;

	[Attribute("SOUND_SHOT", UIWidgets.EditBox, "Sound event (Sound node) name inside the emitter's ACP.")]
	string m_sEventName;

	[Attribute(EEF_EAmbientCombatSoundType.SMALL_ARMS.ToString(), UIWidgets.ComboBox, "Category.", "", ParamEnumArray.FromEnum(EEF_EAmbientCombatSoundType))]
	EEF_EAmbientCombatSoundType m_eType;

	[Attribute(EEF_EAmbientCombatSide.BOTH.ToString(), UIWidgets.ComboBox, "Which side may fire this. Ignored for EXPLOSION.", "", ParamEnumArray.FromEnum(EEF_EAmbientCombatSide))]
	EEF_EAmbientCombatSide m_eSide;

	[Attribute("1.0", UIWidgets.EditBox, "Relative chance of this class being chosen among those eligible.")]
	float m_fWeight;

	[Attribute("3", UIWidgets.EditBox, "Minimum shots per burst / impacts per barrage. 1 and 1 = always a single shot.")]
	int m_iMinCount;

	[Attribute("12", UIWidgets.EditBox, "Maximum shots per burst / impacts per barrage.")]
	int m_iMaxCount;

	[Attribute("90.0", UIWidgets.EditBox, "Minimum milliseconds between shots in a burst (spacing between impacts in a barrage).")]
	float m_fMinIntervalMs;

	[Attribute("160.0", UIWidgets.EditBox, "Maximum milliseconds between shots in a burst (spacing between impacts in a barrage).")]
	float m_fMaxIntervalMs;

	[Attribute("4.0", UIWidgets.EditBox, "Metres each shot may be scattered around the source point. Use a large value (30-80) for barrages.")]
	float m_fScatterRadius;

	[Attribute("4.0", UIWidgets.EditBox, "Weapons only - minimum seconds before this side fires another burst.")]
	float m_fMinGapSeconds;

	[Attribute("12.0", UIWidgets.EditBox, "Weapons only - maximum seconds before this side fires another burst.")]
	float m_fMaxGapSeconds;
}

//------------------------------------------------------------------------------------------------
//! Server-side state for one running engagement (an A point exchanging fire with a B point).
class EEF_AmbientCombatEngagement
{
	vector m_vPosA;
	vector m_vPosB;
	float m_fEndTime;		//! Worldtime (s) the engagement finishes.
	float m_fNextBurstA;	//! Worldtime (s) Side A fires next.
	float m_fNextBurstB;	//! Worldtime (s) Side B fires next.
}

//------------------------------------------------------------------------------------------------
[ComponentEditorProps(category: "EEF/AmbientCombat", description: "EEF Ambient Combat - attach to an anchor entity WITH an RplComponent. Place markers named <Side A prefix><n> and <Side B prefix><n> (n from 1). Simulates a synced distant battle using audio only.")]
class EEF_AmbientCombatComponentClass : ScriptComponentClass {}

//------------------------------------------------------------------------------------------------
class EEF_AmbientCombatComponent : ScriptComponent
{
	//--- Sources
	[Attribute("", UIWidgets.EditBox, "Side A marker name prefix. Markers named <prefix><number> from 1 (e.g. 'BattleA_' matches BattleA_1, BattleA_2...). Gaps tolerated.")]
	protected string m_sSideAMarkerPrefix;

	[Attribute("", UIWidgets.EditBox, "Side B marker name prefix. Same convention as Side A.")]
	protected string m_sSideBMarkerPrefix;

	//--- Sounds
	[Attribute("", UIWidgets.Object, "Sound classes. Add weapon classes (side A / B) and explosion classes.")]
	protected ref array<ref EEF_AmbientCombatSoundClass> m_aSoundClasses;

	//--- Battle shape
	[Attribute("0.5", UIWidgets.Slider, "Base battle intensity. 0 = silent, 1 = full battle. Can be changed at runtime with the SetAmbientCombatIntensity action.", "0 1 0.05")]
	protected float m_fIntensity;

	[Attribute("0.2", UIWidgets.Slider, "Intensity drifts randomly by up to +/- this amount, so the battle ebbs and flows.", "0 0.5 0.05")]
	protected float m_fIntensityVariance;

	[Attribute("3", UIWidgets.EditBox, "Maximum simultaneous engagements (at full intensity).")]
	protected int m_iMaxEngagements;

	[Attribute("20.0", UIWidgets.EditBox, "Minimum seconds an engagement lasts.")]
	protected float m_fMinEngagementDuration;

	[Attribute("60.0", UIWidgets.EditBox, "Maximum seconds an engagement lasts.")]
	protected float m_fMaxEngagementDuration;

	[Attribute("800.0", UIWidgets.EditBox, "A Side A point is paired with a Side B point within this many metres if one exists, otherwise the nearest.")]
	protected float m_fMaxEngagementRange;

	[Attribute("6.0", UIWidgets.EditBox, "Explosions per minute PER ENGAGEMENT at full intensity.")]
	protected float m_fExplosionsPerMinute;

	[Attribute("3000.0", UIWidgets.EditBox, "Clients skip sounds whose source is further than this from their camera. Saves CPU only - the engine still handles attenuation.")]
	protected float m_fMaxAudibleDistance;

	//--- Scenario framework
	[Attribute("0", UIWidgets.CheckBox, "Start automatically on world init, instead of waiting for EEF_ScenarioFrameworkActionStartAmbientCombat.")]
	protected bool m_bAutoActivateOnInit;

	//--- Debug
	[Attribute("0", UIWidgets.CheckBox, "Enable debug logging for this instance.")]
	protected bool m_bDebugEnabled;

	//--- Server runtime state
	protected bool m_bActive;
	protected bool m_bStarting;
	protected bool m_bTickRunning;
	protected int m_iDiscoveryAttempt;
	protected float m_fEffectiveIntensity;
	protected float m_fNextIntensityRoll;
	protected ref array<vector> m_aSideAPoints = {};
	protected ref array<vector> m_aSideBPoints = {};
	protected ref array<ref EEF_AmbientCombatEngagement> m_aEngagements = {};

	//--- Client runtime state
	protected ref array<IEntity> m_aEmitters = {};	//! One lazily spawned local emitter per sound class index.

	//--- Tuning constants (not mission-maker facing)
	protected const float TICK_INTERVAL_S = 0.5;			//! Director tick rate.
	protected const float SYNC_LEAD_MS = 400.0;				//! How far ahead of "now" the server stamps each burst, so slower clients still receive it in time.
	protected const float MAX_LATE_MS = 250.0;				//! A shot already this late when a client processes it is dropped instead of played out of sync.
	protected const float INTENSITY_REROLL_S = 12.0;		//! How often the effective intensity is re-rolled within +/- variance.
	protected const int MARKER_NAME_MAX_INDEX = 300;		//! Upper bound for <prefix><number> scan. Gaps tolerated.
	protected const int DISCOVERY_MAX_ATTEMPTS = 3;
	protected const float DISCOVERY_RETRY_INTERVAL_S = 2.0;

	//------------------------------------------------------------------------------------------------
	// INITIALISATION
	//------------------------------------------------------------------------------------------------

	//------------------------------------------------------------------------------------------------
	override void OnPostInit(IEntity owner)
	{
		super.OnPostInit(owner);

		if (!Replication.IsServer())
			return;

		if (!RplComponent.Cast(owner.FindComponent(RplComponent)))
			Print("[EEF AmbientCombat] ERROR: Anchor entity has no RplComponent - clients will never receive any sound. Add one to the anchor entity.", LogLevel.ERROR);

		if (m_bAutoActivateOnInit)
			GetGame().GetCallqueue().CallLater(StartBattle, 1000, false);

		DebugLog("Registered.");
	}

	//------------------------------------------------------------------------------------------------
	override void OnDelete(IEntity owner)
	{
		if (Replication.IsServer() && (m_bActive || m_bStarting))
			StopBattle();

		foreach (IEntity emitter : m_aEmitters)
		{
			if (emitter)
				SCR_EntityHelper.DeleteEntityAndChildren(emitter);
		}
		m_aEmitters.Clear();

		super.OnDelete(owner);
	}

	//------------------------------------------------------------------------------------------------
	// PUBLIC API (SERVER) - called by the EEF_ScenarioFrameworkAction* wrappers.
	//------------------------------------------------------------------------------------------------

	//------------------------------------------------------------------------------------------------
	//! Validates config, discovers markers and starts the director. No-op if already running.
	void StartBattle()
	{
		if (!Replication.IsServer())
			return;

		if (m_bActive || m_bStarting)
		{
			DebugLog("StartBattle ignored - already active.");
			return;
		}

		if (!ValidateConfig())
			return;

		m_bStarting = true;
		m_iDiscoveryAttempt = 0;
		AttemptDiscovery();
	}

	//------------------------------------------------------------------------------------------------
	//! Stops the director. Shots already scheduled on clients (a few seconds at most) play out.
	void StopBattle()
	{
		if (!Replication.IsServer())
			return;

		if (m_bStarting)
		{
			m_bStarting = false;
			GetGame().GetCallqueue().Remove(AttemptDiscovery);
		}

		if (m_bTickRunning)
		{
			GetGame().GetCallqueue().Remove(Tick);
			m_bTickRunning = false;
		}

		m_bActive = false;
		m_aEngagements.Clear();
		DebugLog("Battle stopped.");
	}

	//------------------------------------------------------------------------------------------------
	//! Sets the base intensity (0-1). Takes effect from the next tick.
	void SetIntensity(float intensity)
	{
		if (!Replication.IsServer())
			return;

		m_fIntensity = Math.Clamp(intensity, 0, 1);
		m_fNextIntensityRoll = 0;	// Force a re-roll against the new base on the next tick.
		DebugLog(string.Format("Intensity set to %1.", m_fIntensity));
	}

	//------------------------------------------------------------------------------------------------
	// DISCOVERY / VALIDATION (SERVER)
	//------------------------------------------------------------------------------------------------

	//------------------------------------------------------------------------------------------------
	protected bool ValidateConfig()
	{
		if (m_sSideAMarkerPrefix.IsEmpty() || m_sSideBMarkerPrefix.IsEmpty())
		{
			Print("[EEF AmbientCombat] ERROR: Both Side A and Side B marker prefixes must be set.", LogLevel.ERROR);
			return false;
		}

		if (!m_aSoundClasses || m_aSoundClasses.IsEmpty())
		{
			Print("[EEF AmbientCombat] ERROR: No sound classes configured.", LogLevel.ERROR);
			return false;
		}

		bool hasWeapon = false;
		foreach (int i, EEF_AmbientCombatSoundClass cls : m_aSoundClasses)
		{
			if (!cls || cls.m_sEmitterPrefab.IsEmpty() || cls.m_sEventName.IsEmpty())
			{
				Print(string.Format("[EEF AmbientCombat] ERROR: Sound class %1 is missing its emitter prefab or event name.", i), LogLevel.ERROR);
				return false;
			}

			if (cls.m_eType != EEF_EAmbientCombatSoundType.EXPLOSION)
				hasWeapon = true;
		}

		if (!hasWeapon)
		{
			Print("[EEF AmbientCombat] ERROR: At least one non-explosion sound class is required.", LogLevel.ERROR);
			return false;
		}

		return true;
	}

	//------------------------------------------------------------------------------------------------
	//! Retries the name scan a few times - defensive margin for a layer that streams in late.
	protected void AttemptDiscovery()
	{
		if (!m_bStarting)
			return;	// StopBattle cancelled activation while a retry was pending.

		m_iDiscoveryAttempt++;

		m_aSideAPoints.Clear();
		m_aSideBPoints.Clear();
		CollectPointsByName(m_sSideAMarkerPrefix, m_aSideAPoints);
		CollectPointsByName(m_sSideBMarkerPrefix, m_aSideBPoints);

		if (!m_aSideAPoints.IsEmpty() && !m_aSideBPoints.IsEmpty())
		{
			DebugLog(string.Format("Discovered %1 Side A and %2 Side B point(s) on attempt %3.", m_aSideAPoints.Count(), m_aSideBPoints.Count(), m_iDiscoveryAttempt));
			BeginBattle();
			return;
		}

		if (m_iDiscoveryAttempt >= DISCOVERY_MAX_ATTEMPTS)
		{
			m_bStarting = false;
			Print(string.Format("[EEF AmbientCombat] ERROR: Need at least one marker per side after %1 attempt(s). Found %2 for prefix '%3' and %4 for prefix '%5'. Names must be '<prefix><number>' from 1 (e.g. '%6').", m_iDiscoveryAttempt, m_aSideAPoints.Count(), m_sSideAMarkerPrefix, m_aSideBPoints.Count(), m_sSideBMarkerPrefix, string.Format("%1%2", m_sSideAMarkerPrefix, 1)), LogLevel.ERROR);
			return;
		}

		GetGame().GetCallqueue().CallLater(AttemptDiscovery, DISCOVERY_RETRY_INTERVAL_S * 1000, false);
	}

	//------------------------------------------------------------------------------------------------
	//! Collects the world position of every entity named <prefix><n> for n = 1..MARKER_NAME_MAX_INDEX.
	//! Scans the full range so gaps in numbering don't truncate discovery.
	protected void CollectPointsByName(string prefix, out array<vector> result)
	{
		for (int idx = 1; idx <= MARKER_NAME_MAX_INDEX; idx++)
		{
			IEntity ent = GetGame().GetWorld().FindEntityByName(string.Format("%1%2", prefix, idx));
			if (ent)
				result.Insert(ent.GetOrigin());
		}
	}

	//------------------------------------------------------------------------------------------------
	protected void BeginBattle()
	{
		m_bStarting = false;
		m_bActive = true;
		m_aEngagements.Clear();
		m_fNextIntensityRoll = 0;
		m_fEffectiveIntensity = m_fIntensity;

		if (!m_bTickRunning)
		{
			GetGame().GetCallqueue().CallLater(Tick, TICK_INTERVAL_S * 1000, true);
			m_bTickRunning = true;
		}

		DebugLog("Battle started.");
	}

	//------------------------------------------------------------------------------------------------
	// DIRECTOR (SERVER)
	//------------------------------------------------------------------------------------------------

	//------------------------------------------------------------------------------------------------
	protected void Tick()
	{
		if (!m_bActive)
			return;

		float now = GetCurrentTime();
		UpdateIntensity(now);

		// Retire finished engagements.
		for (int i = m_aEngagements.Count() - 1; i >= 0; i--)
		{
			if (now >= m_aEngagements[i].m_fEndTime)
				m_aEngagements.Remove(i);
		}

		// Top up to the intensity-driven target. Never drops running engagements early -
		// they simply aren't replaced once intensity falls, so the battle winds down naturally.
		int target = Math.Round(m_fEffectiveIntensity * m_iMaxEngagements);
		if (target < 1 && m_fEffectiveIntensity > 0.01)
			target = 1;

		while (m_aEngagements.Count() < target)
		{
			if (!StartEngagement(now))
				break;
		}

		foreach (EEF_AmbientCombatEngagement engagement : m_aEngagements)
			UpdateEngagement(engagement, now);
	}

	//------------------------------------------------------------------------------------------------
	protected void UpdateIntensity(float now)
	{
		if (now < m_fNextIntensityRoll)
			return;

		m_fNextIntensityRoll = now + INTENSITY_REROLL_S;
		m_fEffectiveIntensity = Math.Clamp(m_fIntensity + Math.RandomFloat(-m_fIntensityVariance, m_fIntensityVariance), 0, 1);
		DebugLog(string.Format("Effective intensity %1.", m_fEffectiveIntensity));
	}

	//------------------------------------------------------------------------------------------------
	//! Pairs a random Side A point with a Side B point within range (else the nearest).
	protected bool StartEngagement(float now)
	{
		if (m_aSideAPoints.IsEmpty() || m_aSideBPoints.IsEmpty())
			return false;

		vector a = m_aSideAPoints[Math.RandomInt(0, m_aSideAPoints.Count())];

		array<vector> inRange = {};
		vector nearest = m_aSideBPoints[0];
		float nearestDist = vector.Distance(a, nearest);

		foreach (vector b : m_aSideBPoints)
		{
			float dist = vector.Distance(a, b);
			if (dist <= m_fMaxEngagementRange)
				inRange.Insert(b);
			if (dist < nearestDist)
			{
				nearest = b;
				nearestDist = dist;
			}
		}

		EEF_AmbientCombatEngagement engagement = new EEF_AmbientCombatEngagement();
		engagement.m_vPosA = a;
		if (inRange.IsEmpty())
			engagement.m_vPosB = nearest;
		else
			engagement.m_vPosB = inRange[Math.RandomInt(0, inRange.Count())];

		engagement.m_fEndTime = now + Math.RandomFloat(m_fMinEngagementDuration, m_fMaxEngagementDuration);
		// Stagger the two sides so they don't open fire on the same tick.
		engagement.m_fNextBurstA = now + Math.RandomFloat(0, 3);
		engagement.m_fNextBurstB = now + Math.RandomFloat(0.5, 4);

		m_aEngagements.Insert(engagement);
		DebugLog(string.Format("Engagement started (%1 running).", m_aEngagements.Count()));
		return true;
	}

	//------------------------------------------------------------------------------------------------
	protected void UpdateEngagement(EEF_AmbientCombatEngagement e, float now)
	{
		if (now >= e.m_fNextBurstA)
		{
			float gapA = FireBurst(EEF_EAmbientCombatSide.A, e.m_vPosA);
			if (gapA < 0)
				gapA = 10;	// No eligible class - back off rather than re-rolling every tick.
			e.m_fNextBurstA = now + gapA;
		}

		if (now >= e.m_fNextBurstB)
		{
			float gapB = FireBurst(EEF_EAmbientCombatSide.B, e.m_vPosB);
			if (gapB < 0)
				gapB = 10;
			e.m_fNextBurstB = now + gapB;
		}

		// Explosions land near either side's position. Expected rate scales with intensity.
		float chance = m_fExplosionsPerMinute * m_fEffectiveIntensity * (TICK_INTERVAL_S / 60.0);
		if (Math.RandomFloat01() < chance)
		{
			vector impactCentre = e.m_vPosA;
			if (Math.RandomFloat01() < 0.5)
				impactCentre = e.m_vPosB;
			FireExplosion(impactCentre);
		}
	}

	//------------------------------------------------------------------------------------------------
	//! Fires one weapon burst from the given side. Returns the seconds until that side should fire
	//! again, or -1 if no class is eligible.
	protected float FireBurst(EEF_EAmbientCombatSide side, vector pos)
	{
		int idx = PickClassIndex(side, false);
		if (idx < 0)
			return -1;

		EEF_AmbientCombatSoundClass cls = m_aSoundClasses[idx];
		int count = Math.RandomInt(cls.m_iMinCount, cls.m_iMaxCount + 1);
		int intervalMs = Math.Round(Math.RandomFloat(cls.m_fMinIntervalMs, cls.m_fMaxIntervalMs));
		BroadcastPlay(idx, pos, count, intervalMs, Math.RandomInt(1, 65000), cls.m_fScatterRadius);

		// Higher intensity = shorter gaps between bursts.
		float gap = Math.RandomFloat(cls.m_fMinGapSeconds, cls.m_fMaxGapSeconds);
		return gap / (0.25 + 0.75 * m_fEffectiveIntensity);
	}

	//------------------------------------------------------------------------------------------------
	protected void FireExplosion(vector pos)
	{
		int idx = PickClassIndex(EEF_EAmbientCombatSide.BOTH, true);
		if (idx < 0)
			return;

		EEF_AmbientCombatSoundClass cls = m_aSoundClasses[idx];
		int count = Math.RandomInt(cls.m_iMinCount, cls.m_iMaxCount + 1);
		int intervalMs = Math.Round(Math.RandomFloat(cls.m_fMinIntervalMs, cls.m_fMaxIntervalMs));
		BroadcastPlay(idx, pos, count, intervalMs, Math.RandomInt(1, 65000), cls.m_fScatterRadius);
	}

	//------------------------------------------------------------------------------------------------
	//! Weighted random pick. explosion=true selects only EXPLOSION classes; otherwise only weapon
	//! classes whose side matches (or is BOTH). Returns -1 if none are eligible.
	protected int PickClassIndex(EEF_EAmbientCombatSide side, bool explosion)
	{
		float total = 0;
		foreach (EEF_AmbientCombatSoundClass cls : m_aSoundClasses)
		{
			if (IsEligible(cls, side, explosion))
				total += Math.Max(cls.m_fWeight, 0);
		}

		if (total <= 0)
			return -1;

		float roll = Math.RandomFloat(0, total);
		float acc = 0;
		int lastEligible = -1;
		foreach (int i, EEF_AmbientCombatSoundClass cls : m_aSoundClasses)
		{
			if (!IsEligible(cls, side, explosion))
				continue;

			lastEligible = i;
			acc += Math.Max(cls.m_fWeight, 0);
			if (roll <= acc)
				return i;
		}

		return lastEligible;
	}

	//------------------------------------------------------------------------------------------------
	protected bool IsEligible(EEF_AmbientCombatSoundClass cls, EEF_EAmbientCombatSide side, bool explosion)
	{
		if (!cls)
			return false;

		bool isExplosion = (cls.m_eType == EEF_EAmbientCombatSoundType.EXPLOSION);
		if (isExplosion != explosion)
			return false;

		if (explosion)
			return true;

		return (cls.m_eSide == EEF_EAmbientCombatSide.BOTH || cls.m_eSide == side);
	}

	//------------------------------------------------------------------------------------------------
	// NETWORK
	//------------------------------------------------------------------------------------------------

	//------------------------------------------------------------------------------------------------
	//! Stamps the burst with a start time slightly in the future and broadcasts it. On a
	//! non-dedicated host (listen server) the host is also a player, so it plays locally too.
	protected void BroadcastPlay(int classIndex, vector pos, int count, int intervalMs, int seed, float scatter)
	{
		ChimeraWorld world = ChimeraWorld.CastFrom(GetGame().GetWorld());
		if (!world)
			return;

		WorldTimestamp startTime = world.GetServerTimestamp().PlusMilliseconds(SYNC_LEAD_MS);
		Rpc(RpcDo_Play, classIndex, pos, count, intervalMs, seed, scatter, startTime);

		if (RplSession.Mode() != RplMode.Dedicated)
			RpcDo_Play(classIndex, pos, count, intervalMs, seed, scatter, startTime);

		DebugLog(string.Format("Burst: class %1, %2 shot(s) @ %3 ms, at %4.", classIndex, count, intervalMs, pos));
	}

	//------------------------------------------------------------------------------------------------
	//! CLIENT. Expands one burst/barrage into individual scheduled shots against the replicated clock.
	[RplRpc(RplChannel.Unreliable, RplRcver.Broadcast)]
	protected void RpcDo_Play(int classIndex, vector pos, int count, int intervalMs, int seed, float scatter, WorldTimestamp startTime)
	{
		if (RplSession.Mode() == RplMode.Dedicated)
			return;

		ChimeraWorld world = ChimeraWorld.CastFrom(GetGame().GetWorld());
		if (!world)
			return;

		if (!m_aSoundClasses || classIndex < 0 || classIndex >= m_aSoundClasses.Count())
			return;

		if (IsBeyondAudibleRange(pos))
			return;

		// Milliseconds from "now" (on the shared server clock) until the burst's start stamp.
		float baseDelayMs = startTime.DiffMilliseconds(world.GetServerTimestamp());
		int state = seed;

		for (int i = 0; i < count; i++)
		{
			// Deterministic jitter (+/-30% of the interval) and scatter - identical on every client.
			state = NextState(state);
			float jitterMs = (StateTo01(state) - 0.5) * 0.6 * intervalMs;

			state = NextState(state);
			float angle = StateTo01(state) * Math.PI2;
			state = NextState(state);
			float radius = StateTo01(state) * scatter;

			float delayMs = baseDelayMs + (i * intervalMs) + jitterMs;
			if (delayMs < -MAX_LATE_MS)
				continue;	// Missed it (late join / slow frame) - skip rather than play out of sync.

			vector shotPos = pos;
			shotPos[0] = shotPos[0] + Math.Cos(angle) * radius;
			shotPos[2] = shotPos[2] + Math.Sin(angle) * radius;

			GetGame().GetCallqueue().CallLater(PlayShot, Math.Max(delayMs, 0), false, classIndex, shotPos);
		}
	}

	//------------------------------------------------------------------------------------------------
	//! Tiny integer LCG (x * 75 + 74 mod 65537) - small enough that nothing overflows 32 bits,
	//! and fully deterministic across machines.
	protected static int NextState(int state)
	{
		return (state * 75 + 74) % 65537;
	}

	//------------------------------------------------------------------------------------------------
	protected static float StateTo01(int state)
	{
		return state / 65537.0;
	}

	//------------------------------------------------------------------------------------------------
	protected bool IsBeyondAudibleRange(vector pos)
	{
		CameraManager cameraManager = GetGame().GetCameraManager();
		if (!cameraManager)
			return false;

		CameraBase camera = cameraManager.CurrentCamera();
		if (!camera)
			return false;

		return vector.Distance(camera.GetOrigin(), pos) > m_fMaxAudibleDistance;
	}

	//------------------------------------------------------------------------------------------------
	// PLAYBACK (CLIENT)
	//------------------------------------------------------------------------------------------------

	//------------------------------------------------------------------------------------------------
	protected void PlayShot(int classIndex, vector pos)
	{
		if (!m_aSoundClasses || classIndex < 0 || classIndex >= m_aSoundClasses.Count())
			return;

		EEF_AmbientCombatSoundClass cls = m_aSoundClasses[classIndex];
		IEntity emitter = GetOrCreateEmitter(classIndex, cls);
		if (!emitter)
			return;

		SoundComponent soundComponent = SoundComponent.Cast(emitter.FindComponent(SoundComponent));
		if (!soundComponent)
			return;

		vector transform[4];
		Math3D.MatrixIdentity4(transform);
		transform[3] = pos;
		soundComponent.SoundEventTransform(cls.m_sEventName, transform);
	}

	//------------------------------------------------------------------------------------------------
	//! One local-only emitter per sound class, spawned on first use.
	protected IEntity GetOrCreateEmitter(int classIndex, EEF_AmbientCombatSoundClass cls)
	{
		while (m_aEmitters.Count() < m_aSoundClasses.Count())
			m_aEmitters.Insert(null);

		if (m_aEmitters[classIndex])
			return m_aEmitters[classIndex];

		Resource res = Resource.Load(cls.m_sEmitterPrefab);
		if (!res || !res.IsValid())
		{
			Print(string.Format("[EEF AmbientCombat] ERROR: Emitter prefab for sound class %1 could not be loaded: %2", classIndex, cls.m_sEmitterPrefab), LogLevel.ERROR);
			return null;
		}

		EntitySpawnParams spawnParams = new EntitySpawnParams();
		spawnParams.TransformMode = ETransformMode.WORLD;
		Math3D.MatrixIdentity4(spawnParams.Transform);
		spawnParams.Transform[3] = GetOwner().GetOrigin();

		IEntity emitter = GetGame().SpawnEntityPrefabLocal(res, GetGame().GetWorld(), spawnParams);
		if (!emitter)
		{
			Print(string.Format("[EEF AmbientCombat] ERROR: Emitter prefab for sound class %1 failed to spawn: %2", classIndex, cls.m_sEmitterPrefab), LogLevel.ERROR);
			return null;
		}

		m_aEmitters[classIndex] = emitter;
		return emitter;
	}

	//------------------------------------------------------------------------------------------------
	// HELPERS
	//------------------------------------------------------------------------------------------------

	//------------------------------------------------------------------------------------------------
	protected float GetCurrentTime()
	{
		return GetGame().GetWorld().GetWorldTime() / 1000.0;
	}

	//------------------------------------------------------------------------------------------------
	//! Log only when debug is enabled
	protected void DebugLog(string message)
	{
		if (m_bDebugEnabled)
			Print("[EEF AmbientCombat] " + message);
	}
}
