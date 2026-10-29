import bpy, sys
sc = bpy.context.scene
cards = [o for o in bpy.data.objects if o.name.startswith("C_")]
cams = [o for o in bpy.data.objects if o.type == "CAMERA"]
mk = list(sc.timeline_markers)
meshes = set(o.data.name for o in cards)
print("OBJ", len(bpy.data.objects), "CARDS", len(cards), "CAMS", len(cams), "MARKERS", len(mk))
print("MESH_DATA", len(meshes), "MATERIALS", len(bpy.data.materials))
print("FRAMES", sc.frame_start, sc.frame_end, "FPS", sc.render.fps, "RES", sc.render.resolution_x, sc.render.resolution_y, "ENGINE", sc.render.engine)
if cards:
    o = cards[0]
    bb = [o.matrix_world @ v.co for v in o.data.vertices[:1]]
    print("CARD0 loc", tuple(round(x, 2) for x in o.location), "verts", len(o.data.vertices), "polys", len(o.data.polygons))
    print("CARD0 bbox", [round(x, 2) for x in o.dimensions])
print("MISSING", [o.name for o in bpy.data.objects if o.type == "MESH" and o.data is None])
print("ACTIONS", len(bpy.data.actions))
print("CAM_ACTIVE", sc.camera.name if sc.camera else None)
sc.frame_set(300)
print("AT300_CAM", sc.camera.name if sc.camera else None)
bpy.ops.render.render(write_still=False)
print("RENDER_OK")
