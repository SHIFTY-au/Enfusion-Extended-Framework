// ============================================================
// EEF_HunterSpawnerComponent.c
// Enfusion Extended Framework
//
// Spawns AI groups that hunt players. Supports hemisphere
// filtering, waypoint refresh, distant group cleanup, and
// multiple player split handling modes.
//
// Depends on: EEF_HunterTrackerComponent on same GameMode entity.
// Runs on SERVER (authority) only.
// ============================================================

// ============================================================
// Hemisphere mode
// BOTH   - spawn anywhere on circle edge
// CHASE  - spawn behind players (AI chases from rear)
// BLOCK  - spawn in front (AI blocks path ahead)
// RANDOM - pick randomly each cycle
// ============================================================
enum EEF_EHemisphereMode
{
	BOTH	= 0,
	CHASE	= 1,
	BLOCK	= 2,
	RANDOM	= 3
}

// ============================================================
// Player split handling mode
// CENTROID - mean of all players, falls back to largest
//            cluster if spread exceeds dispersion threshold
// SPLIT    - divide AI groups between detected clusters
// NEAREST  - each AI group independently tracks nearest player
// ============================================================
enum EEF_EPlayerMode
{
	CENTROID	= 0,
	SPLIT		= 1,
	NEAREST		= 2
}

// Internal tracking data per spawned group
class EEF_HunterGroupData
{
	IEntity m_Group;
	int m_iAssignedClusterIndex;	// Used in SPLIT mode
	float m_fLastFlareTime;			// World time of last periodic flare — staggered on spawn

	void EEF_HunterGroupData(IEntity group, int clusterIndex = 0, float flareTimeOffset = 0)
	{
		m_Group = group;
		m_iAssignedClusterIndex = clusterIndex;
		m_fLastFlareTime = flareTimeOffset;
	}
}

[ComponentEditorProps(category: "EEF/Hunter", description: "Spawns AI groups that hunt players. Requires EEF_HunterTrackerComponent on the same GameMode entity.")]
class EEF_HunterSpawnerComponentClass : SCR_BaseGameModeComponentClass {}

class EEF_HunterSpawnerComponent : SCR_BaseGameModeComponent
{
	// --------------------------------------------------------
	// Spawn settings
	// --------------------------------------------------------

	[Attribute("300.0", UIWidgets.EditBox, "Radius in metres around player centerpoint. AI spawn on the edge of this circle.")]
	protected float m_fHuntRadius;

	[Attribute("45.0", UIWidgets.EditBox, "How often in seconds to attempt a new spawn.")]
	protected float m_fSpawnInterval;

	[Attribute("4", UIWidgets.EditBox, "Maximum number of active AI groups at any one time.")]
	protected int m_iMaxActiveGroups;

	[Attribute("", UIWidgets.ResourcePickerThumbnail, "Prefab of the AI group to spawn.", "et")]
	protected ResourceName m_sGroupPrefab;

	[Attribute("6", UIWidgets.EditBox, "Maximum attempts to find a valid spawn point before skipping cycle.")]
	protected int m_iMaxSpawnAttempts;

	// --------------------------------------------------------
	// Hemisphere settings
	// --------------------------------------------------------

	[Attribute("0", UIWidgets.ComboBox, "Which hemisphere to spawn AI in relative to player movement.", "", ParamEnumArray.FromEnum(EEF_EHemisphereMode))]
	protected EEF_EHemisphereMode m_eHemisphereMode;

	[Attribute("60.0", UIWidgets.EditBox, "Half-angle in degrees of hemisphere cone. 90 = full half, 45 = tighter cone.")]
	protected float m_fHemisphereAngle;

	// --------------------------------------------------------
	// Waypoint settings
	// --------------------------------------------------------

	[Attribute("{E37A00B4AFED7B31}Prefabs/AI/Waypoints/AIWaypoint_Move.et", UIWidgets.ResourcePickerThumbnail, "Prefab for AI move waypoints.", "et")]
	protected ResourceName m_sWaypointPrefab;
	
	[Attribute("80.0", UIWidgets.EditBox, "Waypoints placed within this many metres of the target centerpoint.")]
	protected float m_fWaypointScatter;

	[Attribute("1", UIWidgets.CheckBox, "Automatically refresh AI waypoints on an interval to keep them chasing.")]
	protected bool m_bAutoRefreshWaypoints;

	[Attribute("15.0", UIWidgets.EditBox, "How often in seconds to refresh AI waypoints. Only used if Auto Refresh is enabled.")]
	protected float m_fWaypointRefreshInterval;

	// --------------------------------------------------------
	// Player split handling
	// --------------------------------------------------------

	[Attribute("0", UIWidgets.ComboBox, "How AI groups handle players that are spread across the map.", "", ParamEnumArray.FromEnum(EEF_EPlayerMode))]
	protected EEF_EPlayerMode m_ePlayerMode;

	[Attribute("300.0", UIWidgets.EditBox, "CENTROID mode: if player spread exceeds this distance, switch to largest cluster targeting.")]
	protected float m_fDispersionThreshold;

	// --------------------------------------------------------
	// Cleanup settings
	// --------------------------------------------------------

	[Attribute("1", UIWidgets.CheckBox, "Remove AI groups that are too far from all players.")]
	protected bool m_bCleanupDistant;

	[Attribute("600.0", UIWidgets.EditBox, "Distance in metres beyond which an AI group is cleaned up. Only used if Cleanup Distant is enabled.")]
	protected float m_fCleanupDistance;

	// --------------------------------------------------------
	// Flare settings
	// --------------------------------------------------------

	[Attribute("1", UIWidgets.CheckBox, "Spawn an illumination flare at the AI spawn point.")]
	protected bool m_bFlareOnSpawn;

	[Attribute("", UIWidgets.ResourcePickerThumbnail, "Prefab to use as spawn flare/illumination. Leave empty to skip.", "et")]
	protected ResourceName m_sFlarePrefab;

	[Attribute("0", UIWidgets.CheckBox, "Periodically spawn a flare above each active AI group while hunting. Groups are staggered so they do not fire simultaneously.")]
	protected bool m_bPeriodicGroupFlare;

	[Attribute("60.0", UIWidgets.EditBox, "How often in seconds to spawn a flare above each active group. Only used if Periodic Group Flare is enabled.")]
	protected float m_fGroupFlareInterval;

	[Attribute("20.0", UIWidgets.EditBox, "Height in metres above the group to spawn the periodic flare.")]
	protected float m_fGroupFlareHeight;

	// --------------------------------------------------------
	// Debug
	// --------------------------------------------------------

	[Attribute("1", UIWidgets.CheckBox, "Print debug info to console. Disable in final missions.")]
	protected bool m_bDebugLog;

	// --------------------------------------------------------
	// Internal state
	// --------------------------------------------------------
	protected EEF_HunterTrackerComponent m_Tracker;
	protected ref array<ref EEF_HunterGroupData> m_aActiveGroups = new array<ref EEF_HunterGroupData>();
	protected bool m_bActive = false;

	// --------------------------------------------------------
	// Init
	// --------------------------------------------------------
	override void OnPostInit(IEntity owner)
	{
		super.OnPostInit(owner);

		if (!Replication.IsServer())
			return;

		m_Tracker = EEF_HunterTrackerComponent.Cast(
			owner.FindComponent(EEF_HunterTrackerComponent)
		);

		if (!m_Tracker)
		{
			Print("[EEF Spawner] ERROR: EEF_HunterTrackerComponent not found on GameMode entity. Hunter system will not function.", LogLevel.ERROR);
			return;
		}

		if (m_sGroupPrefab.IsEmpty())
			Print("[EEF Spawner] WARNING: No group prefab set. Nothing will spawn.", LogLevel.WARNING);

		// Spawn ticker
		GetGame().GetCallqueue().CallLater(SpawnTick, m_fSpawnInterval * 1000, true);

		// Waypoint refresh + cleanup ticker
		if (m_bAutoRefreshWaypoints || m_bCleanupDistant)
			GetGame().GetCallqueue().CallLater(WaypointTick, m_fWaypointRefreshInterval * 1000, true);

		DebugLog("Initialised successfully.");
	}

	// --------------------------------------------------------
	// Spawn tick
	// --------------------------------------------------------
	protected void SpawnTick()
	{
		if (!m_bActive)
			return;

		if (!m_Tracker || !m_Tracker.HasValidData())
			return;

		CleanDeadGroups();
		TrySpawn();
	}

	// --------------------------------------------------------
	// Waypoint refresh + cleanup tick
	// --------------------------------------------------------
	protected void WaypointTick()
	{
		if (!m_bActive)
			return;

		if (!m_Tracker || !m_Tracker.HasValidData())
			return;

		for (int i = m_aActiveGroups.Count() - 1; i >= 0; i--)
		{
			EEF_HunterGroupData data = m_aActiveGroups[i];

			if (!data || !data.m_Group)
			{
				m_aActiveGroups.Remove(i);
				continue;
			}

			// Cleanup distant groups
			if (m_bCleanupDistant && IsGroupTooFar(data.m_Group))
			{
				DebugLog("Cleaning up distant group.");
				SCR_EntityHelper.DeleteEntityAndChildren(data.m_Group);
				m_aActiveGroups.Remove(i);
				continue;
			}

			// Refresh waypoint
			if (m_bAutoRefreshWaypoints)
			{
				vector target = GetTargetForGroup(data);
				RefreshWaypoint(data.m_Group, target);
			}

			// Periodic group flare — each group has its own staggered timer
			if (m_bPeriodicGroupFlare && !m_sFlarePrefab.IsEmpty())
			{
				float currentTime = GetGame().GetWorld().GetWorldTime() / 1000.0;
				if (currentTime - data.m_fLastFlareTime >= m_fGroupFlareInterval)
				{
					vector groupPos = data.m_Group.GetOrigin();
					groupPos[1] = groupPos[1] + m_fGroupFlareHeight;
					SpawnFlare(groupPos);
					data.m_fLastFlareTime = currentTime;
					DebugLog(string.Format("Periodic flare spawned above group at %1", groupPos));
				}
			}
		}
	}

	// --------------------------------------------------------
	// Main spawn attempt
	// --------------------------------------------------------
	protected void TrySpawn()
	{
		if (m_aActiveGroups.Count() >= m_iMaxActiveGroups)
		{
			DebugLog(string.Format("At max groups (%1). Skipping spawn.", m_iMaxActiveGroups));
			return;
		}

		if (m_sGroupPrefab.IsEmpty())
			return;

		vector spawnCenterpoint = GetSpawnCenterpoint();
		vector movementVec = m_Tracker.GetMovementVector();

		EEF_EHemisphereMode modeThisCycle = m_eHemisphereMode;
		if (m_eHemisphereMode == EEF_EHemisphereMode.RANDOM)
		{
			if (Math.RandomInt(0, 2) == 0)
				modeThisCycle = EEF_EHemisphereMode.CHASE;
			else
				modeThisCycle = EEF_EHemisphereMode.BLOCK;
		}

		vector spawnPoint;
		if (!FindSpawnPoint(spawnCenterpoint, movementVec, modeThisCycle, spawnPoint))
		{
			DebugLog("Could not find valid spawn point. Skipping cycle.");
			return;
		}

		IEntity group = SpawnGroup(spawnPoint);
		if (!group)
		{
			DebugLog("SpawnGroup returned null. Check prefab path.");
			return;
		}

		// Determine cluster assignment for SPLIT mode
		int clusterIndex = 0;
		if (m_ePlayerMode == EEF_EPlayerMode.SPLIT)
			clusterIndex = GetLeastPopulatedClusterIndex();

		// Stagger the flare timer so groups don't all fire at the same time.
		// Each group is offset by an even slice of the interval based on how
		// many groups are currently active when it spawns.
		float currentTime = GetGame().GetWorld().GetWorldTime() / 1000.0;
		int groupIndex = m_aActiveGroups.Count(); // count before insert = index of this group
		float staggerOffset = 0;
		if (m_bPeriodicGroupFlare && m_fGroupFlareInterval > 0 && m_iMaxActiveGroups > 1)
			staggerOffset = (m_fGroupFlareInterval / m_iMaxActiveGroups) * groupIndex;
		float initialFlareTime = currentTime - m_fGroupFlareInterval + staggerOffset;

		EEF_HunterGroupData data = new EEF_HunterGroupData(group, clusterIndex, initialFlareTime);
		m_aActiveGroups.Insert(data);

		// Assign initial waypoint
		vector target = GetTargetForGroup(data);
		AssignWaypoint(group, target);

		if (m_bFlareOnSpawn && !m_sFlarePrefab.IsEmpty())
		{
			vector flarePos = spawnPoint;
			flarePos[1] = flarePos[1] + 2.0;
			SpawnFlare(flarePos);
		}

		DebugLog(string.Format("Spawned group at %1. Active groups: %2", spawnPoint, m_aActiveGroups.Count()));
	}

	// --------------------------------------------------------
	// Determine spawn centerpoint based on player mode
	// --------------------------------------------------------
	protected vector GetSpawnCenterpoint()
	{
		switch (m_ePlayerMode)
		{
			case EEF_EPlayerMode.CENTROID:
			{
				// Use mean centerpoint unless players are too spread out
				if (m_Tracker.GetPlayerSpread() > m_fDispersionThreshold)
				{
					DebugLog("Players dispersed beyond threshold. Targeting largest cluster.");
					return m_Tracker.GetLargestClusterCenterpoint();
				}
				return m_Tracker.GetCenterpoint();
			}

			case EEF_EPlayerMode.SPLIT:
			{
				// Spawn on edge of largest cluster for initial placement
				return m_Tracker.GetLargestClusterCenterpoint();
			}

			case EEF_EPlayerMode.NEAREST:
			{
				// NEAREST uses global centerpoint for spawning
				// individual groups will then seek nearest player on waypoint refresh
				return m_Tracker.GetCenterpoint();
			}
		}

		return m_Tracker.GetCenterpoint();
	}

	// --------------------------------------------------------
	// Determine waypoint target for a specific group
	// based on current player mode
	// --------------------------------------------------------
	protected vector GetTargetForGroup(EEF_HunterGroupData data)
	{
		switch (m_ePlayerMode)
		{
			case EEF_EPlayerMode.CENTROID:
			{
				if (m_Tracker.GetPlayerSpread() > m_fDispersionThreshold)
					return m_Tracker.GetLargestClusterCenterpoint();

				return m_Tracker.GetCenterpoint();
			}

			case EEF_EPlayerMode.SPLIT:
			{
				array<ref EEF_PlayerCluster> clusters = m_Tracker.GetClusters();

				if (clusters.IsEmpty())
					return m_Tracker.GetCenterpoint();

				// Clamp cluster index in case cluster count changed
				int idx = data.m_iAssignedClusterIndex;
				if (idx >= clusters.Count())
					idx = clusters.Count() - 1;

				return clusters[idx].m_vCenterpoint;
			}

			case EEF_EPlayerMode.NEAREST:
			{
				vector nearest;
				if (m_Tracker.GetNearestPlayerPosition(data.m_Group.GetOrigin(), nearest))
					return nearest;

				return m_Tracker.GetCenterpoint();
			}
		}

		return m_Tracker.GetCenterpoint();
	}

	// --------------------------------------------------------
	// In SPLIT mode - find which cluster has fewest assigned groups
	// --------------------------------------------------------
	protected int GetLeastPopulatedClusterIndex()
	{
		array<ref EEF_PlayerCluster> clusters = m_Tracker.GetClusters();
		if (clusters.IsEmpty())
			return 0;

		array<int> counts = new array<int>();
		for (int i = 0; i < clusters.Count(); i++)
			counts.Insert(0);

		foreach (EEF_HunterGroupData data : m_aActiveGroups)
		{
		    int idx = data.m_iAssignedClusterIndex;
		    if (idx < counts.Count())
		    {
		        int currentCount = counts[idx];
		        counts[idx] = currentCount + 1;
		    }
		}

		int lowestCount = int.MAX;
		int lowestIdx = 0;
		for (int i = 0; i < counts.Count(); i++)
		{
			if (counts[i] < lowestCount)
			{
				lowestCount = counts[i];
				lowestIdx = i;
			}
		}

		return lowestIdx;
	}

	// --------------------------------------------------------
	// Check if group is too far from all living players
	// --------------------------------------------------------
	protected bool IsGroupTooFar(IEntity group)
	{
		if (!group)
			return true;

		array<vector> playerPositions = m_Tracker.GetLivingPlayerPositions();
		if (playerPositions.IsEmpty())
			return false;

		float cleanupDistSq = m_fCleanupDistance * m_fCleanupDistance;
		vector groupPos = group.GetOrigin();

		foreach (vector playerPos : playerPositions)
		{
			vector delta = groupPos - playerPos;
			if (delta.LengthSq() <= cleanupDistSq)
				return false;
		}

		return true;
	}

	// --------------------------------------------------------
	// Find valid spawn point on circle edge with hemisphere filter
	// --------------------------------------------------------
	protected bool FindSpawnPoint(vector centerpoint, vector movementVec, EEF_EHemisphereMode mode, out vector result)
	{
		bool movementValid = movementVec.LengthSq() > 0.01;
		float cosAngle = Math.Cos(m_fHemisphereAngle * Math.DEG2RAD);

		for (int attempt = 0; attempt < m_iMaxSpawnAttempts; attempt++)
		{
			float angle = Math.RandomFloat(0, Math.PI2);
			vector candidate = Vector(
				centerpoint[0] + m_fHuntRadius * Math.Sin(angle),
				centerpoint[1],
				centerpoint[2] + m_fHuntRadius * Math.Cos(angle)
			);

			if (movementValid && mode != EEF_EHemisphereMode.BOTH)
			{
				vector toCandidate = (candidate - centerpoint).Normalized();
				float dot = vector.Dot(toCandidate, movementVec);

				if (mode == EEF_EHemisphereMode.BLOCK && dot < cosAngle)
					continue;
				if (mode == EEF_EHemisphereMode.CHASE && dot > -cosAngle)
					continue;
			}

			float surfaceY = GetGame().GetWorld().GetSurfaceY(candidate[0], candidate[2]);
			candidate[1] = surfaceY;

			if (surfaceY < -100)
				continue;

			result = candidate;
			return true;
		}

		return false;
	}

	// --------------------------------------------------------
	// Spawn AI group prefab at world position
	// --------------------------------------------------------
	protected IEntity SpawnGroup(vector position)
	{
		Resource groupResource = Resource.Load(m_sGroupPrefab);
		if (!groupResource || !groupResource.IsValid())
		{
			Print("[EEF Spawner] ERROR: Could not load group prefab: " + m_sGroupPrefab, LogLevel.ERROR);
			return null;
		}

		EntitySpawnParams spawnParams = new EntitySpawnParams();
		spawnParams.TransformMode = ETransformMode.WORLD;
		Math3D.MatrixIdentity4(spawnParams.Transform);
		spawnParams.Transform[3] = position;

		return GetGame().SpawnEntityPrefab(groupResource, GetGame().GetWorld(), spawnParams);
	}

	// --------------------------------------------------------
	// Assign initial waypoint to a freshly spawned group
	// --------------------------------------------------------
	protected void AssignWaypoint(IEntity group, vector targetPos)
	{
		if (!group)
			return;

		SCR_AIGroup aiGroup = SCR_AIGroup.Cast(group);
		if (!aiGroup)
		{
			DebugLog("WARNING: Could not cast group to SCR_AIGroup. Waypoint not assigned.");
			return;
		}

		vector waypointPos = ScatterPosition(targetPos);

		AIWaypoint waypoint = AIWaypoint.Cast(
		    GetGame().SpawnEntityPrefab(
		        Resource.Load(m_sWaypointPrefab),
		        GetGame().GetWorld()
		    )
		);

		if (!waypoint)
		{
			DebugLog("WARNING: Failed to spawn AIWaypoint entity.");
			return;
		}

		waypoint.SetOrigin(waypointPos);
		aiGroup.AddWaypoint(waypoint);

		DebugLog(string.Format("Waypoint assigned at %1", waypointPos));
	}

	// --------------------------------------------------------
	// Refresh waypoint for an already-active group
	// Clears existing waypoints before adding new one
	// to prevent waypoint queue buildup
	// --------------------------------------------------------
	protected void RefreshWaypoint(IEntity group, vector targetPos)
	{
		if (!group)
			return;

		SCR_AIGroup aiGroup = SCR_AIGroup.Cast(group);
		if (!aiGroup)
			return;

		// Get all current waypoints and remove them
		array<AIWaypoint> queue = {};
		aiGroup.GetWaypoints(queue);
		foreach (AIWaypoint wp : queue)
		{
		    aiGroup.RemoveWaypoint(wp);
		}

		vector waypointPos = ScatterPosition(targetPos);

		AIWaypoint waypoint = AIWaypoint.Cast(
		    GetGame().SpawnEntityPrefab(
		        Resource.Load(m_sWaypointPrefab),
		        GetGame().GetWorld()
		    )
		);

		if (!waypoint)
		{
			DebugLog("WARNING: Failed to spawn AIWaypoint for refresh.");
			return;
		}

		waypoint.SetOrigin(waypointPos);
		aiGroup.AddWaypoint(waypoint);
	}

	// --------------------------------------------------------
	// Returns a position scattered randomly within
	// m_fWaypointScatter metres of the given target
	// --------------------------------------------------------
	protected vector ScatterPosition(vector targetPos)
	{
		float angle = Math.RandomFloat(0, Math.PI2);
		float distance = Math.RandomFloat(0, m_fWaypointScatter);

		vector scattered = Vector(
			targetPos[0] + distance * Math.Sin(angle),
			targetPos[1],
			targetPos[2] + distance * Math.Cos(angle)
		);

		scattered[1] = GetGame().GetWorld().GetSurfaceY(scattered[0], scattered[2]);
		return scattered;
	}

	// --------------------------------------------------------
	// Spawn illumination flare at a given world position.
	// Callers are responsible for setting the desired height
	// before passing the position in.
	// --------------------------------------------------------
	protected void SpawnFlare(vector position)
	{
		Resource flareResource = Resource.Load(m_sFlarePrefab);
		if (!flareResource || !flareResource.IsValid())
		{
			DebugLog("WARNING: Could not load flare prefab.");
			return;
		}

		EntitySpawnParams spawnParams = new EntitySpawnParams();
		spawnParams.TransformMode = ETransformMode.WORLD;
		Math3D.MatrixIdentity4(spawnParams.Transform);
		spawnParams.Transform[3] = position;

		GetGame().SpawnEntityPrefab(flareResource, GetGame().GetWorld(), spawnParams);
	}

	// --------------------------------------------------------
	// Remove groups whose entity is gone or has no living members
	// --------------------------------------------------------
	protected void CleanDeadGroups()
	{
		for (int i = m_aActiveGroups.Count() - 1; i >= 0; i--)
		{
			EEF_HunterGroupData data = m_aActiveGroups[i];

			if (!data || !data.m_Group)
			{
				m_aActiveGroups.Remove(i);
				continue;
			}

			SCR_AIGroup aiGroup = SCR_AIGroup.Cast(data.m_Group);
			if (aiGroup && aiGroup.GetAgentsCount() == 0)
			{
				m_aActiveGroups.Remove(i);
				DebugLog("Removed dead group from active list.");
			}
		}
	}

	// --------------------------------------------------------
	// Public activation API
	// Called by EEF_ScenarioFrameworkActionStartHunter /
	// EEF_ScenarioFrameworkActionStopHunter or any other
	// component that needs to start/stop the hunter.
	// --------------------------------------------------------
	void StartHunter()
	{
		if (m_bActive)
			return;

		m_bActive = true;
		DebugLog("Hunter activated.");
	}

	void StopHunter(bool cleanupGroups = true)
	{
		if (!m_bActive)
			return;

		m_bActive = false;

		if (cleanupGroups)
		{
			foreach (EEF_HunterGroupData data : m_aActiveGroups)
			{
				if (data && data.m_Group)
					SCR_EntityHelper.DeleteEntityAndChildren(data.m_Group);
			}
			m_aActiveGroups.Clear();
		}

		DebugLog("Hunter deactivated.");
	}

	// --------------------------------------------------------
	// Gated debug logging
	// --------------------------------------------------------
	protected void DebugLog(string message)
	{
		if (m_bDebugLog)
			Print("[EEF Spawner] " + message);
	}
}