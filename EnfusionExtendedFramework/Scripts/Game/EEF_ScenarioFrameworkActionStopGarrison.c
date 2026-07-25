// ============================================================
// EEF_ScenarioFrameworkActionStopGarrison.c
// Enfusion Extended Framework
//
// A Scenario Framework Action that stops a specific Garrison
// instance. Add this to the "Finished Actions" or "Entity Left
// Actions" list on a Plugin Trigger in the World Editor. Set the
// Anchor Entity Name to match the name you gave the Garrison
// anchor entity in the World Editor entity hierarchy.
//
// Stop always despawns and cleans up all currently active AI for
// the instance. A subsequent Start rolls a brand new population -
// no state is persisted.
// ============================================================

[BaseContainerProps(), SCR_ContainerActionTitle()]
class EEF_ScenarioFrameworkActionStopGarrison : SCR_ScenarioFrameworkActionBase
{
	[Attribute("", UIWidgets.EditBox, "Name of the Garrison anchor entity in the World Editor. Must carry an EEF_GarrisonComponent.")]
	protected string m_sAnchorEntityName;

	[Attribute("1", UIWidgets.CheckBox, "Delete currently active garrison AI when stopping.")]
	protected bool m_bCleanupOnStop;

	override void OnActivate(IEntity object)
	{
		if (!CanActivate())
			return;

		if (m_sAnchorEntityName.IsEmpty())
		{
			Print("[EEF] ActionStopGarrison: Anchor Entity Name is empty.", LogLevel.ERROR);
			return;
		}

		IEntity anchor = GetGame().GetWorld().FindEntityByName(m_sAnchorEntityName);
		if (!anchor)
		{
			Print(string.Format("[EEF] ActionStopGarrison: Anchor entity '%1' not found in world.", m_sAnchorEntityName), LogLevel.ERROR);
			return;
		}

		EEF_GarrisonComponent garrison = EEF_GarrisonComponent.Cast(
			anchor.FindComponent(EEF_GarrisonComponent)
		);

		if (!garrison)
		{
			Print(string.Format("[EEF] ActionStopGarrison: Entity '%1' has no EEF_GarrisonComponent.", m_sAnchorEntityName), LogLevel.ERROR);
			return;
		}

		garrison.StopGarrison(m_bCleanupOnStop);
	}
}
