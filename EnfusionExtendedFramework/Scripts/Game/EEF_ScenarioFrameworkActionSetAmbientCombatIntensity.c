// ============================================================
// EEF_ScenarioFrameworkActionSetAmbientCombatIntensity.c
// Enfusion Extended Framework
//
// A Scenario Framework Action that changes the base intensity of
// a running Ambient Combat instance - e.g. ramp the battle up when
// an assault starts, or down to wind it off. Running engagements
// finish naturally; new ones are started to match the new level.
// ============================================================

[BaseContainerProps(), SCR_ContainerActionTitle()]
class EEF_ScenarioFrameworkActionSetAmbientCombatIntensity : SCR_ScenarioFrameworkActionBase
{
	[Attribute("", UIWidgets.EditBox, "Name of the Ambient Combat anchor entity in the World Editor. Must carry an EEF_AmbientCombatComponent.")]
	protected string m_sAnchorEntityName;

	[Attribute("0.5", UIWidgets.Slider, "New base intensity. 0 = silent, 1 = full battle.", "0 1 0.05")]
	protected float m_fIntensity;

	override void OnActivate(IEntity object)
	{
		if (!CanActivate())
			return;

		EEF_AmbientCombatComponent battle = EEF_AmbientCombatActionHelper.Resolve(m_sAnchorEntityName, "ActionSetAmbientCombatIntensity");
		if (battle)
			battle.SetIntensity(m_fIntensity);
	}
}
