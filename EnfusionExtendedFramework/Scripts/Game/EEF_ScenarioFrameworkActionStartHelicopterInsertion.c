// ============================================================
// EEF_ScenarioFrameworkActionStartHelicopterInsertion.c
// Enfusion Extended Framework
//
// A Scenario Framework Action that triggers a helicopter
// insertion sequence on a specific helicopter entity in the
// world.
//
// Add this to the "Finished Actions" or "Entity Left Actions"
// list on a Plugin Trigger in the World Editor. Set the
// Helicopter Entity Name to match the name you gave the
// helicopter in the World Editor entity hierarchy.
// ============================================================

[BaseContainerProps(), SCR_ContainerActionTitle()]
class EEF_ScenarioFrameworkActionStartHelicopterInsertion : SCR_ScenarioFrameworkActionBase
{
	[Attribute("", UIWidgets.EditBox, "Name of the helicopter entity in the World Editor. Must carry an EEF_HelicopterInsertionComponent.")]
	protected string m_sHelicopterEntityName;

	override void OnActivate(IEntity object)
	{
		if (!CanActivate())
			return;

		if (m_sHelicopterEntityName.IsEmpty())
		{
			Print("[EEF] ActionStartHelicopterInsertion: Helicopter Entity Name is empty.", LogLevel.ERROR);
			return;
		}

		IEntity helicopter = GetGame().GetWorld().FindEntityByName(m_sHelicopterEntityName);
		if (!helicopter)
		{
			Print(string.Format("[EEF] ActionStartHelicopterInsertion: Helicopter entity '%1' not found in world.", m_sHelicopterEntityName), LogLevel.ERROR);
			return;
		}

		EEF_HelicopterInsertionComponent insertion = EEF_HelicopterInsertionComponent.Cast(
			helicopter.FindComponent(EEF_HelicopterInsertionComponent)
		);

		if (!insertion)
		{
			Print(string.Format("[EEF] ActionStartHelicopterInsertion: Entity '%1' has no EEF_HelicopterInsertionComponent.", m_sHelicopterEntityName), LogLevel.ERROR);
			return;
		}

		insertion.StartInsertion();
	}
}
