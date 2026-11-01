// ============================================================
// EEF_ScenarioFrameworkActionStartAmbientCombat.c
// Enfusion Extended Framework
//
// A Scenario Framework Action that starts a specific Ambient
// Combat instance. Add this to the "Finished Actions" or "Entity
// Left Actions" list on a Plugin Trigger in the World Editor. Set
// the Anchor Entity Name to the name you gave the entity carrying
// the EEF_AmbientCombatComponent.
//
// Available regardless of m_bAutoActivateOnInit.
// ============================================================

[BaseContainerProps(), SCR_ContainerActionTitle()]
class EEF_ScenarioFrameworkActionStartAmbientCombat : SCR_ScenarioFrameworkActionBase
{
	[Attribute("", UIWidgets.EditBox, "Name of the Ambient Combat anchor entity in the World Editor. Must carry an EEF_AmbientCombatComponent.")]
	protected string m_sAnchorEntityName;

	override void OnActivate(IEntity object)
	{
		if (!CanActivate())
			return;

		EEF_AmbientCombatComponent battle = EEF_AmbientCombatActionHelper.Resolve(m_sAnchorEntityName, "ActionStartAmbientCombat");
		if (battle)
			battle.StartBattle();
	}
}

//------------------------------------------------------------------------------------------------
//! Shared anchor lookup for the Ambient Combat actions (logs its own errors, returns null on failure).
class EEF_AmbientCombatActionHelper
{
	static EEF_AmbientCombatComponent Resolve(string anchorName, string actionName)
	{
		if (anchorName.IsEmpty())
		{
			Print(string.Format("[EEF] %1: Anchor Entity Name is empty.", actionName), LogLevel.ERROR);
			return null;
		}

		IEntity anchor = GetGame().GetWorld().FindEntityByName(anchorName);
		if (!anchor)
		{
			Print(string.Format("[EEF] %1: Anchor entity '%2' not found in world.", actionName, anchorName), LogLevel.ERROR);
			return null;
		}

		EEF_AmbientCombatComponent battle = EEF_AmbientCombatComponent.Cast(anchor.FindComponent(EEF_AmbientCombatComponent));
		if (!battle)
			Print(string.Format("[EEF] %1: Entity '%2' has no EEF_AmbientCombatComponent.", actionName, anchorName), LogLevel.ERROR);

		return battle;
	}
}
