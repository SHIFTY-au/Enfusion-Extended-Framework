// ============================================================
// EEF_DetainVehicleAction.c
// Enfusion Extended Framework
//
// "Detain vehicle": the interaction a player uses at a queued car's driver door to pull it off the road
// instead of letting it through the roadblock. Added to the checkpoint vehicle prefab's driver door
// context next to "Clear vehicle" (see Prefabs/Vehicles/Wheeled/S105/Checkpoint_S105_randomized.et).
// The checkpoint ignores it unless that vehicle is waiting in its queue.
// ============================================================

class EEF_DetainVehicleAction : EEF_ClearVehicleAction
{
	//------------------------------------------------------------------------------------------------
	override bool GetActionNameScript(out string outName)
	{
		outName = "Detain vehicle";
		return true;
	}

	//------------------------------------------------------------------------------------------------
	override void PerformAction(IEntity pOwnerEntity, IEntity pUserEntity)
	{
		// Only the server owns the checkpoint state.
		if (!Replication.IsServer() || !pOwnerEntity)
			return;

		EEF_CheckpointComponent.DetainVehicleEntity(pOwnerEntity.GetRootParent());
	}
}
