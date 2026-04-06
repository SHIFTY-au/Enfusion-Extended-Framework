// ============================================================
// EEF_HunterTrackerComponent.c
// Enfusion Extended Framework
//
// Tracks living player positions, computes centerpoint,
// movement vector, and player clusters for split detection.
// Runs on SERVER (authority) only.
// ============================================================

// Represents a detected cluster of players
class EEF_PlayerCluster
{
	vector m_vCenterpoint;
	int m_iPlayerCount;
	ref array<int> m_aPlayerIds = new array<int>();

	void EEF_PlayerCluster(vector centerpoint, int playerId)
	{
		m_vCenterpoint = centerpoint;
		m_iPlayerCount = 1;
		m_aPlayerIds.Insert(playerId);
	}

	// Recalculate centerpoint as mean of all player positions in cluster
	void RecalculateCenterpoint(notnull array<vector> positions)
	{
		if (positions.IsEmpty())
			return;

		vector sum = Vector(0, 0, 0);
		foreach (vector pos : positions)
			sum = sum + pos;

		m_vCenterpoint = Vector(
			sum[0] / positions.Count(),
			sum[1] / positions.Count(),
			sum[2] / positions.Count()
		);
	}
}

[ComponentEditorProps(category: "EEF/Hunter", description: "Tracks player centerpoint, movement vector and clusters for the Hunter system.")]
class EEF_HunterTrackerComponentClass : SCR_BaseGameModeComponentClass {}

class EEF_HunterTrackerComponent : SCR_BaseGameModeComponent
{
	[Attribute("3.0", UIWidgets.EditBox, "How often in seconds to update player positions and movement vector.")]
	protected float m_fUpdateInterval;

	[Attribute("150.0", UIWidgets.EditBox, "Radius in metres within which players are considered the same cluster.")]
	protected float m_fClusterRadius;

	// Internal state
	protected vector m_vCenterpoint;
	protected vector m_vMovementVector;
	protected vector m_vLastCenterpoint;
	protected bool m_bHasValidData;
	protected ref array<ref EEF_PlayerCluster> m_aClusters = new array<ref EEF_PlayerCluster>();

	// Cached flat list of living player positions for external queries
	protected ref array<vector> m_aLivingPlayerPositions = new array<vector>();
	protected ref array<int> m_aLivingPlayerIds = new array<int>();

	override void OnPostInit(IEntity owner)
	{
		super.OnPostInit(owner);

		if (!Replication.IsServer())
			return;

		GetGame().GetCallqueue().CallLater(UpdateTracking, m_fUpdateInterval * 1000, true);
	}

	// --------------------------------------------------------
	// Main tracking update
	// --------------------------------------------------------
	protected void UpdateTracking()
	{
		m_aLivingPlayerPositions.Clear();
		m_aLivingPlayerIds.Clear();

		array<int> playerIds = new array<int>();
		GetGame().GetPlayerManager().GetPlayers(playerIds);

		foreach (int playerId : playerIds)
		{
			IEntity playerEntity = GetGame().GetPlayerManager().GetPlayerControlledEntity(playerId);
			if (!playerEntity)
				continue;

			SCR_CharacterDamageManagerComponent damageManager = SCR_CharacterDamageManagerComponent.Cast(
				playerEntity.FindComponent(SCR_CharacterDamageManagerComponent)
			);

			if (damageManager && damageManager.IsDestroyed())
				continue;

			m_aLivingPlayerPositions.Insert(playerEntity.GetOrigin());
			m_aLivingPlayerIds.Insert(playerId);
		}

		if (m_aLivingPlayerPositions.IsEmpty())
		{
			m_bHasValidData = false;
			m_aClusters.Clear();
			return;
		}

		// Update global centerpoint
		m_vLastCenterpoint = m_vCenterpoint;
		m_vCenterpoint = ComputeMean(m_aLivingPlayerPositions);

		if (m_bHasValidData)
		{
			vector delta = m_vCenterpoint - m_vLastCenterpoint;
			if (delta.LengthSq() > 1.0)
				m_vMovementVector = delta.Normalized();
		}

		m_bHasValidData = true;

		// Update cluster detection
		DetectClusters();
	}

	// --------------------------------------------------------
	// Simple greedy cluster detection
	// Groups players within m_fClusterRadius of each other
	// --------------------------------------------------------
	protected void DetectClusters()
	{
		m_aClusters.Clear();

		array<bool> assigned = new array<bool>();
		foreach (vector pos : m_aLivingPlayerPositions)
			assigned.Insert(false);

		float radiusSq = m_fClusterRadius * m_fClusterRadius;

		for (int i = 0; i < m_aLivingPlayerPositions.Count(); i++)
		{
			if (assigned[i])
				continue;

			// Start new cluster with this player
			EEF_PlayerCluster cluster = new EEF_PlayerCluster(
				m_aLivingPlayerPositions[i],
				m_aLivingPlayerIds[i]
			);
			assigned[i] = true;

			ref array<vector> clusterPositions = new array<vector>();
			clusterPositions.Insert(m_aLivingPlayerPositions[i]);

			// Find all other players within cluster radius
			for (int j = i + 1; j < m_aLivingPlayerPositions.Count(); j++)
			{
				if (assigned[j])
					continue;

				vector delta = m_aLivingPlayerPositions[j] - m_aLivingPlayerPositions[i];
				if (delta.LengthSq() <= radiusSq)
				{
					cluster.m_aPlayerIds.Insert(m_aLivingPlayerIds[j]);
					cluster.m_iPlayerCount++;
					clusterPositions.Insert(m_aLivingPlayerPositions[j]);
					assigned[j] = true;
				}
			}

			cluster.RecalculateCenterpoint(clusterPositions);
			m_aClusters.Insert(cluster);
		}
	}

	// --------------------------------------------------------
	// Utility
	// --------------------------------------------------------
	protected vector ComputeMean(notnull array<vector> positions)
	{
		vector sum = Vector(0, 0, 0);
		foreach (vector pos : positions)
			sum = sum + pos;

		return Vector(
			sum[0] / positions.Count(),
			sum[1] / positions.Count(),
			sum[2] / positions.Count()
		);
	}

	// --------------------------------------------------------
	// Public API
	// --------------------------------------------------------

	vector GetCenterpoint()
	{
		return m_vCenterpoint;
	}

	vector GetMovementVector()
	{
		return m_vMovementVector;
	}

	bool HasValidData()
	{
		return m_bHasValidData;
	}

	// Returns detected clusters sorted largest first
	array<ref EEF_PlayerCluster> GetClusters()
	{
		// Sort by player count descending
		for (int i = 0; i < m_aClusters.Count(); i++)
		{
			for (int j = i + 1; j < m_aClusters.Count(); j++)
			{
				if (m_aClusters[j].m_iPlayerCount > m_aClusters[i].m_iPlayerCount)
				{
					ref EEF_PlayerCluster temp = m_aClusters[i];
					m_aClusters[i] = m_aClusters[j];
					m_aClusters[j] = temp;
				}
			}
		}

		return m_aClusters;
	}

	// Returns largest cluster centerpoint
	vector GetLargestClusterCenterpoint()
	{
		if (m_aClusters.IsEmpty())
			return m_vCenterpoint;

		array<ref EEF_PlayerCluster> sorted = GetClusters();
		return sorted[0].m_vCenterpoint;
	}

	// Returns player spread — max distance between any two living players
	float GetPlayerSpread()
	{
		if (m_aLivingPlayerPositions.Count() < 2)
			return 0.0;

		float maxDistSq = 0.0;
		for (int i = 0; i < m_aLivingPlayerPositions.Count(); i++)
		{
			for (int j = i + 1; j < m_aLivingPlayerPositions.Count(); j++)
			{
				vector delta = m_aLivingPlayerPositions[j] - m_aLivingPlayerPositions[i];
				float distSq = delta.LengthSq();
				if (distSq > maxDistSq)
					maxDistSq = distSq;
			}
		}

		return Math.Sqrt(maxDistSq);
	}

	// Returns all living player positions
	array<vector> GetLivingPlayerPositions()
	{
		return m_aLivingPlayerPositions;
	}

	// Returns nearest living player position to a given world position
	// Returns false if no living players found
	bool GetNearestPlayerPosition(vector fromPosition, out vector result)
	{
		if (m_aLivingPlayerPositions.IsEmpty())
			return false;

		float nearestDistSq = float.MAX;
		vector nearest;

		foreach (vector pos : m_aLivingPlayerPositions)
		{
			vector delta = pos - fromPosition;
			float distSq = delta.LengthSq();
			if (distSq < nearestDistSq)
			{
				nearestDistSq = distSq;
				nearest = pos;
			}
		}

		result = nearest;
		return true;
	}

	int GetClusterCount()
	{
		return m_aClusters.Count();
	}
}