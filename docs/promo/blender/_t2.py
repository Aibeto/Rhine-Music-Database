import sys, time, os, math
sys.path.insert(0, "/home/aibeto/code/Rhine-Music-Database/docs/promo/blender")
import pvlib as L
import bpy

sc = bpy.context.scene
# which view transforms really work
for vt in ["AgX","Filmic","Standard","Khronos PBR Neutral","None"]:
    try:
        sc.view_settings.view_transform = vt
        print("VT_OK", vt)
    except Exception as e:
        print("VT_NO", vt, type(e).__name__)
sc.view_settings.view_transform = "AgX" if True else "Standard"

L.purge(); L.world(strength=0.0)
sc = L.render_setup(res=(640,360), samples=16, frame_end=1)
L.compositor_bloom(1.2, 6.0, 0.4)

m_vin = L.mat_vinyl("VINYL2", grooves=110.0)
nt = m_vin.node_tree
for n in nt.nodes:
    if n.type=='TEX_WAVE':
        n.inputs["Distortion"].default_value = 0.0
        n.inputs["Detail"].default_value = 0.0
m_alu = L.mat("ALU", L.srgb("#9A9A96"), rough=0.34, metal=1.0, aniso=0.3)
m_dust = L.mat("DUST", (1,1,1), rough=0.4, emit=(1,1,1), estr=2.0)

C="SET1"
d = L.prim('cyl', C, "disc", r=0.15, d=0.004, material=m_vin); L.smooth(d)
ring = L.prim('torus', C, "edge", R=0.149, r=0.0016, material=m_alu)
L.prim('cyl', C, "spindle", r=0.0035, d=0.012, material=m_alu)
for i,(x,y,z) in enumerate([(-0.05,-0.09,0.03),(0.06,0.03,0.01),(-0.10,0.05,0.06)]):
    L.prim('sphere', C, "dust%d"%i, loc=(x,y,z), r=0.0016, material=m_dust)

cam = L.cam("CAM01","CAM", loc=(0.055,-0.115,0.035), target=(0.0,-0.01,0.002), lens=100, dof=0.14, fstop=5.6)
sc.camera = cam
sc.frame_set(1)

for p in (2.0, 6.0, 18.0):
    for ob in list(bpy.data.objects):
        if ob.type=='LIGHT': bpy.data.objects.remove(ob, do_unlink=True)
    L.area("key", C, loc=(0.10,-0.16,0.10), target=(0,0,0), size=0.30, power=p)
    L.spot("s", C, loc=(-0.25,0.18,0.24), target=(0,0,0), size=16, power=p*3)
    t=time.time(); sc.render.filepath="/tmp/cal_%04.1f.png"%p
    bpy.ops.render.render(write_still=True)
    print("P",p,"S",round(time.time()-t,2))
