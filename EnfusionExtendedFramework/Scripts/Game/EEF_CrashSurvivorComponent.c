// ============================================================
// EEF_CrashSurvivorComponent.c
// Enfusion Extended Framework
//
// GameMode component. Call ApplyRandomInjuries(playerEntity)
// on any player entity to simulate crash survival injuries:
// reduced health on random limbs + optional bleeding.
//
// Designed to be called from a Scenario Framework Action
// (EEF_ScenarioFrameworkActionApplyCrashInjuries) which you
// attach to the SpawnPoint Plugin's "Actions On Spawn Point Used"
// list in the World Editor.
//
// Runs on SERVER (authority) only.
// ============================================================

[ComponentEditorProps(category: "EEF/Scenario", description: "Applies random crash survival injuries to players on spawn. Call via EEF_ScenarioFrameworkActionApplyCrashInjuries.")]
class EEF_CrashSurvivorComponentClass : SCR_BaseGameModeComponentClass {}

class EEF_CrashSurvivorComponent : SCR_BaseGameModeComponent
{
	// --------------------------------------------------------
	// Config
	// --------------------------------------------------------

	[Attribute("0.4", UIWidgets.EditBox, "Minimum health a damaged limb can be set to. 0.0 = nearly dead, 1.0 = full health.")]
	protected float m_fMinLimbHealth;

	[Attribute("0.8", UIWidgets.EditBox, "Maximum health a damaged limb can be set to. Should be below 1.0 to ensure visible injury.")]
	protected float m_fMaxLimbHealth;

	[Attribute("1", UIWidgets.EditBox, "Minimum number of limbs to injure per player.")]
	protected int m_iMinInjuredLimbs;

	[Attribute("3", UIWidgets.EditBox, "Maximum number of limbs to injure per player.")]
	protected int m_iMaxInjuredLimbs;

	[Attribute("1", UIWidgets.CheckBox, "If true, at least one injured limb will also be set bleeding.")]
	protected bool m_bApplyBleeding;

	[Attribute("1", UIWidgets.CheckBox, "Print debug info to console.")]
	protected bool m_bDebugLog;

	// --------------------------------------------------------
	// Hit zone names for character limbs.
	// These match the physical HitZone names used by the engine.
	// Head and torso are excluded — this is a survivor, not
	// someone who took a bullet.
	// --------------------------------------------------------
	protected ref array<string> LIMB_HITZONES = {
	    "Head",
	    "Chest", // Not Torso
	    "Abdomen",
	    "LeftArm", // Note the full name, no space
	    "RightArm",
	    "LeftLeg",
	    "RightLeg"
	};

	// --------------------------------------------------------
	// Public API — called by EEF_ScenarioFrameworkActionApplyCrashInjuries
	// --------------------------------------------------------
	void ApplyRandomInjuries(IEntity playerEntity)
	{
		if (!Replication.IsServer())
			return;

		if (!playerEntity)
		{
			DebugLog("ApplyRandomInjuries: playerEntity is null.");
			return;
		}

		SCR_CharacterDamageManagerComponent damageManager = SCR_CharacterDamageManagerComponent.Cast(
			playerEntity.FindComponent(SCR_CharacterDamageManagerComponent)
		);

		if (!damageManager)
		{
			DebugLog("ApplyRandomInjuries: No SCR_CharacterDamageManagerComponent found on player.");
			return;
		}

		// Pick how many limbs to injure this player
		int injuryCount = Math.RandomInt(m_iMinInjuredLimbs, m_iMaxInjuredLimbs + 1);
		injuryCount = Math.Clamp(injuryCount, 1, LIMB_HITZONES.Count());

		// Shuffle a copy of the limb list so we pick randomly without repeats
		array<string> shuffled = new array<string>();
		foreach (string hz : LIMB_HITZONES)
			shuffled.Insert(hz);

		ShuffleArray(shuffled);

		bool bleedingApplied = false;

		for (int i = 0; i < injuryCount; i++)
		{
			string hitZoneName = shuffled[i];

			HitZone hitZone = damageManager.GetHitZoneByName(hitZoneName);
			if (!hitZone)
			{
				DebugLog(string.Format("HitZone not found: %1", hitZoneName));
				continue;
			}

			// Set health to a random value within configured range
			float targetHealth = Math.RandomFloat(m_fMinLimbHealth, m_fMaxLimbHealth);
			hitZone.SetHealth(targetHealth * hitZone.GetMaxHealth());

			DebugLog(string.Format("Injured %1 — health set to %.0f%%", hitZoneName, targetHealth * 100));

			// Apply bleeding to first injured limb if enabled
			if (m_bApplyBleeding && !bleedingApplied)
			{
				damageManager.AddParticularBleeding(hitZoneName);
				bleedingApplied = true;
				DebugLog(string.Format("Bleeding applied to %1", hitZoneName));
			}
		}

		DebugLog(string.Format("Injuries applied to player: %1 limbs affected.", injuryCount));
	}

	// --------------------------------------------------------
	// Fisher-Yates shuffle on a string array
	// --------------------------------------------------------
	protected void ShuffleArray(notnull array<string> arr)
	{
		for (int i = arr.Count() - 1; i > 0; i--)
		{
			int j = Math.RandomInt(0, i + 1);
			string temp = arr[i];
			arr[i] = arr[j];
			arr[j] = temp;
		}
	}

	// --------------------------------------------------------
	// Auto-injection logic for non-Scenario Framework setups
	// --------------------------------------------------------
	override void OnPostInit(IEntity owner)
	{
		super.OnPostInit(owner);
		
		// Only the server should calculate and apply damage
		if (!Replication.IsServer()) 
			return;

		SCR_BaseGameMode gameMode = SCR_BaseGameMode.Cast(GetGame().GetGameMode());
		if (gameMode)
		{
			// Subscribe to the global spawn event
			gameMode.GetOnPlayerSpawned().Insert(OnPlayerSpawned);
		}
	}

	protected void OnPlayerSpawned(int playerId, IEntity player)
	{
		// Wait 500ms (0.5s) to ensure the character is fully 
		// initialized in the world before applying damage.
		GetGame().GetCallqueue().CallLater(ApplyRandomInjuries, 500, false, player);
	}
	
	// --------------------------------------------------------
	// Utility
	// --------------------------------------------------------
	protected void DebugLog(string message)
	{
		if (m_bDebugLog)
			Print("[EEF CrashSurvivor] " + message);
	}
}
