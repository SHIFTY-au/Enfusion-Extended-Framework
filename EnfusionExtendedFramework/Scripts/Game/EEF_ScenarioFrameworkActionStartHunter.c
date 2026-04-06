// ============================================================
// EEF_ScenarioFrameworkActionStartHunter.c
// Enfusion Extended Framework
//
// A Scenario Framework Action that starts the Hunter spawner.
// Add this to the "Finished Actions" or "Entity Left Actions"
// list on a Plugin Trigger in the World Editor.
// ============================================================

[BaseContainerProps(), SCR_ContainerActionTitle()]
class EEF_ScenarioFrameworkActionStartHunter : SCR_ScenarioFrameworkActionBase
{
	override void OnActivate(IEntity object)
	{
		if (!CanActivate())
			return;

		EEF_HunterSpawnerComponent spawner = EEF_HunterSpawnerComponent.Cast(
			GetGame().GetGameMode().FindComponent(EEF_HunterSpawnerComponent)
		);

		if (!spawner)
		{
			Print("[EEF] ActionStartHunter: EEF_HunterSpawnerComponent not found on GameMode.", LogLevel.ERROR);
			return;
		}

		spawner.StartHunter();
	}
}
