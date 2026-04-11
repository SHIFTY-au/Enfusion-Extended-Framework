//------------------------------------------------------------------------------------------------
// EEF_PatrolComponent.c
// Enfusion Extended Framework - Patrol Zone Module
//
// Attach to a SCR_BaseTriggerEntity inside a world editor layer.
// Set the trigger sphere radius in the editor to define the patrol zone size.
// When the layer activates, OnPostInit fires and defers patrol start until
// the world is fully initialised.
//------------------------------------------------------------------------------------------------

//------------------------------------------------------------------------------------------------
//! Patrol behaviour mode
enum EEF_EPatrolMode
{
	RANDOM_CONTINUOUS,		//! AI continuously generate new random waypoints within the zone
	//CLOCKWISE,			//! TODO: CW/CCW loop needs further design - disabled for now
	//COUNTER_CLOCKWISE	//! TODO: CW/CCW loop needs further design - disabled for now
}

//------------------------------------------------------------------------------------------------
[ComponentEditorProps(category: "EEF/Patrol", description: "EEF Patrol Zone - attach to a SCR_BaseTriggerEntity inside a layer. Layer activation starts the patrol.")]
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
	int m_iCurrentLoopIndex;					//! Current index in CW/CCW loop (reserved)
}

//------------------------------------------------------------------------------------------------
//! EEF Patrol Zone Component
//! Attach to a SCR_BaseTriggerEntity inside a world editor layer.
//! Set the trigger sphere radius to define the patrol area.
//! Layer activation drives spawn - deferred until world is fully initialised.
class EEF_PatrolComponent : ScriptComponent
{
	//--- Patrol behaviour
	[Attribute(EEF_EPatrolMode.RANDOM_CONTINUOUS.ToString(), UIWidgets.ComboBox, "Patrol behaviour mode", "", ParamEnumArray.FromEnum(EEF_EPatrolMode))]
	protected EEF_EPatrolMode m_ePatrolMode;

	//--- Group configuration
	[Attribute("", UIWidgets.Object, "Patrol groups to spawn. Each entry defines one group prefab.")]
	protected ref array<ref EEF_PatrolGroupSlot> m_aGroupSlots;

	//--- Waypoint prefab
	[Attribute("", UIWidgets.ResourcePickerThumbnail, "Waypoint prefab used for all patrol groups. Select AIWaypoint_Move from Prefabs/AI/Waypoints/.", "et")]
	protected ResourceName m_sWaypointPrefab;

	//--- Movement speed - applied as a waypoint setting on every spawned waypoint
	[Attribute("1", UIWidgets.ComboBox, "Patrol movement speed.", "", ParamEnumArray.FromEnum(EMovementType))]
	protected EMovementType m_eMaxSpeed;

	//--- Formation - applied as a waypoint setting on every spawned waypoint
	[Attribute(SCR_EAIGroupFormation.StaggeredColumn.ToString(), UIWidgets.ComboBox, "Group formation during patrol.", "", ParamEnumArray.FromEnum(SCR_EAIGroupFormation))]
	protected SCR_EAIGroupFormation m_eFormation;

	//--- Position finding
	[Attribute("10", UIWidgets.Slider, "Maximum attempts to find a valid position before giving up.", "1 50 1")]
	protected int m_iMaxPositionAttempts;

	//--- Debug
	[Attribute("0", UIWidgets.CheckBox, "Enable debug logging for this patrol zone.")]
	protected bool m_bDebugEnabled;

	//--- Runtime state
	protected ref array<ref EEF_PatrolGroupState> m_aActiveGroups = {};

	//------------------------------------------------------------------------------------------------
	// INITIALISATION
	//------------------------------------------------------------------------------------------------

	//------------------------------------------------------------------------------------------------
	override void OnPostInit(IEntity owner)
	{
		super.OnPostInit(owner);

		if (!Replication.IsServer())
			return;

		EEF_PatrolDebugLog("EEF_PatrolComponent: Registered - deferring patrol start until world is ready.");

		// Defer until world is fully initialised
		GetGame().GetCallqueue().CallLater(StartPatrol, 1000, false, owner);
	}

	//------------------------------------------------------------------------------------------------
	//! Deferred start - called 1 second after world init to ensure all systems are ready
	protected void StartPatrol(IEntity owner)
	{
		EEF_PatrolDebugLog("EEF_PatrolComponent: Starting patrol zone.");
		EEF_PatrolDebugLog(string.Format("EEF_PatrolComponent: Group slots: %1, Mode: %2", m_aGroupSlots.Count(), m_ePatrolMode));

		SCR_BaseTriggerEntity trigger = SCR_BaseTriggerEntity.Cast(owner);
		if (!trigger)
		{
			EEF_PatrolDebugLog("EEF_PatrolComponent: Owner is not a SCR_BaseTriggerEntity. Attach this component to a trigger entity.");
			return;
		}

		EEF_PatrolDebugLog(string.Format("EEF_PatrolComponent: Sphere radius: %1", trigger.GetSphereRadius()));

		if (trigger.GetSphereRadius() <= 0)
		{
			EEF_PatrolDebugLog("EEF_PatrolComponent: Sphere radius is 0. Set a radius on the trigger entity in the editor.");
			return;
		}

		// Spawn each configured group
		foreach (EEF_PatrolGroupSlot slot : m_aGroupSlots)
		{
			if (slot.m_sGroupPrefab == ResourceName.Empty)
			{
				EEF_PatrolDebugLog("EEF_PatrolComponent: Group slot has no prefab set, skipping.");
				continue;
			}

			SpawnPatrolGroup(owner, slot);
		}
	}

	//------------------------------------------------------------------------------------------------
	// POSITION GENERATION
	//------------------------------------------------------------------------------------------------

	//------------------------------------------------------------------------------------------------
	//! Find a random surface position within the trigger sphere
	protected bool GetRandomPositionInArea(IEntity areaEntity, out vector outPosition)
	{
		SCR_BaseTriggerEntity trigger = SCR_BaseTriggerEntity.Cast(areaEntity);
		if (!trigger)
			return false;

		float radius = trigger.GetSphereRadius();
		vector center = areaEntity.GetOrigin();
		BaseWorld world = GetOwner().GetWorld();

		for (int attempt = 0; attempt < m_iMaxPositionAttempts; attempt++)
	    {
	        float angle = Math.RandomFloat(0, Math.PI2);
	        float dist = Math.RandomFloat(0, radius);
	
	        float worldX = center[0] + dist * Math.Cos(angle);
	        float worldZ = center[2] + dist * Math.Sin(angle);
	
	        float surfaceY = world.GetSurfaceY(worldX, worldZ);
	
	        // Skip positions off the map
	        if (surfaceY < -100)
	            continue;
	
	        vector candidate = Vector(worldX, surfaceY, worldZ);
	
	        // Skip positions in or over water
	        vector waterSurfacePoint;
	        EWaterSurfaceType waterSurfaceType;
	        vector waterTransform[4];
	        vector waterObbExtents;
	        if (ChimeraWorldUtils.TryGetWaterSurface(GetGame().GetWorld(), candidate, waterSurfacePoint, waterSurfaceType, waterTransform, waterObbExtents))
	            continue;
	
	        outPosition = candidate;
	        return true;
	    }

		EEF_PatrolDebugLog(string.Format("EEF_PatrolComponent: Failed to find valid position after %1 attempts.", m_iMaxPositionAttempts));
		return false;
	}

	//------------------------------------------------------------------------------------------------
	// SPAWNING
	//------------------------------------------------------------------------------------------------

	//------------------------------------------------------------------------------------------------
	protected void SpawnPatrolGroup(IEntity areaEntity, EEF_PatrolGroupSlot slot)
	{
		vector spawnPos;
		if (!GetRandomPositionInArea(areaEntity, spawnPos))
		{
			EEF_PatrolDebugLog("EEF_PatrolComponent: Could not find valid spawn position for group.");
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

		EEF_PatrolGroupState state = new EEF_PatrolGroupState();
		state.m_Group = group;
		state.m_iCurrentLoopIndex = 0;
		m_aActiveGroups.Insert(state);

		group.GetOnWaypointCompleted().Insert(OnWaypointCompleted);
		group.GetOnCurrentWaypointChanged().Insert(OnCurrentWaypointChanged);

		InitRandomContinuousPatrol(areaEntity, state);
	}

	//------------------------------------------------------------------------------------------------
	// WAYPOINT LOGIC - RANDOM CONTINUOUS
	//------------------------------------------------------------------------------------------------

	//------------------------------------------------------------------------------------------------
	//! Seed the rolling 2-waypoint queue for a group
	protected void InitRandomContinuousPatrol(IEntity areaEntity, EEF_PatrolGroupState state)
	{
		QueueNextRandomWaypoint(areaEntity, state);
		QueueNextRandomWaypoint(areaEntity, state);
	}

	//------------------------------------------------------------------------------------------------
	//! Generate and queue one random waypoint for the group
	protected void QueueNextRandomWaypoint(IEntity areaEntity, EEF_PatrolGroupState state)
	{
		if (!state.m_Group)
			return;

		vector waypointPos;
		if (!GetRandomPositionInArea(areaEntity, waypointPos))
		{
			EEF_PatrolDebugLog("EEF_PatrolComponent: Could not generate random waypoint position.");
			return;
		}

		AIWaypoint waypoint = SpawnWaypoint(waypointPos);
		if (!waypoint)
		{
			EEF_PatrolDebugLog("EEF_PatrolComponent: SpawnWaypoint returned null - waypoint not queued.");
			return;
		}

		state.m_Group.AddWaypoint(waypoint);
		state.m_aWaypoints.Insert(waypoint);

		EEF_PatrolDebugLog(string.Format("EEF_PatrolComponent: Queued random waypoint at %1.", waypointPos.ToString()));
	}

	//------------------------------------------------------------------------------------------------
	// WAYPOINT COMPLETION
	//------------------------------------------------------------------------------------------------

	//------------------------------------------------------------------------------------------------
	//! Called when the group's current waypoint is completed - queues the next one
	//! NOTE: Do NOT delete wp here - the group manages completed waypoint cleanup internally
	protected void OnWaypointCompleted(AIWaypoint wp)
	{
		foreach (EEF_PatrolGroupState state : m_aActiveGroups)
		{
			if (!state.m_aWaypoints.Contains(wp))
				continue;

			// Remove from our tracking only - do not delete the entity
			state.m_aWaypoints.RemoveItem(wp);

			// Queue next
			QueueNextRandomWaypoint(GetOwner(), state);

			return;
		}
	}

	//------------------------------------------------------------------------------------------------
	//! Debug - confirms whether OnCurrentWaypointChanged fires and settings are present on the waypoint
	protected void OnCurrentWaypointChanged(AIWaypoint currentWP, AIWaypoint prevWP)
	{
		EEF_PatrolDebugLog(string.Format("EEF_PatrolComponent: OnCurrentWaypointChanged fired. currentWP null: %1", currentWP == null));

		if (!currentWP)
			return;

		SCR_AIWaypoint scrWP = SCR_AIWaypoint.Cast(currentWP);
		if (scrWP)
		{
			array<SCR_AISettingBase> settings = {};
			scrWP.GetSettings(settings);
			EEF_PatrolDebugLog(string.Format("EEF_PatrolComponent: Current waypoint has %1 settings.", settings.Count()));
		}
		else
			EEF_PatrolDebugLog("EEF_PatrolComponent: Current waypoint is not SCR_AIWaypoint.");
	}

	//------------------------------------------------------------------------------------------------
	// HELPERS
	//------------------------------------------------------------------------------------------------

	//------------------------------------------------------------------------------------------------
	//! Spawn a waypoint entity and apply speed and formation settings before handing to the group.
	//! Settings MUST be added before AddWaypoint() is called - API requirement.
	protected AIWaypoint SpawnWaypoint(vector position)
	{
		if (m_sWaypointPrefab == ResourceName.Empty)
		{
			EEF_PatrolDebugLog("EEF_PatrolComponent: No waypoint prefab set. Select one from Prefabs/AI/Waypoints/ in the component attributes.");
			return null;
		}

		AIWaypoint waypoint = AIWaypoint.Cast(
			GetGame().SpawnEntityPrefab(
				Resource.Load(m_sWaypointPrefab),
				GetGame().GetWorld()
			)
		);

		if (!waypoint)
		{
			EEF_PatrolDebugLog("EEF_PatrolComponent: Failed to spawn AIWaypoint entity.");
			return null;
		}

		// Apply speed and formation as waypoint settings
		// Must be added before waypoint is given to the group
		SCR_AIWaypoint scrWaypoint = SCR_AIWaypoint.Cast(waypoint);
		if (scrWaypoint)
		{
			// Speed - locks AI to exact movement type while this waypoint is active
			SCR_AIGroupCharactersMovementSpeedSetting speedSetting = SCR_AIGroupCharactersMovementSpeedSetting.Create(
			    SCR_EAISettingOrigin.WAYPOINT,
			    m_eMaxSpeed
			);
			if (speedSetting)
			    scrWaypoint.AddSetting(speedSetting);

			// Formation - sets group formation while this waypoint is active
			SCR_AIGroupFormationSetting formationSetting = SCR_AIGroupFormationSetting.Create(
				SCR_EAISettingOrigin.WAYPOINT,
				m_eFormation
			);
			if (formationSetting)
				scrWaypoint.AddSetting(formationSetting);
			
			array<SCR_AISettingBase> checkSettings = {};
			scrWaypoint.GetSettings(checkSettings);
			EEF_PatrolDebugLog(string.Format("EEF_PatrolComponent: Waypoint has %1 settings after AddSetting calls.", checkSettings.Count()));
			foreach (SCR_AISettingBase s : checkSettings)
			{
			    EEF_PatrolDebugLog(string.Format("EEF_PatrolComponent:  - Setting: %1, Origin: %2, Priority: %3, HasWaypointFlag: %4",
			        s.ClassName(),
			        s.GetOrigin(),
			        s.GetPriority(),
			        s.HasFlag(SCR_EAISettingFlags.WAYPOINT)
			    ));
			}
		}
		else
			EEF_PatrolDebugLog("EEF_PatrolComponent: Waypoint is not SCR_AIWaypoint - speed and formation settings not applied.");

		waypoint.SetOrigin(position);
		return waypoint;
	}

	//------------------------------------------------------------------------------------------------
	//! Log only when debug is enabled
	protected void EEF_PatrolDebugLog(string message)
	{
		if (m_bDebugEnabled)
			Print(message, LogLevel.WARNING);
	}
}