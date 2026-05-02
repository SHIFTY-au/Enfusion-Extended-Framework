// ============================================================
// EEF_HelicopterInsertionComponent.c
// Enfusion Extended Framework
//
// Troop-management layer for helicopter insertion. Attach this
// component alongside EEF_HelicopterControlComponent on the
// same spawn-point entity in the World Editor.
//
// Responsibilities:
//   - Spawns a troop group prefab and seats members in the
//     helicopter's CARGO compartments once the helicopter is
//     ready (via OnHelicopterSpawned callback).
//   - Troops that exceed available cargo slots are deleted
//     silently (logged at debug level).
//   - Calls StartFlight() on the control component once troops
//     are seated.
//   - Disembarks troops one per tick when OnLanded fires
//     (staggered for visual cleanliness).
//   - Hands the disembarked group off to EEF_GroupReceiverComponent
//     on the LZ entity when disembark is complete.
//
// Zero flight logic lives here — all flight behaviour is
// delegated to EEF_HelicopterControlComponent.
//
// Attach to:   the same spawn-point entity as EEF_HelicopterControlComponent.
// Activated by: EEF_ScenarioFrameworkActionStartHelicopterInsertion.
// Runs on:     SERVER (authority) only.
// ============================================================

[ComponentEditorProps(category: "EEF/Helicopter", description: "EEF Helicopter Insertion - attach alongside EEF_HelicopterControlComponent. Spawns a troop group, seats them, and hands off to a receiver after disembark. Activated via EEF_ScenarioFrameworkActionStartHelicopterInsertion.")]
class EEF_HelicopterInsertionComponentClass : ScriptComponentClass {}

class EEF_HelicopterInsertionComponent : ScriptComponent
{
    // --------------------------------------------------------
    // CONFIGURATION
    // --------------------------------------------------------

    [Attribute("", UIWidgets.ResourcePickerThumbnail, "AI group prefab to spawn and seat in the helicopter's CARGO compartments.", "et")]
    protected ResourceName m_sTroopGroupPrefab;

    [Attribute("", UIWidgets.EditBox, "Name of the LZ entity in the World Editor. EEF_GroupReceiverComponent on this entity receives the disembarked group.")]
    protected string m_sLZEntityName;

    [Attribute("0", UIWidgets.CheckBox, "Print state transitions and key events to the server log.")]
    protected bool m_bDebugLog;

    // --------------------------------------------------------
    // RUNTIME STATE
    // --------------------------------------------------------

    protected EEF_HelicopterControlComponent m_ControlComponent;
    protected SCR_AIGroup m_TroopGroup;
    protected IEntity m_HeliEntity;

    protected const float DISEMBARK_TICK_INTERVAL = 0.5; //! Seconds between each staggered eject.

    // --------------------------------------------------------
    // INITIALISATION
    // --------------------------------------------------------

    override void OnPostInit(IEntity owner)
    {
        super.OnPostInit(owner);

        if (!Replication.IsServer())
            return;

        m_ControlComponent = EEF_HelicopterControlComponent.Cast(
            owner.FindComponent(EEF_HelicopterControlComponent)
        );

        if (!m_ControlComponent)
        {
            Print("[EEF HelicopterInsertion] ERROR: EEF_HelicopterControlComponent not found on same entity. Insertion disabled.", LogLevel.ERROR);
            return;
        }

        m_ControlComponent.GetOnHelicopterSpawned().Insert(OnHelicopterSpawned);
        m_ControlComponent.GetOnLanded().Insert(OnLanded);

        DebugLog("Initialised. Call StartInsertion() to begin.");
    }

    // --------------------------------------------------------
    // PUBLIC API
    // --------------------------------------------------------

    //! Begin the insertion sequence. Triggers helicopter spawn on the control component.
    //! The rest of the sequence is event-driven: OnHelicopterSpawned -> seat troops ->
    //! StartFlight -> OnLanded -> disembark -> handoff.
    void StartInsertion()
    {
        if (!Replication.IsServer())
            return;

        if (!m_ControlComponent)
        {
            Print("[EEF HelicopterInsertion] ERROR: Cannot start - EEF_HelicopterControlComponent missing.", LogLevel.ERROR);
            return;
        }

        if (m_sTroopGroupPrefab.IsEmpty())
        {
            Print("[EEF HelicopterInsertion] ERROR: Cannot start - no troop group prefab configured.", LogLevel.ERROR);
            return;
        }

        if (m_sLZEntityName.IsEmpty())
        {
            Print("[EEF HelicopterInsertion] ERROR: Cannot start - no LZ entity name configured.", LogLevel.ERROR);
            return;
        }

        DebugLog("StartInsertion - triggering helicopter spawn.");
        m_ControlComponent.SpawnHelicopter();
    }

    // --------------------------------------------------------
    // EVENT CALLBACKS (from EEF_HelicopterControlComponent)
    // --------------------------------------------------------

    //! Fired by control component when the helicopter entity is ready (no parameters).
    protected void OnHelicopterSpawned()
    {
        m_HeliEntity = m_ControlComponent.GetHelicopterEntity();
        if (!m_HeliEntity)
        {
            Print("[EEF HelicopterInsertion] ERROR: OnHelicopterSpawned fired but GetHelicopterEntity() returned null.", LogLevel.ERROR);
            return;
        }
        DebugLog("OnHelicopterSpawned - spawning troop group.");

        m_TroopGroup = SpawnTroopGroup(m_HeliEntity);
        if (!m_TroopGroup)
        {
            Print("[EEF HelicopterInsertion] ERROR: Failed to spawn troop group. Aborting insertion.", LogLevel.ERROR);
            return;
        }

        // SCR_AIGroup spawns members across multiple frames. Wait for initialisation before seating.
        if (m_TroopGroup.IsInitializing())
        {
            m_TroopGroup.GetOnAllDelayedEntitySpawned().Insert(OnTroopGroupReady);
            DebugLog("Troop group initialising - waiting for OnAllDelayedEntitySpawned.");
            return;
        }

        // Synchronous spawn path (rare).
        SeatAndStartFlight();
    }

    //! Fires when a delayed-spawn troop group finishes initialising.
    protected void OnTroopGroupReady(SCR_AIGroup group)
    {
        if (group != m_TroopGroup)
        {
            DebugLog("OnTroopGroupReady fired for stale group, ignoring.");
            return;
        }

        DebugLog("Troop group ready - seating and starting flight.");
        SeatAndStartFlight();
    }

    //! Fired by control component when the helicopter reaches the LZ.
    protected void OnLanded()
    {
        DebugLog("OnLanded - beginning staggered disembark.");
        GetGame().GetCallqueue().CallLater(DisembarkTick, DISEMBARK_TICK_INTERVAL * 1000, false);
    }

    // --------------------------------------------------------
    // SEATING
    // --------------------------------------------------------

    protected void SeatAndStartFlight()
    {
        SeatTroops();

        // The LZ entity is the flight destination. Set it as the control component's only
        // waypoint so StartFlight() has a target and will start the engine.
        IEntity lzEntity = GetGame().GetWorld().FindEntityByName(m_sLZEntityName);
        if (!lzEntity)
        {
            Print(string.Format("[EEF HelicopterInsertion] ERROR: LZ entity '%1' not found. Cannot start flight.", m_sLZEntityName), LogLevel.ERROR);
            return;
        }

        m_ControlComponent.ClearWaypoints();
        m_ControlComponent.AddWaypoint(lzEntity.GetOrigin());

        m_ControlComponent.StartFlight();
    }

    protected SCR_AIGroup SpawnTroopGroup(IEntity heli)
    {
        Resource res = Resource.Load(m_sTroopGroupPrefab);
        if (!res || !res.IsValid())
        {
            Print("[EEF HelicopterInsertion] ERROR: Could not load troop group prefab: " + m_sTroopGroupPrefab, LogLevel.ERROR);
            return null;
        }

        EntitySpawnParams spawnParams = new EntitySpawnParams();
        spawnParams.TransformMode = ETransformMode.WORLD;
        Math3D.MatrixIdentity4(spawnParams.Transform);
        spawnParams.Transform[3] = heli.GetOrigin();

        IEntity spawned = GetGame().SpawnEntityPrefab(res, GetGame().GetWorld(), spawnParams);
        SCR_AIGroup group = SCR_AIGroup.Cast(spawned);
        if (!group)
        {
            Print("[EEF HelicopterInsertion] ERROR: Spawned troop prefab is not an SCR_AIGroup.", LogLevel.ERROR);
            return null;
        }

        DebugLog("Troop group spawned at helicopter origin.");
        return group;
    }

    protected void SeatTroops()
    {
        if (!m_HeliEntity || !m_TroopGroup)
            return;

        SCR_BaseCompartmentManagerComponent compMgr = SCR_BaseCompartmentManagerComponent.Cast(
            m_HeliEntity.FindComponent(SCR_BaseCompartmentManagerComponent)
        );
        if (!compMgr)
        {
            Print("[EEF HelicopterInsertion] ERROR: Helicopter has no SCR_BaseCompartmentManagerComponent.", LogLevel.ERROR);
            return;
        }

        array<BaseCompartmentSlot> freeSlots = {};
        compMgr.GetFreeCompartmentsOfType(freeSlots, ECompartmentType.CARGO);

        array<AIAgent> agents = {};
        m_TroopGroup.GetAgents(agents);

        int seated = 0;
        array<AIAgent> overflow = {};

        foreach (AIAgent agent : agents)
        {
            if (!agent)
                continue;

            IEntity character = agent.GetControlledEntity();
            if (!character)
                continue;

            if (seated >= freeSlots.Count())
            {
                overflow.Insert(agent);
                continue;
            }

            SCR_CompartmentAccessComponent access = SCR_CompartmentAccessComponent.Cast(
                character.FindComponent(SCR_CompartmentAccessComponent)
            );
            if (!access)
                continue;

            access.MoveInVehicle(m_HeliEntity, ECompartmentType.CARGO);
            seated++;
        }

        DebugLog(string.Format("Seated %1 troop(s). Overflow: %2.", seated, overflow.Count()));

        foreach (AIAgent agent : overflow)
        {
            if (!agent)
                continue;
            IEntity character = agent.GetControlledEntity();
            if (character)
                SCR_EntityHelper.DeleteEntityAndChildren(character);
            DebugLog("Deleted overflow troop (no cargo slot available).");
        }
    }

    // --------------------------------------------------------
    // DISEMBARK
    // --------------------------------------------------------

    protected void DisembarkTick()
    {
        if (!m_HeliEntity)
        {
            HandoffGroup();
            return;
        }

        SCR_BaseCompartmentManagerComponent compMgr = SCR_BaseCompartmentManagerComponent.Cast(
            m_HeliEntity.FindComponent(SCR_BaseCompartmentManagerComponent)
        );
        if (!compMgr)
        {
            HandoffGroup();
            return;
        }

        array<IEntity> occupants = {};
        compMgr.GetOccupantsOfType(occupants, ECompartmentType.CARGO);

        if (occupants.IsEmpty())
        {
            DebugLog("Disembark complete.");
            HandoffGroup();
            return;
        }

        // Eject one trooper per tick for staggered visual disembark.
        array<BaseCompartmentSlot> cargoSlots = {};
        compMgr.GetCompartmentsOfType(cargoSlots, ECompartmentType.CARGO);
        foreach (BaseCompartmentSlot slot : cargoSlots)
        {
            if (!slot || !slot.GetOccupant())
                continue;
            bool ejected;
            slot.EjectOccupant(true, false, ejected, false);
            DebugLog(string.Format("Ejected one trooper. %1 remaining.", occupants.Count() - 1));
            break;
        }

        GetGame().GetCallqueue().CallLater(DisembarkTick, DISEMBARK_TICK_INTERVAL * 1000, false);
    }

    // --------------------------------------------------------
    // GROUP HANDOFF
    // --------------------------------------------------------

    protected void HandoffGroup()
    {
        if (!m_TroopGroup)
            return;

        IEntity lzEntity = GetGame().GetWorld().FindEntityByName(m_sLZEntityName);
        if (!lzEntity)
        {
            Print(string.Format("[EEF HelicopterInsertion] WARNING: LZ entity '%1' not found at handoff time. Group orphaned.", m_sLZEntityName), LogLevel.WARNING);
            return;
        }

        EEF_GroupReceiverComponent receiver = EEF_GroupReceiverComponent.Cast(
            lzEntity.FindComponent(EEF_GroupReceiverComponent)
        );
        if (!receiver)
        {
            Print(string.Format("[EEF HelicopterInsertion] WARNING: LZ entity '%1' has no EEF_GroupReceiverComponent. Group orphaned.", m_sLZEntityName), LogLevel.WARNING);
            return;
        }

        DebugLog(string.Format("Handing off group to receiver on '%1'.", m_sLZEntityName));
        receiver.OnGroupReceived(m_TroopGroup);
    }

    // --------------------------------------------------------
    // HELPERS
    // --------------------------------------------------------

    protected void DebugLog(string message)
    {
        if (m_bDebugLog)
            Print("[EEF HelicopterInsertion] " + message);
    }
}
