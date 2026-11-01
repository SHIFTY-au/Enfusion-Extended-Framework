// ============================================================
// EEF_ScenarioFrameworkActionStopAmbientCombat.c
// Enfusion Extended Framework
//
// A Scenario Framework Action that stops a specific Ambient
// Combat instance. Shots already scheduled on clients (a few
// seconds at most) play out; nothing new is started.
// ============================================================

[BaseContainerProps(), SCR_ContainerActionTitle()]
class EEF_ScenarioFrameworkActionStopAmbientCombat : SCR_ScenarioFrameworkActionBase
{
	[Attribute("", UIWidgets.EditBox, "Name of the Ambient Combat anchor entity in the World Editor. Must carry an EEF_AmbientCombatComponent.")]
	protected string m_sAnchorEntityName;

	override void OnActivate(IEntity object)
	{
		if (!CanActivate())
			return;

		EEF_AmbientCombatComponent battle = EEF_AmbientCombatActionHelper.Resolve(m_sAnchorEntityName, "ActionStopAmbientCombat");
		if (battle)
			battle.StopBattle();
	}
}
