// ============================================================
// EEF_ScenarioFrameworkActionStopHunter.c
// Enfusion Extended Framework
//
// A Scenario Framework Action that stops the Hunter spawner.
// Add this to the "Finished Actions" or "Entity Left Actions"
// list on a Plugin Trigger in the World Editor.
// ============================================================

[BaseContainerProps(), SCR_ContainerActionTitle()]
class EEF_ScenarioFrameworkActionStopHunter : SCR_ScenarioFrameworkActionBase
{
	[Attribute("1", UIWidgets.CheckBox, "Delete spawned AI groups when stopping.")]
	protected bool m_bCleanupOnStop;

	override void OnActivate(IEntity object)
	{
		if (!CanActivate())
			return;

		EEF_HunterSpawnerComponent spawner = EEF_HunterSpawnerComponent.Cast(
			GetGame().GetGameMode().FindComponent(EEF_HunterSpawnerComponent)
		);

		if (!spawner)
		{
			Print("[EEF] ActionStopHunter: EEF_HunterSpawnerComponent not found on GameMode.", LogLevel.ERROR);
			return;
		}

		spawner.StopHunter(m_bCleanupOnStop);
	}
}
