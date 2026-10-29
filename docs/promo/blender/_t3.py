import sys, time, math
sys.path.insert(0, "/home/aibeto/code/Rhine-Music-Database/docs/promo/blender")
import pvlib as L
import bpy

L.purge(); L.world(strength=0.0)
sc = L.render_setup(res=(640,360), samples=16, frame_end=1)
sc.view_settings.view_transform = "AgX"
sc.view_settings.exposure = 0.0
L.compositor_bloom(1.4, 6.0, 0.35)

m_paper = L.mat("PAPER", L.srgb("#E7E7E1"), rough=0.62, spec=0.25)
m_moss  = L.mat("MOSS",  L.srgb("#777B60"), rough=0.55, spec=0.3)
m_mint  = L.mat("MINT",  L.srgb("#8FE3BB"), emit=L.srgb("#8FE3BB"), estr=3.0)
m_org   = L.mat("ORG",   L.srgb("#E8622A"), emit=L.srgb("#E8622A"), estr=2.0)
m_ink   = L.mat("INK",   L.srgb("#141613"), rough=0.42, spec=0.15)

C = "CAL"
L.prim('cube', C, "pbox", loc=(-0.09,0.0,0.075), scale=(0.15,0.15,0.15), material=m_paper)
L.prim('cube', C, "ibox", loc=(0.06,0.05,0.035), scale=(0.07,0.07,0.07), material=m_ink)
L.prim('cyl',  C, "disc", loc=(0.05,-0.06,0.02), r=0.045, d=0.004, material=L.mat_vinyl("VIN", grooves=90.0))
L.prim('cube', C, "mline", loc=(-0.02,-0.10,0.06), scale=(0.14,0.0016,0.0016), material=m_mint)
L.prim('cube', C, "osq", loc=(0.13,-0.02,0.055), scale=(0.012,0.012,0.012), material=m_org)
L.prim('plane', C, "floor", loc=(0,0,0), size=1.6, material=m_moss)

cam = L.cam("CAM_CAL","CAM", loc=(-0.06,-0.42,0.22), target=(0.0,0.0,0.05), lens=42)
sc.camera = cam
for p in (15.0, 35.0, 80.0):
    for ob in list(bpy.data.objects):
        if ob.type == 'LIGHT': bpy.data.objects.remove(ob, do_unlink=True)
    L.spot("key", "CAL", loc=(-0.45,-0.40,0.75), target=(0,0,0.05), size=24, power=p, blend=0.25)
    t=time.time(); sc.render.filepath = "/tmp/cal3_%03d.png" % int(p)
    bpy.ops.render.render(write_still=True)
    print("P", p, "S", round(time.time()-t,2))
