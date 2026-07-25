// ============================================================
// EEF_GarrisonComponent.c
// Enfusion Extended Framework - Interior/Compound AI Population Module
//
// Attach to a single "anchor" entity in the World Editor. Place
// AIWaypoint marker entities anywhere underneath it in the entity
// hierarchy (any nesting depth, any organisation) - every AIWaypoint
// found is collected as a possible garrison post. A random subset of
// those markers is populated with individual, independent AI on
// activation: each AI is spawned as its own standalone single-member
// AI group so it never coalesces toward a shared leader.
//
// STATIC mode holds each AI at its assigned marker indefinitely.
// MOVING mode has each AI loiter at a marker for a randomised dwell
// then move to another random unoccupied marker, with no shared
// route or synchronisation between AI.
//
// Activated via m_bAutoActivateOnInit (deferred start on world init,
// same pattern as EEF_PatrolComponent) and/or
// EEF_ScenarioFrameworkActionStartGarrison / StopGarrison. Stop always
// despawns and cleans up; a subsequent Start always rolls a brand new
// population - no state is persisted across Stop/Start.
//
// Runs on SERVER (authority) only.
//
// ------------------------------------------------------------
// IMPLEMENTATION NOTES / ASSUMPTIONS FLAGGED FOR IN-EDITOR VERIFICATION
// ------------------------------------------------------------
// The underlying design (see issue #14) explicitly flags several
// engine-behaviour questions as "needs research/testing" rather than
// guessed at blind. Two of them land on specific calls below:
//
//   - CreateSoloGroup() spawns a mission-maker-supplied "empty group"
//     prefab and calls SCR_AIGroup.AddAgent() to place a single
//     standalone character into it. This is the standard mechanism
//     for a scripted "group of one", but has not been confirmed
//     against a live Workbench session in this repo - verify the
//     empty group prefab you assign actually accepts AddAgent() with
//     no pre-authored members.
//   - ResolveAIAgent() looks up the AIAgent directly as a component on
//     the spawned character entity. If your character prefabs expose
//     the agent through a different accessor (e.g. via
//     AIControlComponent) this is the one line to adjust.
//
// Combat/investigate detection deliberately avoids guessing at an
// unconfirmed "is this AI personally engaged" API (Open Question #1
// in the issue) and instead polls each garrison AI's own
// SCR_CharacterDamageManagerComponent.GetHealthScaled() every tick -
// a health drop means "just took damage", which is a reliable,
// already-proven-in-this-codebase signal (see
// EEF_HelicopterControlComponent's damage-release logic) for "fired
// upon". It will not catch an AI that spots an enemy but has not yet
// been hit; that refinement depends on whatever perception API the
// Open Question research turns up.
// ============================================================

//------------------------------------------------------------------------------------------------
//! Garrison behaviour mode - single switch applied to every AI this instance controls.
enum EEF_EGarrisonMode
{
	STATIC,	//! Each AI holds its assigned marker indefinitely until investigate logic intervenes.
	MOVING	//! Each AI loiters at its marker then moves to another random unoccupied marker.
}

//------------------------------------------------------------------------------------------------
//! Defines a single individual character prefab slot - mission maker selects prefab.
//! Must resolve to a single controllable character, not a group prefab.
[BaseContainerProps()]
class EEF_GarrisonCharacterSlot
{
	[Attribute("", UIWidgets.ResourcePickerThumbnail, "Individual character prefab (not a group).", "et")]
	ResourceName m_sCharacterPrefab;
}

//------------------------------------------------------------------------------------------------
//! Runtime state for a single spawned garrison AI.
class EEF_GarrisonAIState
{
	SCR_AIGroup m_Group;								//! The standalone "group of one" wrapping m_Agent.
	AIAgent m_Agent;									//! The spawned character's AI agent.
	IEntity m_Character;								//! The spawned character entity.
	SCR_CharacterDamageManagerComponent m_DamageManager;	//! Cached for per-tick health polling.

	AIWaypoint m_HomeMarker;		//! STATIC: fixed post. MOVING: initial marker only (no fixed home).
	AIWaypoint m_CurrentMarker;		//! Marker this AI currently occupies. Null while in transit.
	AIWaypoint m_TargetMarker;		//! Marker this AI is currently travelling to. Null when settled.

	float m_fNextMoveTime;			//! MOVING mode: worldtime (s) at which the loiter dwell ends.

	float m_fLastHealthScaled;		//! Last observed GetHealthScaled() value, for damage-delta detection.
	bool m_bEngaged;				//! True while personally in combat (recently took damage).
	float m_fLastDamageTime;		//! Worldtime (s) of the last detected personal damage.

	bool m_bInvestigating;			//! True while responding to a nearby fight.
	vector m_vInvestigateOrigin;	//! Position of the fight being investigated (also the leash anchor).
	float m_fInvestigateStartTime;	//! Worldtime (s) investigation began - drives the minimum dwell.
}

//------------------------------------------------------------------------------------------------
[ComponentEditorProps(category: "EEF/Garrison", description: "EEF Garrison - attach to an anchor entity with AIWaypoint markers placed anywhere underneath it. Populates a random subset with independent standalone AI.")]
class EEF_GarrisonComponentClass : ScriptComponentClass {}

//------------------------------------------------------------------------------------------------
class EEF_GarrisonComponent : ScriptComponent
{
	//--- Population
	[Attribute("", UIWidgets.Object, "Individual character prefabs. One is picked at random per spawn.")]
	protected ref array<ref EEF_GarrisonCharacterSlot> m_aCharacterPrefabs;

	[Attribute("", UIWidgets.ResourcePickerThumbnail, "AI group prefab with no pre-placed members - used as the standalone container for each spawned AI's 'group of one'.", "et")]
	protected ResourceName m_sEmptyGroupPrefab;

	[Attribute("1", UIWidgets.EditBox, "Minimum number of AI to spawn on activation.")]
	protected int m_iMinSpawnCount;

	[Attribute("4", UIWidgets.EditBox, "Maximum number of AI to spawn on activation.")]
	protected int m_iMaxSpawnCount;

	//--- Behaviour mode
	[Attribute(EEF_EGarrisonMode.STATIC.ToString(), UIWidgets.ComboBox, "Behaviour mode applied to every AI this instance controls.", "", ParamEnumArray.FromEnum(EEF_EGarrisonMode))]
	protected EEF_EGarrisonMode m_eGarrisonMode;

	[Attribute("20.0", UIWidgets.EditBox, "MOVING mode only - minimum seconds an AI loiters at a marker before moving.")]
	protected float m_fMinMoveInterval;

	[Attribute("90.0", UIWidgets.EditBox, "MOVING mode only - maximum seconds an AI loiters at a marker before moving.")]
	protected float m_fMaxMoveInterval;

	//--- Combat / investigate
	[Attribute("40.0", UIWidgets.EditBox, "Distance in metres from a fight within which bystander AI of this instance are eligible to investigate.")]
	protected float m_fInvestigateRadius;

	[Attribute("50.0", UIWidgets.EditBox, "Cap on the percentage of eligible bystanders that actually break off to investigate.")]
	protected float m_fInvestigatePercentage;

	[Attribute("60.0", UIWidgets.EditBox, "Distance boundary combined with calmed alert state used to decide when an investigating AI returns.")]
	protected float m_fLeashDistance;

	//--- Scenario framework
	[Attribute("0", UIWidgets.CheckBox, "Start automatically once the layer/world initialises, instead of waiting for EEF_ScenarioFrameworkActionStartGarrison.")]
	protected bool m_bAutoActivateOnInit;

	//--- Debug
	[Attribute("0", UIWidgets.CheckBox, "Enable debug logging for this garrison instance.")]
	protected bool m_bDebugEnabled;

	//--- Runtime state
	protected bool m_bActive;
	protected bool m_bTickRunning;
	protected ref array<AIWaypoint> m_aMarkers = {};
	protected ref array<ref EEF_GarrisonAIState> m_aActiveAI = {};

	//--- Tuning constants (not mission-maker facing - internal behaviour timing)
	protected const float GARRISON_TICK_INTERVAL_S = 1.0;		//! Poll rate for engagement/move/investigate logic.
	protected const float ENGAGED_COOLDOWN_SECONDS = 8.0;		//! Time since last damage before an AI is no longer "personally engaged".
	protected const float MIN_INVESTIGATE_DWELL_SECONDS = 20.0;	//! Minimum time a pure bystander stays at the investigate point before giving up.
	protected const float HEALTH_DROP_EPSILON = 0.01;			//! Minimum GetHealthScaled() delta to count as "took damage".

	//------------------------------------------------------------------------------------------------
	// INITIALISATION
	//------------------------------------------------------------------------------------------------

	//------------------------------------------------------------------------------------------------
	override void OnPostInit(IEntity owner)
	{
		super.OnPostInit(owner);

		if (!Replication.IsServer())
			return;

		if (m_bAutoActivateOnInit)
			GetGame().GetCallqueue().CallLater(StartGarrison, 1000, false);

		DebugLog("Registered.");
	}

	//------------------------------------------------------------------------------------------------
	override void OnDelete(IEntity owner)
	{
		if (Replication.IsServer() && m_bActive)
			StopGarrison(true);

		super.OnDelete(owner);
	}

	//------------------------------------------------------------------------------------------------
	// PUBLIC API - called by EEF_ScenarioFrameworkActionStartGarrison / StopGarrison.
	//------------------------------------------------------------------------------------------------

	//------------------------------------------------------------------------------------------------
	//! Validates config, discovers markers, rolls and spawns a brand new population, and starts ticking.
	//! Always a full reset - a subsequent Start after Stop rolls fresh random count and marker selection.
	void StartGarrison()
	{
		if (!Replication.IsServer())
			return;

		if (m_bActive)
		{
			DebugLog("StartGarrison ignored - already active.");
			return;
		}

		if (!ValidateCharacterPrefabs())
		{
			Print("[EEF Garrison] ERROR: Character prefab validation failed. Instance will not activate.", LogLevel.ERROR);
			return;
		}

		if (m_sEmptyGroupPrefab.IsEmpty())
		{
			Print("[EEF Garrison] ERROR: No empty group prefab configured (m_sEmptyGroupPrefab). Instance will not activate.", LogLevel.ERROR);
			return;
		}

		m_aMarkers.Clear();
		CollectMarkersRecursive(GetOwner(), m_aMarkers);

		if (m_aMarkers.IsEmpty())
		{
			Print("[EEF Garrison] ERROR: No AIWaypoint markers found under owner entity. Place at least one.", LogLevel.ERROR);
			return;
		}

		DebugLog(string.Format("Discovered %1 marker(s).", m_aMarkers.Count()));

		int spawnCount = Math.RandomInt(m_iMinSpawnCount, m_iMaxSpawnCount + 1);
		spawnCount = Math.Clamp(spawnCount, 0, m_aMarkers.Count());

		array<AIWaypoint> shuffled = {};
		foreach (AIWaypoint marker : m_aMarkers)
			shuffled.Insert(marker);
		ShuffleMarkers(shuffled);

		m_aActiveAI.Clear();

		for (int i = 0; i < spawnCount; i++)
			SpawnGarrisonAI(shuffled[i]);

		m_bActive = true;

		if (!m_bTickRunning)
		{
			GetGame().GetCallqueue().CallLater(Tick, GARRISON_TICK_INTERVAL_S * 1000, true);
			m_bTickRunning = true;
		}

		DebugLog(string.Format("Garrison started - %1 AI spawned across %2 marker(s).", m_aActiveAI.Count(), m_aMarkers.Count()));
	}

	//------------------------------------------------------------------------------------------------
	//! Deactivates the instance. cleanup=true (default) despawns all currently active AI, matching
	//! EEF_HunterSpawnerComponent.StopHunter(). Markers themselves (mission-placed) are never touched.
	void StopGarrison(bool cleanup = true)
	{
		if (!Replication.IsServer())
			return;

		if (!m_bActive)
			return;

		m_bActive = false;
		StopTick();

		if (cleanup)
		{
			foreach (EEF_GarrisonAIState state : m_aActiveAI)
			{
				if (!state)
					continue;
				if (state.m_Group)
					SCR_EntityHelper.DeleteEntityAndChildren(state.m_Group);
				if (state.m_Character)
					SCR_EntityHelper.DeleteEntityAndChildren(state.m_Character);
			}
		}

		m_aActiveAI.Clear();
		DebugLog("Garrison stopped.");
	}

	//------------------------------------------------------------------------------------------------
	// MARKER DISCOVERY
	//------------------------------------------------------------------------------------------------

	//------------------------------------------------------------------------------------------------
	//! Recursively collects every AIWaypoint descendant of node, at any nesting depth.
	//! No naming/prefix convention - scoping is entirely by parentage under the anchor.
	protected void CollectMarkersRecursive(IEntity node, out array<AIWaypoint> result)
	{
		IEntity child = node.GetChildren();
		while (child)
		{
			AIWaypoint wp = AIWaypoint.Cast(child);
			if (wp)
				result.Insert(wp);

			CollectMarkersRecursive(child, result);
			child = child.GetSibling();
		}
	}

	//------------------------------------------------------------------------------------------------
	// VALIDATION
	//------------------------------------------------------------------------------------------------

	//------------------------------------------------------------------------------------------------
	//! Hard validation - each configured slot must resolve to a single controllable character, not a
	//! group. Spawns a throwaway instance of each slot to inspect it, then deletes it immediately.
	//! Any misconfigured slot fails the ENTIRE activation with a clear error identifying the slot.
	protected bool ValidateCharacterPrefabs()
	{
		if (!m_aCharacterPrefabs || m_aCharacterPrefabs.IsEmpty())
		{
			Print("[EEF Garrison] ERROR: No character prefabs configured (m_aCharacterPrefabs).", LogLevel.ERROR);
			return false;
		}

		for (int i = 0; i < m_aCharacterPrefabs.Count(); i++)
		{
			EEF_GarrisonCharacterSlot slot = m_aCharacterPrefabs[i];
			if (!slot || slot.m_sCharacterPrefab.IsEmpty())
			{
				Print(string.Format("[EEF Garrison] ERROR: Character prefab slot %1 is empty.", i), LogLevel.ERROR);
				return false;
			}

			Resource res = Resource.Load(slot.m_sCharacterPrefab);
			if (!res || !res.IsValid())
			{
				Print(string.Format("[EEF Garrison] ERROR: Character prefab slot %1 could not be loaded: %2", i, slot.m_sCharacterPrefab), LogLevel.ERROR);
				return false;
			}

			EntitySpawnParams spawnParams = new EntitySpawnParams();
			spawnParams.TransformMode = ETransformMode.WORLD;
			Math3D.MatrixIdentity4(spawnParams.Transform);
			spawnParams.Transform[3] = GetOwner().GetOrigin();

			IEntity testEntity = GetGame().SpawnEntityPrefab(res, GetGame().GetWorld(), spawnParams);
			if (!testEntity)
			{
				Print(string.Format("[EEF Garrison] ERROR: Character prefab slot %1 failed to spawn for validation: %2", i, slot.m_sCharacterPrefab), LogLevel.ERROR);
				return false;
			}

			bool isGroup = SCR_AIGroup.Cast(testEntity) != null;
			AIAgent agent = ResolveAIAgent(testEntity);

			SCR_EntityHelper.DeleteEntityAndChildren(testEntity);

			if (isGroup)
			{
				Print(string.Format("[EEF Garrison] ERROR: Character prefab slot %1 resolves to an AI group, not an individual character: %2. Both share the .et extension in the resource picker - double-check the selection.", i, slot.m_sCharacterPrefab), LogLevel.ERROR);
				return false;
			}

			if (!agent)
			{
				Print(string.Format("[EEF Garrison] ERROR: Character prefab slot %1 does not resolve to a controllable character (no AIAgent found): %2", i, slot.m_sCharacterPrefab), LogLevel.ERROR);
				return false;
			}
		}

		return true;
	}

	//------------------------------------------------------------------------------------------------
	// SPAWNING
	//------------------------------------------------------------------------------------------------

	//------------------------------------------------------------------------------------------------
	//! Spawns one garrison AI at marker: picks a random character prefab, spawns it, wraps it in a
	//! standalone "group of one", and assigns the marker as its first destination/post.
	protected void SpawnGarrisonAI(AIWaypoint marker)
	{
		ResourceName prefab = PickRandomCharacterPrefab();
		if (prefab.IsEmpty())
			return;

		IEntity character = SpawnCharacterAtMarker(prefab, marker);
		if (!character)
		{
			Print("[EEF Garrison] ERROR: Failed to spawn character for garrison AI.", LogLevel.ERROR);
			return;
		}

		AIAgent agent = ResolveAIAgent(character);
		if (!agent)
		{
			Print("[EEF Garrison] ERROR: Spawned character has no AIAgent - cannot form group. Deleting.", LogLevel.ERROR);
			SCR_EntityHelper.DeleteEntityAndChildren(character);
			return;
		}

		SCR_AIGroup group = CreateSoloGroup(marker.GetOrigin());
		if (!group)
		{
			Print("[EEF Garrison] ERROR: Failed to create solo group for garrison AI. Deleting character.", LogLevel.ERROR);
			SCR_EntityHelper.DeleteEntityAndChildren(character);
			return;
		}

		group.AddAgent(agent);
		group.GetOnWaypointCompleted().Insert(OnWaypointCompleted);

		EEF_GarrisonAIState state = new EEF_GarrisonAIState();
		state.m_Group = group;
		state.m_Agent = agent;
		state.m_Character = character;
		state.m_HomeMarker = marker;
		state.m_DamageManager = SCR_CharacterDamageManagerComponent.Cast(character.FindComponent(SCR_CharacterDamageManagerComponent));

		if (state.m_DamageManager)
			state.m_fLastHealthScaled = state.m_DamageManager.GetHealthScaled();
		else
			state.m_fLastHealthScaled = 1.0;

		m_aActiveAI.Insert(state);
		AssignMove(state, marker);

		DebugLog(string.Format("Spawned garrison AI at marker %1.", marker.GetOrigin().ToString()));
	}

	//------------------------------------------------------------------------------------------------
	protected ResourceName PickRandomCharacterPrefab()
	{
		if (!m_aCharacterPrefabs || m_aCharacterPrefabs.IsEmpty())
			return ResourceName.Empty;

		int idx = Math.RandomInt(0, m_aCharacterPrefabs.Count());
		EEF_GarrisonCharacterSlot slot = m_aCharacterPrefabs[idx];
		if (!slot)
			return ResourceName.Empty;

		return slot.m_sCharacterPrefab;
	}

	//------------------------------------------------------------------------------------------------
	//! Spawns the character prefab using the marker's full world transform (position + rotation),
	//! so STATIC AI face whichever direction the mission maker rotated the marker to in the editor.
	protected IEntity SpawnCharacterAtMarker(ResourceName prefab, AIWaypoint marker)
	{
		Resource res = Resource.Load(prefab);
		if (!res || !res.IsValid())
			return null;

		EntitySpawnParams spawnParams = new EntitySpawnParams();
		spawnParams.TransformMode = ETransformMode.WORLD;
		marker.GetWorldTransform(spawnParams.Transform);

		return GetGame().SpawnEntityPrefab(res, GetGame().GetWorld(), spawnParams);
	}

	//------------------------------------------------------------------------------------------------
	//! Retrieves the AIAgent for a spawned character. See header note - this is the one accessor
	//! most likely to need a one-line adjustment once verified against the target Workbench version.
	protected AIAgent ResolveAIAgent(IEntity character)
	{
		return AIAgent.Cast(character.FindComponent(AIAgent));
	}

	//------------------------------------------------------------------------------------------------
	//! Spawns an empty group prefab to act as the standalone "group of one" container for a single
	//! spawned character. See header note - verify m_sEmptyGroupPrefab has no pre-authored members.
	protected SCR_AIGroup CreateSoloGroup(vector position)
	{
		Resource groupRes = Resource.Load(m_sEmptyGroupPrefab);
		if (!groupRes || !groupRes.IsValid())
		{
			Print("[EEF Garrison] ERROR: Could not load empty group prefab: " + m_sEmptyGroupPrefab, LogLevel.ERROR);
			return null;
		}

		EntitySpawnParams spawnParams = new EntitySpawnParams();
		spawnParams.TransformMode = ETransformMode.WORLD;
		Math3D.MatrixIdentity4(spawnParams.Transform);
		spawnParams.Transform[3] = position;

		return SCR_AIGroup.Cast(GetGame().SpawnEntityPrefab(groupRes, GetGame().GetWorld(), spawnParams));
	}

	//------------------------------------------------------------------------------------------------
	// PER-TICK LOGIC
	//------------------------------------------------------------------------------------------------

	//------------------------------------------------------------------------------------------------
	protected void Tick()
	{
		if (!m_bActive)
			return;

		float now = GetCurrentTime();

		for (int i = m_aActiveAI.Count() - 1; i >= 0; i--)
		{
			EEF_GarrisonAIState state = m_aActiveAI[i];

			if (!IsStateAlive(state))
			{
				// Permanent attrition - dead AI are not replaced. Corpse is left in the world;
				// only StopGarrison(true) does a full despawn of remaining living AI.
				DebugLog("Garrison AI died - permanent attrition, marker freed.");
				m_aActiveAI.Remove(i);
				continue;
			}

			UpdateEngagement(state, now);

			if (state.m_bEngaged)
				continue; // Withhold new orders entirely while personally fighting - native AI owns combat.

			if (state.m_bInvestigating)
			{
				TickInvestigating(state, now);
				continue;
			}

			if (m_eGarrisonMode == EEF_EGarrisonMode.MOVING)
				TickMoving(state, now);
		}
	}

	//------------------------------------------------------------------------------------------------
	protected bool IsStateAlive(EEF_GarrisonAIState state)
	{
		if (!state || !state.m_Character)
			return false;

		if (state.m_DamageManager && state.m_DamageManager.IsDestroyed())
			return false;

		return true;
	}

	//------------------------------------------------------------------------------------------------
	//! Detects "personally engaged" via a health drop since last tick (see header note). Also handles
	//! the calm-down transition and fires the one-shot investigate broadcast on new engagement.
	protected void UpdateEngagement(EEF_GarrisonAIState state, float now)
	{
		float currentHealth = 1.0;
		if (state.m_DamageManager)
			currentHealth = state.m_DamageManager.GetHealthScaled();

		if (currentHealth < state.m_fLastHealthScaled - HEALTH_DROP_EPSILON)
		{
			bool wasEngaged = state.m_bEngaged;
			state.m_bEngaged = true;
			state.m_fLastDamageTime = now;

			if (!wasEngaged)
			{
				DebugLog("Garrison AI took damage - now personally engaged.");
				TriggerInvestigateBroadcast(state, now);
			}
		}
		else if (state.m_bEngaged && (now - state.m_fLastDamageTime) > ENGAGED_COOLDOWN_SECONDS)
		{
			state.m_bEngaged = false;
			DebugLog("Garrison AI calmed down from personal engagement.");
		}

		state.m_fLastHealthScaled = currentHealth;
	}

	//------------------------------------------------------------------------------------------------
	//! MOVING mode: loiter-then-move cycle. No fixed order, no looping route, not synchronised
	//! across AI - deliberately avoids clumping or a predictable patrol circuit.
	protected void TickMoving(EEF_GarrisonAIState state, float now)
	{
		if (state.m_TargetMarker)
			return; // Already in transit - arrival is handled by OnWaypointCompleted.

		if (now < state.m_fNextMoveTime)
			return;

		AIWaypoint next = PickRandomUnoccupiedMarker(state);
		if (!next)
		{
			// No free marker right now - retry shortly rather than every tick indefinitely.
			state.m_fNextMoveTime = now + GARRISON_TICK_INTERVAL_S;
			return;
		}

		AssignMove(state, next);
		DebugLog("Garrison AI (MOVING) moving to new marker.");
	}

	//------------------------------------------------------------------------------------------------
	//! Return-to-normal: leash distance from the investigation point, or calmed alert state combined
	//! with a minimum dwell for bystanders who were never personally hit. STATIC AI return to their
	//! own fixed post; MOVING AI resume the pool via the nearest unoccupied marker to the fight.
	protected void TickInvestigating(EEF_GarrisonAIState state, float now)
	{
		if (state.m_TargetMarker)
			return; // Still travelling to the investigate point / home marker.

		vector delta = state.m_Character.GetOrigin() - state.m_vInvestigateOrigin;
		bool leashExceeded = delta.Length() > m_fLeashDistance;

		bool calmedDown = (now - state.m_fLastDamageTime) > ENGAGED_COOLDOWN_SECONDS;
		bool dwellElapsed = (now - state.m_fInvestigateStartTime) > MIN_INVESTIGATE_DWELL_SECONDS;

		if (leashExceeded || (calmedDown && dwellElapsed))
			ReturnFromInvestigate(state);
	}

	//------------------------------------------------------------------------------------------------
	// COMBAT / INVESTIGATE
	//------------------------------------------------------------------------------------------------

	//------------------------------------------------------------------------------------------------
	//! Fires once when originState newly becomes personally engaged. Finds eligible bystanders of
	//! THIS instance within m_fInvestigateRadius and sends up to m_fInvestigatePercentage% of them
	//! to respond - the rest hold their current behaviour undisturbed.
	protected void TriggerInvestigateBroadcast(EEF_GarrisonAIState originState, float now)
	{
		vector origin = originState.m_Character.GetOrigin();
		array<ref EEF_GarrisonAIState> eligible = {};

		foreach (EEF_GarrisonAIState other : m_aActiveAI)
		{
			if (other == originState || !other || !other.m_Character)
				continue;
			if (other.m_bEngaged || other.m_bInvestigating)
				continue;

			vector delta = other.m_Character.GetOrigin() - origin;
			if (delta.Length() <= m_fInvestigateRadius)
				eligible.Insert(other);
		}

		if (eligible.IsEmpty())
			return;

		int investigateCount = Math.Round(eligible.Count() * (m_fInvestigatePercentage / 100.0));
		investigateCount = Math.Clamp(investigateCount, 0, eligible.Count());

		if (investigateCount <= 0)
			return;

		ShuffleStates(eligible);

		for (int i = 0; i < investigateCount; i++)
			StartInvestigate(eligible[i], origin, now);

		DebugLog(string.Format("Investigate triggered - %1 of %2 eligible bystander(s) responding.", investigateCount, eligible.Count()));
	}

	//------------------------------------------------------------------------------------------------
	protected void StartInvestigate(EEF_GarrisonAIState state, vector origin, float now)
	{
		state.m_bInvestigating = true;
		state.m_vInvestigateOrigin = origin;
		state.m_fInvestigateStartTime = now;

		AIWaypoint target = PickNearestUnoccupiedMarker(origin, state);
		if (target)
			AssignMove(state, target);
		else
			DebugLog("Investigate: no free marker near investigation point - holding current position.");
	}

	//------------------------------------------------------------------------------------------------
	protected void ReturnFromInvestigate(EEF_GarrisonAIState state)
	{
		state.m_bInvestigating = false;

		AIWaypoint target;
		if (m_eGarrisonMode == EEF_EGarrisonMode.STATIC)
			target = state.m_HomeMarker;
		else
			target = PickNearestUnoccupiedMarker(state.m_vInvestigateOrigin, state);

		if (!target)
			target = state.m_HomeMarker; // Fallback so the AI always has somewhere to go.

		if (target)
			AssignMove(state, target);

		DebugLog("Garrison AI returning from investigation.");
	}

	//------------------------------------------------------------------------------------------------
	// WAYPOINT / MARKER HELPERS
	//------------------------------------------------------------------------------------------------

	//------------------------------------------------------------------------------------------------
	//! Clears the group's current waypoint queue and assigns marker directly - markers are used as
	//! the destination pool as-is, never copied. See header note re: shared waypoint entity reuse.
	protected void AssignMove(EEF_GarrisonAIState state, AIWaypoint marker)
	{
		if (!state || !state.m_Group || !marker)
			return;

		ClearGroupWaypoints(state.m_Group);
		state.m_Group.AddWaypoint(marker);
		state.m_TargetMarker = marker;
		state.m_CurrentMarker = null;
	}

	//------------------------------------------------------------------------------------------------
	protected void ClearGroupWaypoints(SCR_AIGroup group)
	{
		array<AIWaypoint> queue = {};
		group.GetWaypoints(queue);
		foreach (AIWaypoint wp : queue)
			group.RemoveWaypoint(wp);
	}

	//------------------------------------------------------------------------------------------------
	//! Fired when any of our groups completes its current waypoint. Marks the owning state as
	//! arrived and, in MOVING mode outside of an investigate response, rolls the next loiter timer.
	protected void OnWaypointCompleted(AIWaypoint wp)
	{
		foreach (EEF_GarrisonAIState state : m_aActiveAI)
		{
			if (state.m_TargetMarker != wp)
				continue;

			state.m_CurrentMarker = wp;
			state.m_TargetMarker = null;

			if (m_eGarrisonMode == EEF_EGarrisonMode.MOVING && !state.m_bInvestigating)
				state.m_fNextMoveTime = GetCurrentTime() + Math.RandomFloat(m_fMinMoveInterval, m_fMaxMoveInterval);

			return;
		}
	}

	//------------------------------------------------------------------------------------------------
	//! A marker is unavailable if any active AI (other than requester) currently occupies it or is
	//! travelling toward it. Prevents two AI ever occupying - or converging on - the same marker.
	protected bool IsMarkerAvailable(AIWaypoint marker, EEF_GarrisonAIState requester)
	{
		foreach (EEF_GarrisonAIState state : m_aActiveAI)
		{
			if (state == requester)
				continue;
			if (state.m_CurrentMarker == marker || state.m_TargetMarker == marker)
				return false;
		}

		return true;
	}

	//------------------------------------------------------------------------------------------------
	protected AIWaypoint PickRandomUnoccupiedMarker(EEF_GarrisonAIState requester)
	{
		array<AIWaypoint> candidates = {};
		foreach (AIWaypoint marker : m_aMarkers)
		{
			if (IsMarkerAvailable(marker, requester))
				candidates.Insert(marker);
		}

		if (candidates.IsEmpty())
			return null;

		return candidates[Math.RandomInt(0, candidates.Count())];
	}

	//------------------------------------------------------------------------------------------------
	protected AIWaypoint PickNearestUnoccupiedMarker(vector fromPos, EEF_GarrisonAIState requester)
	{
		AIWaypoint best = null;
		float bestDistSq = float.MAX;

		foreach (AIWaypoint marker : m_aMarkers)
		{
			if (!IsMarkerAvailable(marker, requester))
				continue;

			vector delta = marker.GetOrigin() - fromPos;
			float distSq = delta.LengthSq();
			if (distSq < bestDistSq)
			{
				bestDistSq = distSq;
				best = marker;
			}
		}

		return best;
	}

	//------------------------------------------------------------------------------------------------
	// UTILITY
	//------------------------------------------------------------------------------------------------

	//------------------------------------------------------------------------------------------------
	protected void ShuffleMarkers(array<AIWaypoint> arr)
	{
		for (int i = arr.Count() - 1; i > 0; i--)
		{
			int j = Math.RandomInt(0, i + 1);
			AIWaypoint temp = arr[i];
			arr[i] = arr[j];
			arr[j] = temp;
		}
	}

	//------------------------------------------------------------------------------------------------
	protected void ShuffleStates(array<ref EEF_GarrisonAIState> arr)
	{
		for (int i = arr.Count() - 1; i > 0; i--)
		{
			int j = Math.RandomInt(0, i + 1);
			EEF_GarrisonAIState temp = arr[i];
			arr[i] = arr[j];
			arr[j] = temp;
		}
	}

	//------------------------------------------------------------------------------------------------
	protected float GetCurrentTime()
	{
		return GetGame().GetWorld().GetWorldTime() / 1000.0;
	}

	//------------------------------------------------------------------------------------------------
	protected void StopTick()
	{
		if (!m_bTickRunning)
			return;

		GetGame().GetCallqueue().Remove(Tick);
		m_bTickRunning = false;
	}

	//------------------------------------------------------------------------------------------------
	//! Log only when debug is enabled
	protected void DebugLog(string message)
	{
		if (m_bDebugEnabled)
			Print("[EEF Garrison] " + message);
	}
}
