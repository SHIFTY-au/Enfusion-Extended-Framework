// ============================================================
// EEF_GroupReceiverComponent.c
// Enfusion Extended Framework
//
// Shared base class for any module that should receive an
// AI group handed off from another EEF module — e.g.
// helicopter insertion, vehicle transport, assault wave.
//
// Downstream modules extend this class instead of
// ScriptComponent and override OnGroupReceived to react when
// an upstream module hands them a spawned, populated group.
//
// The handoff contract is one-shot: the upstream module
// finishes its job (boarded, flown, disembarked, etc.) and
// then calls receiver.OnGroupReceived(group) on the SERVER.
// The receiver is responsible for everything that happens
// to the group from that point on — waypoints, speed,
// formation, cleanup.
// ============================================================

[ComponentEditorProps(category: "EEF/Shared", description: "Base class for EEF modules that receive AI groups handed off from other modules. Override OnGroupReceived to react.")]
class EEF_GroupReceiverComponentClass : ScriptComponentClass {}

class EEF_GroupReceiverComponent : ScriptComponent
{
	//------------------------------------------------------------------------------------------------
	//! Called on SERVER when an upstream module has finished delivering an AI group to this receiver.
	//! Override in downstream modules to take ownership of the group and start downstream behaviour.
	//!
	//! Senders MUST guarantee:
	//!  - This is called on the server only.
	//!  - The group entity is valid and populated when the call is made.
	//!  - The group is not currently owned by another module (no double-handoff).
	//!
	//! Default behaviour is a no-op so that subclasses are free to override only when they care.
	//!
	//! \param group The AI group being handed off.
	void OnGroupReceived(SCR_AIGroup group)
	{
		// Default no-op. Override in subclass.
	}
}
