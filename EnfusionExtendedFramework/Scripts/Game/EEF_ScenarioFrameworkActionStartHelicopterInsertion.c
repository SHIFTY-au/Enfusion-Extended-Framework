// ============================================================
// EEF_ScenarioFrameworkActionStartHelicopterInsertion.c
// Enfusion Extended Framework
//
// A Scenario Framework Action that triggers a helicopter
// insertion sequence.
//
// Add this to the "Finished Actions" or "Entity Left Actions"
// list on a Plugin Trigger in the World Editor. Set the
// Spawn Point Entity Name to match the name of the spawn-point
// entity that carries both EEF_HelicopterControlComponent and
// EEF_HelicopterInsertionComponent.
// ============================================================

[BaseContainerProps(), SCR_ContainerActionTitle()]
class EEF_ScenarioFrameworkActionStartHelicopterInsertion : SCR_ScenarioFrameworkActionBase
{
    [Attribute("", UIWidgets.EditBox, "Name of the spawn-point entity in the World Editor. Must carry EEF_HelicopterInsertionComponent and EEF_HelicopterControlComponent.")]
    protected string m_sSpawnPointEntityName;

    override void OnActivate(IEntity object)
    {
        if (!CanActivate())
            return;

        if (m_sSpawnPointEntityName.IsEmpty())
        {
            Print("[EEF] ActionStartHelicopterInsertion: Spawn Point Entity Name is empty.", LogLevel.ERROR);
            return;
        }

        IEntity spawnPoint = GetGame().GetWorld().FindEntityByName(m_sSpawnPointEntityName);
        if (!spawnPoint)
        {
            Print(string.Format("[EEF] ActionStartHelicopterInsertion: Spawn point entity '%1' not found in world.", m_sSpawnPointEntityName), LogLevel.ERROR);
            return;
        }

        EEF_HelicopterInsertionComponent insertion = EEF_HelicopterInsertionComponent.Cast(
            spawnPoint.FindComponent(EEF_HelicopterInsertionComponent)
        );

        if (!insertion)
        {
            Print(string.Format("[EEF] ActionStartHelicopterInsertion: Entity '%1' has no EEF_HelicopterInsertionComponent.", m_sSpawnPointEntityName), LogLevel.ERROR);
            return;
        }

        insertion.StartInsertion();
    }
}
