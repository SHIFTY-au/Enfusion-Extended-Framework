//------------------------------------------------------------------------------------------------
// EEF_PatrolComponent.c
// Enfusion Extended Framework - Patrol Zone Module
// Attaches to a trigger entity in the World Editor.
// On trigger activation, spawns AI groups that patrol within the trigger bounds.
//------------------------------------------------------------------------------------------------

//------------------------------------------------------------------------------------------------
//! Patrol behaviour mode
enum EEF_EPatrolMode
{
	RANDOM_CONTINUOUS,	//! AI continuously generate new random waypoints within the zone
	CLOCKWISE,			//! AI cycle through 4 generated points in clockwise order
	COUNTER_CLOCKWISE	//! AI cycle through 4 generated points in counter-clockwise order
}

//------------------------------------------------------------------------------------------------
[ComponentEditorProps(category: "EEF/Patrol", description: "EEF Patrol Zone - attach to a trigger entity to define an AI patrol area.")]
class EEF_PatrolComponentClass : ScriptComponentClass {}

//------------------------------------------------------------------------------------------------
//! Defines a single patrol group slot - mission maker selects prefab
[BaseContainerProps()]
class EEF_PatrolGroupSlot
{
	[Attribute("", UIWidgets.ResourcePickerThumbnail, "Group prefab to spawn", "et")]
	ResourceName m_sGroupPrefab;
}

//------------------------------------------------------------------------------------------------
//! Tracks runtime state for a single spawned patrol group
class EEF_PatrolGroupState
{
	SCR_AIGroup m_Group;						//! The spawned group
	ref array<AIWaypoint> m_aWaypoints = {};	//! Currently queued waypoints
	int m_iCurrentLoopIndex;					//! Current index in CW/CCW loop
}

//------------------------------------------------------------------------------------------------
//! EEF Patrol Zone Component
//! Place on a resizable, rotatable trigger entity in the World Editor.
//! Trigger activation drives spawn - handle activation via layer/trigger hierarchy in editor.
class EEF_PatrolComponent : ScriptComponent
{
	//--- Patrol behaviour
	[Attribute(EEF_EPatrolMode.RANDOM_CONTINUOUS.ToString(), UIWidgets.ComboBox, "Patrol behaviour mode", "", ParamEnumArray.FromEnum(EEF_EPatrolMode))]
	protected EEF_EPatrolMode m_ePatrolMode;

	//--- Group configuration
	[Attribute("", UIWidgets.Object, "Patrol groups to spawn. Each entry defines one group prefab.")]
	protected ref array<ref EEF_PatrolGroupSlot> m_aGroupSlots;

	//--- CW/CCW loop point constraints
	[Attribute("10.0", UIWidgets.Slider, "Minimum distance between generated loop points (CW/CCW modes)", "1 200 1")]
	protected float m_fLoopPointMinDistance;

	[Attribute("50.0", UIWidgets.Slider, "Maximum distance between generated loop points (CW/CCW modes)", "1 500 1")]
	protected float m_fLoopPointMaxDistance;

	//--- Navmesh retries
	[Attribute("10", UIWidgets.Slider, "Maximum attempts to find a valid navmesh position before giving up", "1 50 1")]
	protected int m_iNavmeshRetryLimit;

	//--- Debug
	[Attribute("0", UIWidgets.CheckBox, "Enable debug logging for this patrol zone")]
	protected bool m_bDebugEnabled;

	//--- Runtime state
	protected ref array<ref EEF_PatrolGroupState> m_aActiveGroups = {};
	protected ref array<vector> m_aLoopPoints = {};	//! Locked CW/CCW points, generated once on activation
	protected bool m_bActivated = false;

	//------------------------------------------------------------------------------------------------
	// INITIALISATION
	//------------------------------------------------------------------------------------------------

	//------------------------------------------------------------------------------------------------
	override void OnPostInit(IEntity owner)
	{
		super.OnPostInit(owner);

		if (!Replication.IsServer())
			return;

		SCR_BaseTriggerEntity trigger = SCR_BaseTriggerEntity.Cast(owner);
		if (!trigger)
		{
			EEF_PatrolDebugLog("EEF_PatrolComponent: Owner is not a trigger entity. Component will not function.");
			return;
		}

		trigger.GetOnActivate().Insert(OnTriggerActivated);
	}

	//------------------------------------------------------------------------------------------------
	// TRIGGER ACTIVATION
	//------------------------------------------------------------------------------------------------

	//------------------------------------------------------------------------------------------------
	protected void OnTriggerActivated(IEntity triggerEntity)
	{
		if (m_bActivated)
			return;

		m_bActivated = true;
		EEF_PatrolDebugLog("EEF_PatrolComponent: Trigger activated - beginning patrol spawn.");

		// Pre-generate loop points once if in CW/CCW mode
		if (m_ePatrolMode == EEF_EPatrolMode.CLOCKWISE || m_ePatrolMode == EEF_EPatrolMode.COUNTER_CLOCKWISE)
			GenerateLoopPoints(triggerEntity);

		// Spawn each configured group
		foreach (EEF_PatrolGroupSlot slot : m_aGroupSlots)
		{
			if (slot.m_sGroupPrefab == ResourceName.Empty)
			{
				EEF_PatrolDebugLog("EEF_PatrolComponent: Group slot has no prefab set, skipping.");
				continue;
			}

			SpawnPatrolGroup(triggerEntity, slot);
		}
	}

	//------------------------------------------------------------------------------------------------
	// POSITION GENERATION
	//------------------------------------------------------------------------------------------------

	//------------------------------------------------------------------------------------------------
	//! Attempt to find a random navmesh-valid position within the rotated trigger bounds
	protected bool GetRandomPositionInTrigger(IEntity triggerEntity, out vector outPosition)
	{
		vector mins, maxs;
		triggerEntity.GetWorldBounds(mins, maxs);

		vector halfExtents = (maxs - mins) * 0.5;
		vector center = triggerEntity.GetOrigin();
		vector transform[4];
		triggerEntity.GetWorldTransform(transform);

		NavmeshWorldComponent navmesh = GetNavmeshWorldComponent();
		if (!navmesh)
		{
			EEF_PatrolDebugLog("EEF_PatrolComponent: Could not retrieve NavmeshWorldComponent.");
			return false;
		}

		for (int attempt = 0; attempt < m_iNavmeshRetryLimit; attempt++)
		{
			// Random offset in local space
			float randX = Math.RandomFloat(-halfExtents[0], halfExtents[0]);
			float randZ = Math.RandomFloat(-halfExtents[2], halfExtents[2]);
			vector localOffset = Vector(randX, 0, randZ);

			// Transform into world space respecting trigger rotation
			vector worldPos = center + localOffset.Multiply3(transform);
			worldPos[1] = center[1];

			// Snap to nearest navmesh position within 5m
			vector snappedPos;
			if (navmesh.SamplePosition(worldPos, 5.0, snappedPos))
			{
				outPosition = snappedPos;
				return true;
			}
		}

		EEF_PatrolDebugLog(string.Format("EEF_PatrolComponent: Failed to find navmesh position after %1 attempts.", m_iNavmeshRetryLimit));
		return false;
	}

	//------------------------------------------------------------------------------------------------
	//! Generate 4 loop points for CW/CCW mode, sorted by angle around the trigger center
	protected void GenerateLoopPoints(IEntity triggerEntity)
	{
		m_aLoopPoints.Clear();
		vector center = triggerEntity.GetOrigin();
		int retryBudget = m_iNavmeshRetryLimit * 4;
		int attempts = 0;

		while (m_aLoopPoints.Count() < 4 && attempts < retryBudget)
		{
			attempts++;

			vector candidate;
			if (!GetRandomPositionInTrigger(triggerEntity, candidate))
				continue;

			// Check min/max distance against already accepted points
			bool valid = true;
			foreach (vector existing : m_aLoopPoints)
			{
				float dist = vector.Distance(candidate, existing);
				if (dist < m_fLoopPointMinDistance || dist > m_fLoopPointMaxDistance)
				{
					valid = false;
					break;
				}
			}

			if (valid)
				m_aLoopPoints.Insert(candidate);
		}

		// Distance constraints could not be satisfied within budget
		// Box is the hard limit - fill remaining slots without distance constraint
		if (m_aLoopPoints.Count() < 4)
		{
			EEF_PatrolDebugLog(string.Format(
				"EEF_PatrolComponent: Could only generate %1/4 loop points satisfying distance constraints. Filling remaining slots without distance constraint.",
				m_aLoopPoints.Count()
			));

			while (m_aLoopPoints.Count() < 4)
			{
				vector fallback;
				if (GetRandomPositionInTrigger(triggerEntity, fallback))
					m_aLoopPoints.Insert(fallback);
				else
					break;
			}
		}

		if (m_aLoopPoints.Count() < 2)
		{
			EEF_PatrolDebugLog("EEF_PatrolComponent: Insufficient loop points generated. CW/CCW patrol will not function.");
			return;
		}

		// Sort points by angle around center for clean CW/CCW ordering
		SortPointsByAngle(center);

		// Reverse order for counter-clockwise
		if (m_ePatrolMode == EEF_EPatrolMode.COUNTER_CLOCKWISE)
			// Manual Reverse for m_aLoopPoints
			int count = m_aLoopPoints.Count();
			for (int i = 0; i < count / 2; i++)
			{
			    vector temp = m_aLoopPoints[i];
			    m_aLoopPoints[i] = m_aLoopPoints[count - i - 1];
			    m_aLoopPoints[count - i - 1] = temp;
			}

		EEF_PatrolDebugLog(string.Format("EEF_PatrolComponent: Generated %1 loop points.", m_aLoopPoints.Count()));
	}

	//------------------------------------------------------------------------------------------------
	//! Sort m_aLoopPoints by angle around a center point (XZ plane) for geometric CW ordering
	protected void SortPointsByAngle(vector center)
	{
		int count = m_aLoopPoints.Count();
		for (int i = 1; i < count; i++)
		{
			vector key = m_aLoopPoints[i];
			float keyAngle = Math.Atan2(key[2] - center[2], key[0] - center[0]);
			int j = i - 1;

			while (j >= 0)
			{
				vector comp = m_aLoopPoints[j];
				float compAngle = Math.Atan2(comp[2] - center[2], comp[0] - center[0]);
				if (compAngle <= keyAngle)
					break;

				m_aLoopPoints[j + 1] = m_aLoopPoints[j];
				j--;
			}

			m_aLoopPoints[j + 1] = key;
		}
	}

	//------------------------------------------------------------------------------------------------
	// SPAWNING
	//------------------------------------------------------------------------------------------------

	//------------------------------------------------------------------------------------------------
	protected void SpawnPatrolGroup(IEntity triggerEntity, EEF_PatrolGroupSlot slot)
	{
		vector spawnPos;
		if (!GetRandomPositionInTrigger(triggerEntity, spawnPos))
		{
			EEF_PatrolDebugLog("EEF_PatrolComponent: Could not find valid spawn position for group. Group will not spawn.");
			return;
		}

		Resource groupResource = Resource.Load(slot.m_sGroupPrefab);
		if (!groupResource.IsValid())
		{
			EEF_PatrolDebugLog("EEF_PatrolComponent: Group prefab resource is invalid.");
			return;
		}

		EntitySpawnParams spawnParams = new EntitySpawnParams();
		spawnParams.TransformMode = ETransformMode.WORLD;
		Math3D.MatrixIdentity4(spawnParams.Transform);
		spawnParams.Transform[3] = spawnPos;

		SCR_AIGroup group = SCR_AIGroup.Cast(GetGame().SpawnEntityPrefab(groupResource, GetOwner().GetWorld(), spawnParams));
		if (!group)
		{
			EEF_PatrolDebugLog("EEF_PatrolComponent: Failed to spawn group entity.");
			return;
		}

		EEF_PatrolDebugLog(string.Format("EEF_PatrolComponent: Spawned group at %1.", spawnPos.ToString()));

		// Register state and subscribe to waypoint completion
		EEF_PatrolGroupState state = new EEF_PatrolGroupState();
		state.m_Group = group;
		state.m_iCurrentLoopIndex = 0;
		m_aActiveGroups.Insert(state);

		group.GetOnWaypointCompleted().Insert(OnWaypointCompleted);

		// Issue initial waypoints
		if (m_ePatrolMode == EEF_EPatrolMode.RANDOM_CONTINUOUS)
			InitRandomContinuousPatrol(triggerEntity, state);
		else
			InitLoopPatrol(state);
	}

	//------------------------------------------------------------------------------------------------
	// WAYPOINT LOGIC - RANDOM CONTINUOUS
	//------------------------------------------------------------------------------------------------

	//------------------------------------------------------------------------------------------------
	//! Seed the rolling 2-waypoint queue for a group
	protected void InitRandomContinuousPatrol(IEntity triggerEntity, EEF_PatrolGroupState state)
	{
		QueueNextRandomWaypoint(triggerEntity, state);
		QueueNextRandomWaypoint(triggerEntity, state);
	}

	//------------------------------------------------------------------------------------------------
	//! Generate and queue one random waypoint for the group
	protected void QueueNextRandomWaypoint(IEntity triggerEntity, EEF_PatrolGroupState state)
	{
		if (!state.m_Group)
			return;

		vector waypointPos;
		if (!GetRandomPositionInTrigger(triggerEntity, waypointPos))
		{
			EEF_PatrolDebugLog("EEF_PatrolComponent: Could not generate random waypoint position.");
			return;
		}

		AIWaypoint waypoint = SpawnWaypoint(waypointPos);
		if (!waypoint)
			return;

		state.m_Group.AddWaypoint(waypoint);
		state.m_aWaypoints.Insert(waypoint);

		EEF_PatrolDebugLog(string.Format("EEF_PatrolComponent: Queued random waypoint at %1.", waypointPos.ToString()));
	}

	//------------------------------------------------------------------------------------------------
	// WAYPOINT LOGIC - CW/CCW LOOP
	//------------------------------------------------------------------------------------------------

	//------------------------------------------------------------------------------------------------
	//! Seed the loop with the first two waypoints
	protected void InitLoopPatrol(EEF_PatrolGroupState state)
	{
		if (m_aLoopPoints.IsEmpty())
		{
			EEF_PatrolDebugLog("EEF_PatrolComponent: No loop points available, cannot init loop patrol.");
			return;
		}

		IssueNextLoopWaypoint(state);
		IssueNextLoopWaypoint(state);
	}

	//------------------------------------------------------------------------------------------------
	//! Issue the next waypoint in the loop sequence
	protected void IssueNextLoopWaypoint(EEF_PatrolGroupState state)
	{
		if (!state.m_Group || m_aLoopPoints.IsEmpty())
			return;

		int index = state.m_iCurrentLoopIndex % m_aLoopPoints.Count();
		vector waypointPos = m_aLoopPoints[index];

		AIWaypoint waypoint = SpawnWaypoint(waypointPos);
		if (!waypoint)
			return;

		state.m_Group.AddWaypoint(waypoint);
		state.m_aWaypoints.Insert(waypoint);
		state.m_iCurrentLoopIndex++;

		EEF_PatrolDebugLog(string.Format("EEF_PatrolComponent: Queued loop waypoint index %1 at %2.", index, waypointPos.ToString()));
	}

	//------------------------------------------------------------------------------------------------
	// WAYPOINT COMPLETION
	//------------------------------------------------------------------------------------------------

	//------------------------------------------------------------------------------------------------
	//! Called when any group completes a waypoint - rolls the queue forward
	protected void OnWaypointCompleted(AIGroup group, AIWaypoint waypoint)
	{
		foreach (EEF_PatrolGroupState state : m_aActiveGroups)
		{
			if (state.m_Group != group)
				continue;

			// Clean up completed waypoint
			state.m_aWaypoints.RemoveItem(waypoint);
			SCR_EntityHelper.DeleteEntityAndChildren(waypoint);

			// Queue next
			if (m_ePatrolMode == EEF_EPatrolMode.RANDOM_CONTINUOUS)
				QueueNextRandomWaypoint(SCR_BaseTriggerEntity.Cast(GetOwner()), state);
			else
				IssueNextLoopWaypoint(state);

			return;
		}
	}

	//------------------------------------------------------------------------------------------------
	// HELPERS
	//------------------------------------------------------------------------------------------------

	//------------------------------------------------------------------------------------------------
	//! Spawn a move waypoint entity at the given world position
	protected AIWaypoint SpawnWaypoint(vector position)
	{
		// NOTE: Verify this resource GUID against your project's AIWaypoint_Move prefab
		Resource waypointResource = Resource.Load("{8F0A7F3D4B7D3A1C}Prefabs/AI/Waypoints/AIWaypoint_Move.et");
		if (!waypointResource.IsValid())
		{
			EEF_PatrolDebugLog("EEF_PatrolComponent: Waypoint prefab could not be loaded. Verify the resource GUID.");
			return null;
		}

		EntitySpawnParams spawnParams = new EntitySpawnParams();
		spawnParams.TransformMode = ETransformMode.WORLD;
		Math3D.MatrixIdentity4(spawnParams.Transform);
		spawnParams.Transform[3] = position;

		return AIWaypoint.Cast(GetGame().SpawnEntityPrefab(waypointResource, GetOwner().GetWorld(), spawnParams));
	}

	//------------------------------------------------------------------------------------------------
	//! Retrieve the soldier NavmeshWorldComponent from AIWorld
	protected NavmeshWorldComponent GetNavmeshWorldComponent()
	{
		AIWorld aiWorld = GetGame().GetWorld().GetAIWorld();
		if (!aiWorld)
			return null;

		return aiWorld.GetNavmeshWorldComponent("Soldiers");
	}

	//------------------------------------------------------------------------------------------------
	//! Log only when debug is enabled
	protected void EEF_PatrolDebugLog(string message)
	{
		if (m_bDebugEnabled)
			Print(message, LogLevel.WARNING);
	}
}