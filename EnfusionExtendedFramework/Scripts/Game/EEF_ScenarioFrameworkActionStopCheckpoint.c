// ============================================================
// EEF_ScenarioFrameworkActionStopCheckpoint.c
// Enfusion Extended Framework
//
// A Scenario Framework Action that stops a checkpoint on a
// specific checkpoint entity in the world.
//
// Add this to the "Finished Actions" or "Entity Left Actions"
// list on a Plugin Trigger in the World Editor. Set the
// Checkpoint Entity Name to match the name you gave the
// checkpoint trigger entity (the one carrying
// EEF_CheckpointComponent) in the World Editor hierarchy.
// ============================================================

[BaseContainerProps(), SCR_ContainerActionTitle()]
class EEF_ScenarioFrameworkActionStopCheckpoint : SCR_ScenarioFrameworkActionBase
{
	[Attribute("", UIWidgets.EditBox, "Name of the checkpoint entity in the World Editor. Must carry an EEF_CheckpointComponent.")]
	protected string m_sCheckpointEntityName;

	[Attribute("1", UIWidgets.CheckBox, "Delete spawned vehicles and their occupants when stopping.")]
	protected bool m_bCleanupOnStop;

	override void OnActivate(IEntity object)
	{
		if (!CanActivate())
			return;

		if (m_sCheckpointEntityName.IsEmpty())
		{
			Print("[EEF] ActionStopCheckpoint: Checkpoint Entity Name is empty.", LogLevel.ERROR);
			return;
		}

		IEntity checkpoint = GetGame().GetWorld().FindEntityByName(m_sCheckpointEntityName);
		if (!checkpoint)
		{
			Print(string.Format("[EEF] ActionStopCheckpoint: Checkpoint entity '%1' not found in world.", m_sCheckpointEntityName), LogLevel.ERROR);
			return;
		}

		EEF_CheckpointComponent component = EEF_CheckpointComponent.Cast(
			checkpoint.FindComponent(EEF_CheckpointComponent)
		);

		if (!component)
		{
			Print(string.Format("[EEF] ActionStopCheckpoint: Entity '%1' has no EEF_CheckpointComponent.", m_sCheckpointEntityName), LogLevel.ERROR);
			return;
		}

		component.StopCheckpoint(m_bCleanupOnStop);
	}
}
