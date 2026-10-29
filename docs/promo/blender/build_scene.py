# -*- coding: utf-8 -*-
"""Build the full 34-shot previz scene, switch cameras with markers, save the .blend."""
import bpy, sys, os, time, math
BASE = "/home/aibeto/code/Rhine-Music-Database/docs/promo/blender"
sys.path.insert(0, BASE)
from mathutils import Vector
import pvlib as L
import stages as ST

FPS = 12
SPACING = 100.0

# (key, t0, t1, builder, label)
SHOTS = [
    ('01', 0.00, 1.61, ST.s01, '光柱与尘埃'),
    ('02', 1.61, 2.68, ST.s02, '巨大圆盘一角 · 反光'),
    ('03', 2.68, 4.29, ST.s03, '同心刻痕 · 盘转90'),
    ('04', 4.29, 5.89, ST.s04, '六棱晶体析出'),
    ('05', 5.89, 7.50, ST.s05, '唱片堆成的柱体升起'),
    ('06a', 7.50, 8.04, ST.s06a, '针尖落到盘面'),
    ('06b', 8.04, 8.57, ST.s06b, '刻痕峡谷 · 光掠谷底'),
    ('06c', 8.57, 9.11, ST.s06c, '光扫过盘面反出网格'),
    ('07', 9.11, 10.71, ST.s07, '克拉尼驻波 · 沙粒成花瓣'),
    ('08', 10.71, 12.86, ST.s08, '坠入最内圈 · 环即隧道'),
    ('09', 12.86, 15.54, ST.s09, '白光洗出档案架'),
    ('10', 15.54, 17.14, ST.s10, '掠过盒侧'),
    ('11', 17.14, 18.75, ST.s11, '盒面化成光幕 · 四世界'),
    ('12', 18.75, 20.36, ST.s12, '盒边拉长成数据柱'),
    ('13', 20.36, 23.04, ST.s13, '档案库 = 巨大唱盘同心圆'),
    ('14', 23.04, 24.11, ST.s14, '侧立的唱片 · 薄如纸'),
    ('15', 24.11, 25.71, ST.s15, '唱片倒下 · 多米诺'),
    ('16', 25.71, 27.32, ST.s16, '尘浪 · 粒子云'),
    ('17', 27.32, 30.00, ST.s17, '粒子云里的眼睛'),
    ('18', 30.00, 32.14, ST.s18, '瞳孔隧道 · 频谱柱'),
    ('19', 32.14, 36.43, ST.s19, '闪切十刀 19a-19j'),
    ('20', 36.43, 39.64, ST.s20, '黑房间 · 悬浮唱片'),
    ('21', 39.64, 42.32, ST.s21, '甩出光带 · 展开成网格'),
    ('22', 42.32, 45.00, ST.s22, '细线搭建 UI'),
    ('23', 45.00, 47.68, ST.s23, '穿过全息 UI'),
    ('24', 47.68, 50.36, ST.s24, 'UI 与建筑混合 · 播放键的洞'),
    ('25', 50.36, 53.04, ST.s25, '光自己去按那个洞'),
    ('26', 53.04, 56.25, ST.s26, '白光溢出 · 峰值'),
    ('27', 56.25, 58.93, ST.s27, '回到光柱 · 尘埃静止'),
    ('28', 58.93, 61.61, ST.s28, '光柱里悬着唱片'),
    ('29', 61.61, 64.29, ST.s29, '后退 · 唱片缩成点'),
    ('30', 64.29, 66.96, ST.s30, '点展开成横线 · 刻度尺第7格'),
    ('31', 66.96, 69.64, ST.s31, '字标浮出'),
    ('32', 69.64, 70.71, ST.s32, '线消失 · 硬断'),
]


def fcurves_of(ob):
    ad = ob.animation_data
    if not ad or not ad.action:
        return []
    act = ad.action
    try:
        if len(act.fcurves):
            return list(act.fcurves)
    except Exception:
        pass
    out = []
    try:
        for lay in act.layers:
            for st in lay.strips:
                for slot in act.slots:
                    cb = st.channelbag(slot)
                    if cb:
                        out.extend(cb.fcurves)
    except Exception:
        pass
    return out


def set_interp(ob, kind):
    for fc in fcurves_of(ob):
        for kp in fc.keyframe_points:
            kp.interpolation = kind
            if kind == 'BEZIER':
                kp.handle_left_type = 'AUTO_CLAMPED'
                kp.handle_right_type = 'AUTO_CLAMPED'


def build():
    t0 = time.time()
    L.purge()
    ST.MAT.clear()
    L.world(strength=0.0)
    sc = L.render_setup(res=(1280, 720), samples=16, fps=FPS, frame_end=int(round(70.714 * FPS)))
    sc.view_settings.view_transform = 'AgX'
    sc.view_settings.look = 'AgX - Medium High Contrast' if 'AgX - Medium High Contrast' in [
        i.name for i in sc.view_settings.bl_rna.properties['look'].enum_items] else 'None'
    L.compositor_bloom(1.2, 10.0, 0.22)

    markers = []
    cams = []
    for i, (key, ta, tb, fn, label) in enumerate(SHOTS):
        off = Vector((i * SPACING, 0, 0))
        C = "S" + key.replace('a', 'A').replace('b', 'B').replace('c', 'C')
        fa = int(round(ta * FPS)) + 1
        fb = int(round(tb * FPS))
        res = fn(off, C, fa, fb)
        segs = res if isinstance(res, list) else [(res, fa, fb)]
        for j, (cam, cfa, cfb) in enumerate(segs):
            markers.append((cfa, cam, key, label))
            cams.append((cam, key, cfa, cfb, label))

    # camera markers
    sc.timeline_markers.clear()
    for f, cam, key, label in markers:
        m = sc.timeline_markers.new("%s %s" % (key, label), frame=f)
        m.camera = cam
    sc.camera = markers[0][1]

    # fade all cameras' keys to smooth by default
    for cam, key, cfa, cfb, label in cams:
        set_interp(cam, 'BEZIER')

    sc.frame_start = 1
    sc.frame_end = int(round(70.714 * FPS))
    sc.frame_set(1)
    sc.render.filepath = "//render/f_"
    sc.render.image_settings.file_format = 'PNG'
    try:
        sc.render.image_settings.color_mode = 'RGB'
        sc.render.image_settings.compression = 15
    except Exception:
        pass
    print("BUILD_S", round(time.time() - t0, 1))
    print("OBJECTS", len(bpy.data.objects), "CAMS", len(bpy.data.cameras), "MARKERS", len(markers))
    print("FRAMES", sc.frame_start, sc.frame_end)
    return sc


def main():
    sc = build()
    out = os.path.join(BASE, "rhine-pv-previz.blend")
    bpy.ops.wm.save_as_mainfile(filepath=out)
    print("SAVED", out, os.path.getsize(out))


main()
