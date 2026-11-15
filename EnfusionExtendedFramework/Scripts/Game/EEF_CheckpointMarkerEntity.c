// ============================================================
// EEF_CheckpointMarkerEntity.c
// Enfusion Extended Framework
//
// Marker entities for the checkpoint prefab. EEF_CheckpointComponent finds them by TYPE among its own
// children, so no entity names are needed and several checkpoints can coexist in one world:
//
//   EEF_CheckpointSpawnMarker    - where traffic spawns. Add as many as you like (one per feeder road);
//                                  drag another Spawn marker prefab onto the checkpoint in the hierarchy.
//   EEF_CheckpointDespawnMarker  - where traffic leaves the map. One is used.
//
// Place each one ON a road. They do nothing at runtime. In the World Editor (and ONLY there - never while
// playing and never in a packaged mod) each draws a tall beacon, a ground ring and a text label so it can
// be found from far away: green = Spawn, red = Despawn.
// ============================================================

[EntityEditorProps(category: "EEF/Checkpoint", description: "Base for checkpoint markers. Use the Spawn or Despawn marker prefab.")]
class EEF_CheckpointMarkerEntityClass : GenericEntityClass
{
}

class EEF_CheckpointMarkerEntity : GenericEntity
{
	void EEF_CheckpointMarkerEntity(IEntitySource src, IEntity parent)
	{
	}

	//! Text shown above the marker in the World Editor.
	protected string GetMarkerLabel()
	{
		return "MARKER";
	}

	//! Beacon colour (opaque) and ball colour (translucent) in the World Editor.
	protected int GetMarkerColor()
	{
		return ARGB(255, 255, 255, 255);
	}

	protected int GetMarkerColorSoft()
	{
		return ARGB(120, 255, 255, 255);
	}

#ifdef WORKBENCH
	protected ref DebugTextWorldSpace m_EditorLabel;

	//------------------------------------------------------------------------------------------------
	override void _WB_AfterWorldUpdate(float timeSlice)
	{
		// Editor only: nothing while playing.
		if (GetGame() && GetGame().InPlayMode())
		{
			m_EditorLabel = null;
			return;
		}

		vector pos = GetOrigin();
		int color = GetMarkerColor();
		int soft = GetMarkerColorSoft();
		ShapeFlags softFlags = ShapeFlags.ONCE | ShapeFlags.TRANSP | ShapeFlags.NOOUTLINE;
		ShapeFlags lineFlags = ShapeFlags.ONCE | ShapeFlags.NOZBUFFER;
		vector ground = pos + Vector(0, 0.3, 0);
		vector top = pos + Vector(0, 40.0, 0);

		// Beacon: a 40 m pole with a ball on top, plus two rings on the ground.
		Shape.Create(ShapeType.LINE, color, lineFlags, pos, top);
		Shape.CreateSphere(soft, softFlags, top, 3.0);
		Shape.CreateSphere(soft, softFlags, ground, 1.0);
		CreateCircle(ground, vector.Up, 4.0, color, 36, lineFlags);
		CreateCircle(ground, vector.Up, 2.0, color, 24, lineFlags);

		// Label above the beacon.
		WorldEditor worldEditor = Workbench.GetModule(WorldEditor);
		if (!worldEditor)
			return;

		WorldEditorAPI api = worldEditor.GetApi();
		if (!api || !api.GetWorld())
			return;

		m_EditorLabel = DebugTextWorldSpace.Create(api.GetWorld(), GetMarkerLabel(), DebugTextFlags.CENTER | DebugTextFlags.FACE_CAMERA | DebugTextFlags.IN_WORLD, pos[0], pos[1] + 46.0, pos[2], 5.0, color, 0xAA000000);
	}
#endif
}

[EntityEditorProps(category: "EEF/Checkpoint", description: "Where checkpoint traffic spawns. Place on a road. Add more for more feeder roads.", color: "0 255 0 255")]
class EEF_CheckpointSpawnMarkerClass : EEF_CheckpointMarkerEntityClass
{
}

class EEF_CheckpointSpawnMarker : EEF_CheckpointMarkerEntity
{
	override protected string GetMarkerLabel()
	{
		return "SPAWN";
	}

	override protected int GetMarkerColor()
	{
		return ARGB(255, 40, 220, 60);
	}

	override protected int GetMarkerColorSoft()
	{
		return ARGB(120, 40, 220, 60);
	}
}

[EntityEditorProps(category: "EEF/Checkpoint", description: "Where checkpoint traffic leaves the map. Place on the road past the roadblock.", color: "255 0 0 255")]
class EEF_CheckpointDespawnMarkerClass : EEF_CheckpointMarkerEntityClass
{
}

class EEF_CheckpointDespawnMarker : EEF_CheckpointMarkerEntity
{
	override protected string GetMarkerLabel()
	{
		return "DESPAWN";
	}

	override protected int GetMarkerColor()
	{
		return ARGB(255, 230, 50, 50);
	}

	override protected int GetMarkerColorSoft()
	{
		return ARGB(120, 230, 50, 50);
	}
}
