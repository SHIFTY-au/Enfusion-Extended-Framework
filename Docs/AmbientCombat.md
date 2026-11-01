# EEF Ambient Combat - Workbench Setup

Distant, synced battle audio driven by mission-maker placed points. Scripts live in
`EnfusionExtendedFramework/Scripts/Game/EEF_AmbientCombat*.c` and
`EEF_ScenarioFrameworkAction*AmbientCombat*.c`.

> **Status: scaffold, not yet compiled or tested in Workbench.** See the
> "Assumptions to verify" list in the header of `EEF_AmbientCombatComponent.c`.

## How it works (one paragraph)

The server picks a Side A point and a Side B point, treats them as an engagement, and
every few seconds broadcasts one small RPC per burst/barrage. Every client expands that
into individual shots against the replicated clock and plays them through a local
emitter entity, so all players hear the same thing at the same time. Distance
attenuation, speed-of-sound delay and reverb are left to the engine.

## 1. Build the emitter prefabs (one per ACP)

An emitter is just a local entity with a `SoundComponent`. A sound component plays the
**first** sound whose name matches the event, so ACPs that share an event name
(e.g. `SOUND_SHOT` in both the AK-74 and M16A2 ACPs) must live on **separate** emitters.

For each ACP below:

1. Resource Browser -> new Prefab, inherit `GenericEntity`. Save as e.g.
   `Prefabs/EEF/AmbientCombat/Emitter_AK74.et`.
2. Add a `SoundComponent`. Add the ACP to its **File Names** array.
3. **Do not** add an `RplComponent` - the emitter must be local-only.

| Emitter | ACP | Suggested role |
|---|---|---|
| Emitter_AK74 | `{2C3081D1EE023D68}Sounds/Weapons/Rifles/AK-74/Weapons_Rifles_AK-74_Shot.acp` | Side A small arms |
| Emitter_M16A2 | `{DB92C647E05B2512}Sounds/Weapons/Rifles/M16A2/Weapons_Rifles_M16A2_Shot.acp` | Side B small arms |
| Emitter_KPVT | `{66E74C5A23D6C8A0}Sounds/Weapons/HeavyWeapons/KPVT/Weapons_HeavyWeapons_KPVT_Shot.acp` | Side A MG |
| Emitter_M2 | `{EE8892C4180091FD}Sounds/Weapons/HeavyWeapons/M2/Weapons_HeavyWeapons_M2_Shot.acp` | Side B MG |
| Emitter_RPG7 | `{E35D01748E28E4CD}Sounds/Environment/Misc/Shots/Environment_Launchers_RPG7_Shot.acp` | Side A launcher |
| Emitter_GrenadeExpl | `{A30C069F58BF6D05}Sounds/Environment/Misc/Explosions/Environment_Grenade_Explosion.acp` | Small explosions |
| Emitter_TNT_M | `{B9347A5498317F65}Sounds/Particles/Logistics/Explosion/TNT/Particles_Explosions_TNT_Medium.acp` | Shell impacts (TNT_Large / TNT_Small exist too) |
| Emitter_Fuel_L | `{9D94BEBA8AA8A997}Sounds/Particles/Logistics/Explosion/Fuel/Particles_Explosions_Fuel_Large.acp` | Big secondary blasts (Fuel_Medium / Fuel_Small exist too) |

`Particles_Explosions_Battery.acp` is probably too small to be useful here.

## 2. Find the event names

The event name is the **name of a Sound node** inside the ACP. You don't need to
understand the node graph - open the ACP in the Audio Editor and read the names of the
Sound nodes (the nodes labelled "Sound"). Weapon shot ACPs are expected to use
`SOUND_SHOT` (the engine ties this event to speed-of-sound simulation), but this is
unconfirmed. Explosion ACPs are unknown - note whatever they call theirs.

## 3. Check what signals the sounds read

Vanilla sounds may read **signals** (ammo type, interior, surface...) that a bare emitter
doesn't have. To see them while a real weapon fires:

1. In game, hold **Ctrl + Win + Alt** to open the debug menu -> **Sounds** -> **Signals
   debug** (by entity).
2. Select the firing weapon/character and note the signal names the shot ACP uses.
3. If a sound plays silent/odd on the emitter, the missing signal is the likely cause.
   Signals can be set on a SoundComponent before the event with `SetSignalValueStr()`;
   tell me which ones and I'll add a per-class signal list.

## 4. Fallback: your own small ACP built from vanilla samples

If vanilla events misbehave on a bare emitter, or you want one ACP with unique event names
and a long audible range, build it once (all vanilla samples are usable from a mod):

1. Audio Editor -> File -> New Project -> `EEF_AmbientCombat.acp`.
2. Drag vanilla `.wav` samples from the Resource Browser into a **Bank** node (hold
   **Shift** while dragging to *replace* its samples instead of adding). Raise
   *volume variance* (~2 dB) and *pitch variance* (~3 semitones) on the Bank for free
   variation.
3. Add a **Shader** node, connect the Bank to it, then right-click the Shader to enable
   extra inputs and add:
   - **Speciality** = 1 (fully 3D),
   - **Amplitude** with a long *outer range* (e.g. 3000 m) and a sensible slope,
   - **Frequency** with distance attenuation enabled (air-absorption "light").
4. Add a **Sound** node and connect the Shader. **Its name is the event name**, e.g.
   `SOUND_EEF_AC_RIFLE_A`.
5. Connect the Sound node to the **Mixer** output (Final Mix -> pick the matching
   weapons/explosions channel).
6. Any wav you bring in yourself must be right-clicked -> **Register and import** first.
7. Put this ACP on a single emitter prefab (as in section 1) and point every sound
   class at that emitter with its own event name.

## 5. Place it in the world

1. Create an anchor entity. Give it an **RplComponent** (required) and an
   **EEF_AmbientCombatComponent**.
2. Place marker entities (any entity type - only position is used) and name them
   `BattleA_1`, `BattleA_2`... for Side A and `BattleB_1`... for Side B (prefixes are
   configurable; numbering starts at 1, gaps are fine).
3. On the component add **Sound Classes** - one per row: emitter prefab, event name, type,
   side, weight, and shot count / interval / scatter. Suggested starting values:

| Class | Type | Side | Count | Interval (ms) | Scatter (m) | Gap (s) |
|---|---|---|---|---|---|---|
| AK-74 | SMALL_ARMS | A | 3-12 | 90-160 | 4 | 4-12 |
| M16A2 | SMALL_ARMS | B | 3-12 | 90-160 | 4 | 4-12 |
| KPVT | MACHINE_GUN | A | 10-30 | 70-110 | 4 | 10-25 |
| M2 | MACHINE_GUN | B | 10-30 | 90-140 | 4 | 10-25 |
| RPG-7 | LAUNCHER | A | 1-1 | 1000-1000 | 4 | 30-90 |
| Grenade | EXPLOSION | - | 1-1 | 500-500 | 15 | - |
| TNT medium (barrage) | EXPLOSION | - | 4-10 | 400-1500 | 60 | - |

4. Enable **Auto Activate** or use a Plugin Trigger with
   `EEF_ScenarioFrameworkActionStartAmbientCombat` (and `Stop...` /
   `SetAmbientCombatIntensity`), setting *Anchor Entity Name* to the anchor's name.

## 6. Test checklist

- [ ] Scripts compile (see assumptions list in the component header).
- [ ] Debug on: server log shows markers discovered and engagements starting.
- [ ] Single player / listen server: shots are heard, **once** (not doubled).
- [ ] Two clients on a dedicated server: both hear the same burst at the same moment.
- [ ] Late-join: no errors, sound picks up with the next burst.
- [ ] Distant (1-2 km) sources are audible and shaped by the engine's attenuation.
- [ ] `SetAmbientCombatIntensity` 0 -> 1 audibly changes density; `Stop` goes quiet.
