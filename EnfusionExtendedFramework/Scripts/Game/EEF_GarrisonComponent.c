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
// guessed at blind. Several remain unconfirmed on a live Workbench session:
//
//   - GROUP_CONTAINER_PREFAB is a hardcoded resource, not a mission-maker
//     field - deliberately so; every AI-spawning module in this codebase
//     spawns its groups from a prefab resource (no "create a group from
//     nothing" API was found), so a real resource is unavoidable, but which
//     one doesn't matter functionally since it gets stripped at runtime
//     regardless of content (see below). Currently a bare GUID reference
//     with no path (`{EACD97CF4A702FAE}`) - if Resource.Load() can't
//     resolve a path-less GUID, swap in the fuller
//     "{GUID}Prefabs/Path/File.et" string instead.
//   - FinishGarrisonSpawn() adds the garrison character via AddAgent()
//     FIRST, then StripGroupMembers() removes whatever pre-authored
//     members the container originally had via RemoveAgent(). Deliberately
//     in that order, not the reverse: if some AIGroup implementation
//     auto-cleans up on hitting zero members, stripping first would delete
//     the group entity out from under us before we got to add our own
//     character. AddAgent()'s existence is fairly well supported (the base
//     game's own agent-removed event passes (AIGroup, AIAgent) params,
//     implying a symmetric Add/Remove pair), but neither call - nor the
//     "does empty auto-cleanup" assumption above - has been exercised
//     against a live Workbench session yet.
//   - SpawnGarrisonAI() waits out the container's delayed member spawn via
//     IsInitializing()/GetOnAllDelayedEntitySpawned() before stripping -
//     the same pattern already proven working in
//     EEF_HelicopterInsertionComponent's cargo-boarding wait, just applied
//     to an arbitrary hardcoded group prefab here instead of one
//     purpose-built for cargo.
//
// ResolveAIAgent() previously guessed FindComponent(AIAgent) directly,
// which returned null at runtime against a real character prefab
// (Character_US_GL_Guard.et). Fixed to go through AIControlComponent.
// GetControlAIAgent() instead, matching base-game SCR_SpawnRequestComponent
// usage - confirmed working pattern, not a guess.
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
//! Tracks a group container mid-spawn, waiting on its pre-authored members (if any) to finish
//! spawning before we strip them and add our own garrison character.
class EEF_GarrisonPendingSpawn
{
	SCR_AIGroup m_Group;
	ResourceName m_CharacterPrefab;
	AIWaypoint m_Marker;
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
	protected ref array<ref EEF_GarrisonPendingSpawn> m_aPendingSpawns = {};

	//--- Tuning constants (not mission-maker facing - internal behaviour timing)
	protected const float GARRISON_TICK_INTERVAL_S = 1.0;		//! Poll rate for engagement/move/investigate logic.
	protected const float ENGAGED_COOLDOWN_SECONDS = 8.0;		//! Time since last damage before an AI is no longer "personally engaged".
	protected const float MIN_INVESTIGATE_DWELL_SECONDS = 20.0;	//! Minimum time a pure bystander stays at the investigate point before giving up.
	protected const float HEALTH_DROP_EPSILON = 0.01;			//! Minimum GetHealthScaled() delta to count as "took damage".

	//! Group prefab used as the container for each spawned AI's standalone "group of one".
	//! Not mission-maker facing - any pre-authored members it has are stripped automatically at
	//! runtime (see FinishGarrisonSpawn/StripGroupMembers), so which real group prefab this points
	//! at doesn't matter functionally. GUID-only reference (no path) - if Resource.Load() can't
	//! resolve it, replace with the fuller "{GUID}Prefabs/Path/File.et" string instead.
	protected const ResourceName GROUP_CONTAINER_PREFAB = "{EACD97CF4A702FAE}";

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

		Resource groupContainerCheck = Resource.Load(GROUP_CONTAINER_PREFAB);
		if (!groupContainerCheck || !groupContainerCheck.IsValid())
		{
			Print(string.Format("[EEF Garrison] ERROR: Internal group container prefab could not be loaded: %1. This is a hardcoded resource, not mission-maker configurable - see GROUP_CONTAINER_PREFAB in EEF_GarrisonComponent.c.", GROUP_CONTAINER_PREFAB), LogLevel.ERROR);
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
		m_aPendingSpawns.Clear();

		for (int i = 0; i < spawnCount; i++)
			SpawnGarrisonAI(shuffled[i]);

		m_bActive = true;

		if (!m_bTickRunning)
		{
			GetGame().GetCallqueue().CallLater(Tick, GARRISON_TICK_INTERVAL_S * 1000, true);
			m_bTickRunning = true;
		}

		// Spawning is async (see SpawnGarrisonAI) - some may still be waiting on a group
		// container's pre-authored members to finish spawning before we can strip them.
		// FinishGarrisonSpawn logs each individual arrival; this is just the request count.
		DebugLog(string.Format("Garrison starting - %1 AI requested across %2 marker(s).", spawnCount, m_aMarkers.Count()));
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

			// Any spawn still waiting on a group container's delayed member spawn - discard the
			// container. The pending list is cleared below so a late OnTemplateGroupReady callback
			// finds no match and is a safe no-op.
			foreach (EEF_GarrisonPendingSpawn pending : m_aPendingSpawns)
			{
				if (pending && pending.m_Group)
					SCR_EntityHelper.DeleteEntityAndChildren(pending.m_Group);
			}
		}

		m_aActiveAI.Clear();
		m_aPendingSpawns.Clear();
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
			AIControlComponent control = AIControlComponent.Cast(testEntity.FindComponent(AIControlComponent));
			bool hasControl = (control != null);
			AIAgent agent = null;
			if (control)
				agent = AIAgent.Cast(control.GetControlAIAgent());
			bool hasAgent = (agent != null);

			SCR_EntityHelper.DeleteEntityAndChildren(testEntity);

			if (isGroup)
			{
				Print(string.Format("[EEF Garrison] ERROR: Character prefab slot %1 resolves to an AI group, not an individual character: %2. Both share the .et extension in the resource picker - double-check the selection.", i, slot.m_sCharacterPrefab), LogLevel.ERROR);
				return false;
			}

			if (!agent)
			{
				Print(string.Format("[EEF Garrison] ERROR: Character prefab slot %1 does not resolve to a controllable character (hasAIControlComponent=%2, agentResolved=%3): %4", i, hasControl, hasAgent, slot.m_sCharacterPrefab), LogLevel.ERROR);
				return false;
			}
		}

		return true;
	}

	//------------------------------------------------------------------------------------------------
	// SPAWNING
	//------------------------------------------------------------------------------------------------

	//------------------------------------------------------------------------------------------------
	//! Spawns the group container for one garrison AI at marker. If the container prefab has
	//! pre-authored members, SCR_AIGroup spawns them across several frames before it's ready
	//! (IsInitializing()/GetOnAllDelayedEntitySpawned() - same pattern as
	//! EEF_HelicopterInsertionComponent's cargo boarding wait) - we wait for that, then strip
	//! them via FinishGarrisonSpawn. An already-empty container finishes immediately.
	protected void SpawnGarrisonAI(AIWaypoint marker)
	{
		ResourceName characterPrefab = PickRandomCharacterPrefab();
		if (characterPrefab.IsEmpty())
			return;

		Resource groupRes = Resource.Load(GROUP_CONTAINER_PREFAB);
		if (!groupRes || !groupRes.IsValid())
		{
			Print(string.Format("[EEF Garrison] ERROR: Could not load internal group container prefab: %1", GROUP_CONTAINER_PREFAB), LogLevel.ERROR);
			return;
		}

		EntitySpawnParams spawnParams = new EntitySpawnParams();
		spawnParams.TransformMode = ETransformMode.WORLD;
		Math3D.MatrixIdentity4(spawnParams.Transform);
		spawnParams.Transform[3] = marker.GetOrigin();

		SCR_AIGroup group = SCR_AIGroup.Cast(GetGame().SpawnEntityPrefab(groupRes, GetGame().GetWorld(), spawnParams));
		if (!group)
		{
			Print("[EEF Garrison] ERROR: Failed to spawn group container for garrison AI.", LogLevel.ERROR);
			return;
		}

		EEF_GarrisonPendingSpawn pending = new EEF_GarrisonPendingSpawn();
		pending.m_Group = group;
		pending.m_CharacterPrefab = characterPrefab;
		pending.m_Marker = marker;
		m_aPendingSpawns.Insert(pending);

		if (group.IsInitializing())
			group.GetOnAllDelayedEntitySpawned().Insert(OnTemplateGroupReady);
		else
			FinishGarrisonSpawn(pending);
	}

	//------------------------------------------------------------------------------------------------
	//! SCR_AIGroup.GetOnAllDelayedEntitySpawned() callback - fires once the container's
	//! pre-authored members (if any) have finished spawning. Matches by group reference against
	//! the pending list (mirrors OnWaypointCompleted's lookup pattern below).
	protected void OnTemplateGroupReady(SCR_AIGroup group)
	{
		foreach (EEF_GarrisonPendingSpawn pending : m_aPendingSpawns)
		{
			if (pending.m_Group != group)
				continue;

			FinishGarrisonSpawn(pending);
			return;
		}

		// No match - StopGarrison already discarded this pending spawn. Safe no-op.
	}

	//------------------------------------------------------------------------------------------------
	//! Spawns the garrison character into the group container FIRST, then strips whatever
	//! pre-authored members it originally had - never the other way around. Some AIGroup
	//! implementations may auto-cleanup when membership hits zero, which would delete `group`
	//! out from under us if we stripped before adding; adding first means it's never transiently
	//! empty.
	protected void FinishGarrisonSpawn(EEF_GarrisonPendingSpawn pending)
	{
		m_aPendingSpawns.RemoveItem(pending);

		SCR_AIGroup group = pending.m_Group;
		if (!group)
			return;

		array<AIAgent> originalAgents = {};
		group.GetAgents(originalAgents);

		IEntity character = SpawnCharacterAtMarker(pending.m_CharacterPrefab, pending.m_Marker);
		if (!character)
		{
			Print("[EEF Garrison] ERROR: Failed to spawn character for garrison AI. Deleting group container.", LogLevel.ERROR);
			SCR_EntityHelper.DeleteEntityAndChildren(group);
			return;
		}

		AIAgent agent = ResolveAIAgent(character);
		if (!agent)
		{
			Print("[EEF Garrison] ERROR: Spawned character has no AIAgent - cannot form group. Deleting.", LogLevel.ERROR);
			SCR_EntityHelper.DeleteEntityAndChildren(character);
			SCR_EntityHelper.DeleteEntityAndChildren(group);
			return;
		}

		group.AddAgent(agent);
		StripGroupMembers(group, originalAgents);
		group.GetOnWaypointCompleted().Insert(OnWaypointCompleted);

		EEF_GarrisonAIState state = new EEF_GarrisonAIState();
		state.m_Group = group;
		state.m_Agent = agent;
		state.m_Character = character;
		state.m_HomeMarker = pending.m_Marker;
		state.m_DamageManager = SCR_CharacterDamageManagerComponent.Cast(character.FindComponent(SCR_CharacterDamageManagerComponent));

		if (state.m_DamageManager)
			state.m_fLastHealthScaled = state.m_DamageManager.GetHealthScaled();
		else
			state.m_fLastHealthScaled = 1.0;

		m_aActiveAI.Insert(state);
		AssignMove(state, pending.m_Marker);

		DebugLog(string.Format("Spawned garrison AI at marker %1.", pending.m_Marker.GetOrigin().ToString()));
	}

	//------------------------------------------------------------------------------------------------
	//! Removes and deletes each agent in agentsToRemove from group - the container's original
	//! members, captured by FinishGarrisonSpawn before our own character was added.
	protected void StripGroupMembers(SCR_AIGroup group, array<AIAgent> agentsToRemove)
	{
		foreach (AIAgent agent : agentsToRemove)
		{
			if (!agent)
				continue;

			IEntity character = agent.GetControlledEntity();
			group.RemoveAgent(agent);

			if (character)
				SCR_EntityHelper.DeleteEntityAndChildren(character);
		}

		if (!agentsToRemove.IsEmpty())
			DebugLog(string.Format("Stripped %1 pre-authored member(s) from group container.", agentsToRemove.Count()));
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
	//! Retrieves the AIAgent for a spawned character via its AIControlComponent.GetControlAIAgent().
	//! See ValidateCharacterPrefabs() for the inline diagnostic breakdown of this same lookup -
	//! kept as raw local booleans there rather than an out-parameter, after an out string here
	//! came back empty on both the "not found" and "found but null" paths in testing (worth
	//! re-examining if this call is revisited - may indicate out string itself doesn't propagate
	//! as expected in this script environment, independent of the AI API question).
	protected AIAgent ResolveAIAgent(IEntity character)
	{
		AIControlComponent control = AIControlComponent.Cast(character.FindComponent(AIControlComponent));
		if (!control)
			return null;

		return AIAgent.Cast(control.GetControlAIAgent());
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
