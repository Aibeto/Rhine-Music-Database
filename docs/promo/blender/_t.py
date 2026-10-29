import sys, time, os
sys.path.insert(0, "/home/aibeto/code/Rhine-Music-Database/docs/promo/blender")
import pvlib as L
import bpy, math

L.purge()
L.world()
sc = L.render_setup(res=(1280,720), samples=24, frame_end=1)
L.compositor_bloom(1.0, 8.0, 0.45)

C = "SET1"
m_vin = L.mat_vinyl("VINYL")
m_dust = L.mat("DUST", (1,1,1), rough=0.4, emit=(1,1,1), estr=1.2)
m_beam = L.mat_beam("BEAM", (0.8,0.85,0.9), strength=3.0)

d = L.prim('cyl', C, "disc", loc=(0,0,0), r=0.15, d=0.003, material=m_vin)
L.smooth(d)
L.bevel(d, 0.0015, 2)
for i,(x,y,z) in enumerate([(-0.02,-0.05,0.02),(0.03,0.01,-0.01),(-0.06,0.04,0.05)]):
    L.prim('sphere', C, "dust%d"%i, loc=(x,y,z+0.03), r=0.0018, material=m_dust)

b = L.prim('cube', C, "beam", loc=(0.10,0,0.35), scale=(0.012,0.012,0.7), material=m_beam)
b.rotation_euler = (math.radians(22), 0, math.radians(-12))

L.area("key", C, loc=(0.35,-0.25,0.55), target=(0,0,0), size=0.6, power=90.0)
L.spot("s", C, loc=(0.6,0.3,0.9), target=(0,0,0), size=18, power=260.0)

cam = L.cam("CAM01","CAM", loc=(0.09,-0.20,0.055), target=(0.02,0.0,0.005), lens=100, dof=0.22, fstop=2.8)
sc.camera = cam
sc.frame_set(1)
t=time.time()
sc.render.filepath = "/tmp/t1.png"
bpy.ops.render.render(write_still=True)
print("RENDER1_S", round(time.time()-t,2))
print("exists", os.path.exists("/tmp/t1.png"), os.path.getsize("/tmp/t1.png") if os.path.exists("/tmp/t1.png") else 0)
print("view_transform", sc.view_settings.view_transform)
