// ============================================================
// EEF_DetaineeActions.c
// Enfusion Extended Framework
//
// Interactions a player uses on a DETAINED crew member (the character, at its head):
//   - EEF_DetaineeRecruitAction     take into the player's squad (swaps side; the vanilla commanding menu then works)
//   - EEF_DetaineeDropAction        remove from the squad and delete the unit
//   - EEF_DetaineeSitAction         stand up / sit down
//   - EEF_DetaineeRoadsideAction    send back to the sitting spot beside the road
// They are added to the crew character prefab (see Prefabs/Checkpoint/EEF_Detainee_US_Driver.et). The
// checkpoint ignores them unless that character is a detained crew member.
// ============================================================

class EEF_DetaineeAction : ScriptedUserAction
{
	//! Which EEF_CheckpointComponent.CREW_COMMAND_* this action sends.
	protected int GetCommand()
	{
		return EEF_CheckpointComponent.CREW_COMMAND_TOGGLE_SIT;
	}

	//! 0 = only while the unit is still a detainee, 1 = only once taken into the squad, 2 = either.
	protected int GetRecruitedMode()
	{
		return 0;
	}

	//------------------------------------------------------------------------------------------------
	override bool CanBeShownScript(IEntity user)
	{
		ChimeraCharacter userCharacter = ChimeraCharacter.Cast(user);
		if (!userCharacter || userCharacter.IsInVehicle())
			return false;

		// Not while the crew member is still inside their car.
		ChimeraCharacter target = ChimeraCharacter.Cast(GetOwner());
		if (!target || target.IsInVehicle())
			return false;

		SCR_ChimeraCharacter unit = SCR_ChimeraCharacter.Cast(target);
		int mode = GetRecruitedMode();
		if (unit && mode == 0 && unit.IsRecruited())
			return false;
		if (unit && mode == 1 && !unit.IsRecruited())
			return false;

		return true;
	}

	//------------------------------------------------------------------------------------------------
	override bool CanBePerformedScript(IEntity user)
	{
		return CanBeShownScript(user);
	}

	//------------------------------------------------------------------------------------------------
	override void PerformAction(IEntity pOwnerEntity, IEntity pUserEntity)
	{
		// Only the server owns the checkpoint state.
		if (!Replication.IsServer() || !pOwnerEntity || !pUserEntity)
			return;

		EEF_CheckpointComponent.DirectCrewEntity(pOwnerEntity, GetCommand(), pUserEntity);
	}
}

class EEF_DetaineeSitAction : EEF_DetaineeAction
{
	override bool GetActionNameScript(out string outName)
	{
		outName = "Stand up / sit down";
		return true;
	}
}

class EEF_DetaineeRoadsideAction : EEF_DetaineeAction
{
	override protected int GetCommand()
	{
		return EEF_CheckpointComponent.CREW_COMMAND_TO_ROADSIDE;
	}

	override bool GetActionNameScript(out string outName)
	{
		outName = "Send to roadside";
		return true;
	}
}

class EEF_DetaineeRecruitAction : EEF_DetaineeAction
{
	override protected int GetCommand()
	{
		return EEF_CheckpointComponent.CREW_COMMAND_RECRUIT;
	}

	override bool GetActionNameScript(out string outName)
	{
		outName = "Take into squad";
		return true;
	}
}

class EEF_DetaineeDropAction : EEF_DetaineeAction
{
	override protected int GetCommand()
	{
		return EEF_CheckpointComponent.CREW_COMMAND_DROP;
	}

	override protected int GetRecruitedMode()
	{
		return 2;
	}

	override bool GetActionNameScript(out string outName)
	{
		outName = "Drop unit";
		return true;
	}
}
