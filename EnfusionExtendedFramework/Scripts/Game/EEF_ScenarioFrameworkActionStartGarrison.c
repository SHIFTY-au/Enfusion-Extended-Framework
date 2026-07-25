// ============================================================
// EEF_ScenarioFrameworkActionStartGarrison.c
// Enfusion Extended Framework
//
// A Scenario Framework Action that starts a specific Garrison
// instance. Add this to the "Finished Actions" or "Entity Left
// Actions" list on a Plugin Trigger in the World Editor. Set the
// Anchor Entity Name to match the name you gave the Garrison
// anchor entity in the World Editor entity hierarchy.
//
// Available regardless of m_bAutoActivateOnInit, so a mission
// maker can manually start an instance that had auto-activate
// disabled.
// ============================================================

[BaseContainerProps(), SCR_ContainerActionTitle()]
class EEF_ScenarioFrameworkActionStartGarrison : SCR_ScenarioFrameworkActionBase
{
	[Attribute("", UIWidgets.EditBox, "Name of the Garrison anchor entity in the World Editor. Must carry an EEF_GarrisonComponent.")]
	protected string m_sAnchorEntityName;

	override void OnActivate(IEntity object)
	{
		if (!CanActivate())
			return;

		if (m_sAnchorEntityName.IsEmpty())
		{
			Print("[EEF] ActionStartGarrison: Anchor Entity Name is empty.", LogLevel.ERROR);
			return;
		}

		IEntity anchor = GetGame().GetWorld().FindEntityByName(m_sAnchorEntityName);
		if (!anchor)
		{
			Print(string.Format("[EEF] ActionStartGarrison: Anchor entity '%1' not found in world.", m_sAnchorEntityName), LogLevel.ERROR);
			return;
		}

		EEF_GarrisonComponent garrison = EEF_GarrisonComponent.Cast(
			anchor.FindComponent(EEF_GarrisonComponent)
		);

		if (!garrison)
		{
			Print(string.Format("[EEF] ActionStartGarrison: Entity '%1' has no EEF_GarrisonComponent.", m_sAnchorEntityName), LogLevel.ERROR);
			return;
		}

		garrison.StartGarrison();
	}
}
