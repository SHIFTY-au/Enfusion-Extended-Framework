// ============================================================
// EEF_ClearVehicleAction.c
// Enfusion Extended Framework
//
// "Clear vehicle": the interaction a player uses at a queued car's driver door to let it through the
// roadblock. The action is added to the checkpoint vehicle prefab's own ActionsManagerComponent, on the
// driver door context (see Prefabs/Vehicles/Wheeled/S105/Checkpoint_S105_randomized.et - copy that entry
// onto any other vehicle prefab you use in the pool). The action tells the checkpoint WHICH vehicle was
// used; the checkpoint ignores it unless that vehicle is waiting in its queue.
// ============================================================

class EEF_ClearVehicleAction : ScriptedUserAction
{
	//------------------------------------------------------------------------------------------------
	override bool GetActionNameScript(out string outName)
	{
		outName = "Clear vehicle";
		return true;
	}

	//------------------------------------------------------------------------------------------------
	override bool CanBeShownScript(IEntity user)
	{
		ChimeraCharacter character = ChimeraCharacter.Cast(user);
		if (!character)
			return false;

		return !character.IsInVehicle();
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
		if (!Replication.IsServer() || !pOwnerEntity)
			return;

		EEF_CheckpointComponent.ClearVehicleEntity(pOwnerEntity.GetRootParent());
	}
}
