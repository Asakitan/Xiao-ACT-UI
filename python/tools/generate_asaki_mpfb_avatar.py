"""Generate the default Asaki avatar using Blender character add-ons.

Requires Blender with MPFB enabled. The output intentionally keeps a complete
FBX while writing a curated sidecar preview mesh for SAO's lightweight overlay.
"""

from __future__ import annotations

import json
import math
from pathlib import Path
from typing import Any

import bpy
from mathutils import Vector


ROOT = Path(r"E:\VC\SAO-UI")
ASSET_DIR = ROOT / "plugins" / "script_stickwoman_csharp" / "assets"
FBX_PATH = ASSET_DIR / "stickwoman_fixture.fbx"
BLEND_PATH = ASSET_DIR / "asaki_mpfb_source.blend"
SIDECAR_PATH = ASSET_DIR / "stickwoman_fixture.model3d.json"
RENDER_PATH = ROOT / "sao_auto" / "temp" / "asaki_mpfb_avatar.png"

CANONICAL_TO_MPFB = {
    "root": "root",
    "hips": "spine05",
    "spine": "spine04",
    "chest": "spine03",
    "neck": "neck01",
    "head": "head",
    "left_shoulder": "clavicle.L",
    "left_arm": "upperarm01.L",
    "left_forearm": "lowerarm01.L",
    "left_hand": "wrist.L",
    "right_shoulder": "clavicle.R",
    "right_arm": "upperarm01.R",
    "right_forearm": "lowerarm01.R",
    "right_hand": "wrist.R",
    "left_leg": "upperleg01.L",
    "left_knee": "lowerleg01.L",
    "left_foot": "foot.L",
    "right_leg": "upperleg01.R",
    "right_knee": "lowerleg01.R",
    "right_foot": "foot.R",
}


def ensure_dirs() -> None:
    ASSET_DIR.mkdir(parents=True, exist_ok=True)
    RENDER_PATH.parent.mkdir(parents=True, exist_ok=True)


def clear_scene() -> None:
    if bpy.ops.object.mode_set.poll():
        bpy.ops.object.mode_set(mode="OBJECT")
    bpy.ops.object.select_all(action="SELECT")
    bpy.ops.object.delete()


def mat(name: str, color: tuple[float, float, float, float], roughness: float = 0.7) -> bpy.types.Material:
    material = bpy.data.materials.new(name)
    material.use_nodes = True
    material.diffuse_color = color
    bsdf = material.node_tree.nodes.get("Principled BSDF")
    if bsdf:
        bsdf.inputs["Base Color"].default_value = color
        bsdf.inputs["Roughness"].default_value = roughness
        bsdf.inputs["Metallic"].default_value = 0.0
        for inp in bsdf.inputs:
            if inp.name == "Subsurface Weight":
                inp.default_value = 0.18 if "skin" in name else 0.0
    return material


def shade(obj: bpy.types.Object) -> bpy.types.Object:
    bpy.context.view_layer.objects.active = obj
    obj.select_set(True)
    try:
        bpy.ops.object.shade_smooth()
    except Exception:
        pass
    obj.select_set(False)
    return obj


def add_sphere(name: str, loc: tuple[float, float, float], scale: tuple[float, float, float],
               material: bpy.types.Material, segments: int = 32, rings: int = 16) -> bpy.types.Object:
    bpy.ops.mesh.primitive_uv_sphere_add(segments=segments, ring_count=rings, radius=1.0, location=loc)
    obj = bpy.context.object
    obj.name = name
    obj.data.name = name + "_Mesh"
    obj.scale = scale
    obj.data.materials.append(material)
    return shade(obj)


def add_cylinder_between(name: str, start: tuple[float, float, float], end: tuple[float, float, float],
                         radius: float, material: bpy.types.Material, vertices: int = 32) -> bpy.types.Object:
    s = Vector(start)
    e = Vector(end)
    direction = e - s
    bpy.ops.mesh.primitive_cylinder_add(vertices=vertices, radius=radius, depth=direction.length, location=(s + e) * 0.5)
    obj = bpy.context.object
    obj.name = name
    obj.data.name = name + "_Mesh"
    if direction.length > 0:
        obj.rotation_euler = direction.to_track_quat("Z", "Y").to_euler()
    obj.data.materials.append(material)
    return shade(obj)


def add_cone(name: str, loc: tuple[float, float, float], radius1: float, radius2: float, depth: float,
             material: bpy.types.Material, vertices: int = 64, scale: tuple[float, float, float] = (1, 1, 1)) -> bpy.types.Object:
    bpy.ops.mesh.primitive_cone_add(vertices=vertices, radius1=radius1, radius2=radius2, depth=depth, location=loc)
    obj = bpy.context.object
    obj.name = name
    obj.data.name = name + "_Mesh"
    obj.scale = scale
    obj.data.materials.append(material)
    return shade(obj)


def add_tube(name: str, points: list[tuple[float, float, float]], material: bpy.types.Material,
             bevel: float = 0.02) -> bpy.types.Object:
    curve = bpy.data.curves.new(name + "_Curve", type="CURVE")
    curve.dimensions = "3D"
    curve.resolution_u = 4
    curve.bevel_depth = bevel
    curve.bevel_resolution = 4
    spline = curve.splines.new("POLY")
    spline.points.add(len(points) - 1)
    for point, co in zip(spline.points, points):
        point.co = (co[0], co[1], co[2], 1.0)
    obj = bpy.data.objects.new(name, curve)
    bpy.context.collection.objects.link(obj)
    obj.data.materials.append(material)
    bpy.context.view_layer.objects.active = obj
    obj.select_set(True)
    bpy.ops.object.convert(target="MESH")
    obj = bpy.context.object
    obj.name = name
    return shade(obj)


def add_leaf(name: str, center: tuple[float, float, float], width: float, height: float,
             material: bpy.types.Material, angle: float = 0.0, y_curve: float = 0.035) -> bpy.types.Object:
    steps = 24
    verts: list[tuple[float, float, float]] = []
    faces: list[tuple[int, int, int, int]] = []
    ca, sa = math.cos(angle), math.sin(angle)
    for i in range(steps + 1):
        t = i / steps
        half = width * math.sin(math.pi * t) * 0.5
        z = (t - 0.5) * height
        y = y_curve * math.sin(math.pi * t)
        for side in (-1.0, 1.0):
            x = side * half
            verts.append((center[0] + x * ca - y * sa, center[1] + x * sa + y * ca, center[2] + z))
    for i in range(steps):
        faces.append((2 * i, 2 * i + 1, 2 * i + 3, 2 * i + 2))
    mesh = bpy.data.meshes.new(name + "_Mesh")
    mesh.from_pydata(verts, [], faces)
    mesh.update()
    obj = bpy.data.objects.new(name, mesh)
    bpy.context.collection.objects.link(obj)
    obj.data.materials.append(material)
    return shade(obj)


def add_ear(name: str, side: float, material: bpy.types.Material, inner: bool = False) -> bpy.types.Object:
    verts: list[tuple[float, float, float]] = []
    faces: list[tuple[int, int, int, int]] = []
    rings = 18
    sides = 32
    for ring in range(rings):
        t = ring / (rings - 1)
        z = 1.68 + 0.72 * t
        x_center = side * (0.105 + 0.20 * t)
        y_center = -0.03 - 0.035 * t
        radius = 0.10 * math.sin(math.pi * max(0.03, min(0.97, t))) + 0.018
        rx = radius * (0.62 if inner else 0.94)
        ry = radius * (0.14 if inner else 0.30)
        if inner:
            y_center -= 0.025
            rx *= 0.66
        for i in range(sides):
            a = math.tau * i / sides
            verts.append((x_center + side * math.cos(a) * rx, y_center + math.sin(a) * ry, z))
    for ring in range(rings - 1):
        for i in range(sides):
            faces.append((ring * sides + i, ring * sides + (i + 1) % sides,
                          (ring + 1) * sides + (i + 1) % sides, (ring + 1) * sides + i))
    mesh = bpy.data.meshes.new(name + "_Mesh")
    mesh.from_pydata(verts, [], faces)
    mesh.update()
    obj = bpy.data.objects.new(name, mesh)
    bpy.context.collection.objects.link(obj)
    obj.data.materials.append(material)
    return shade(obj)


def attach_to_bone(obj: bpy.types.Object, armature: bpy.types.Object, bone: str) -> None:
    if obj.type != "MESH":
        return
    group = obj.vertex_groups.new(name=bone)
    group.add([v.index for v in obj.data.vertices], 1.0, "ADD")
    mod = obj.modifiers.new("Armature", "ARMATURE")
    mod.object = armature
    obj.parent = armature
    obj["asaki_bone"] = bone


def convert_point(point: Vector | tuple[float, float, float]) -> list[float]:
    x, y, z = float(point[0]), float(point[1]), float(point[2])
    return [x, z, -y]


def build_sidecar_preview(objects: list[bpy.types.Object]) -> tuple[list[list[float]], list[list[int]], list[list[dict[str, Any]]]]:
    vertices: list[list[float]] = []
    faces: list[list[int]] = []
    skin: list[list[dict[str, Any]]] = []

    def add_vertex(point: tuple[float, float, float], bone: str, semantic: str) -> int:
        vertices.append([float(point[0]), float(point[1]), float(point[2])])
        skin.append([{"joint": semantic, "weight": 1.0}, {"joint": bone, "weight": 1.0}])
        return len(vertices) - 1

    def add_ellipsoid(
        center: tuple[float, float, float],
        scale: tuple[float, float, float],
        bone: str,
        semantic: str,
        *,
        segments: int = 28,
        rings: int = 14,
    ) -> None:
        start = len(vertices)
        for r in range(rings + 1):
            v = r / rings
            theta = math.pi * v
            y = math.cos(theta)
            radius = math.sin(theta)
            for s in range(segments):
                u = math.tau * s / segments
                add_vertex((
                    center[0] + math.cos(u) * radius * scale[0],
                    center[1] + y * scale[1],
                    center[2] + math.sin(u) * radius * scale[2],
                ), bone, semantic)
        for r in range(rings):
            for s in range(segments):
                a = start + r * segments + s
                b = start + r * segments + (s + 1) % segments
                c = start + (r + 1) * segments + (s + 1) % segments
                d = start + (r + 1) * segments + s
                faces.append([a, b, c, d])

    def add_cone_proxy(
        center: tuple[float, float, float],
        height: float,
        radius_bottom: float,
        radius_top: float,
        bone: str,
        semantic: str,
        *,
        segments: int = 36,
        rings: int = 8,
        z_scale: float = 0.62,
    ) -> None:
        start = len(vertices)
        for r in range(rings + 1):
            t = r / rings
            radius = radius_bottom + (radius_top - radius_bottom) * t
            y = center[1] - height * 0.5 + height * t
            for s in range(segments):
                u = math.tau * s / segments
                add_vertex((center[0] + math.cos(u) * radius, y, center[2] + math.sin(u) * radius * z_scale), bone, semantic)
        for r in range(rings):
            for s in range(segments):
                a = start + r * segments + s
                b = start + r * segments + (s + 1) % segments
                c = start + (r + 1) * segments + (s + 1) % segments
                d = start + (r + 1) * segments + s
                faces.append([a, b, c, d])

    def add_capsule(
        start_point: tuple[float, float, float],
        end_point: tuple[float, float, float],
        radius: float,
        bone: str,
        semantic: str,
        *,
        segments: int = 18,
    ) -> None:
        sx, sy, sz = start_point
        ex, ey, ez = end_point
        dx, dy, dz = ex - sx, ey - sy, ez - sz
        length = math.sqrt(dx * dx + dy * dy + dz * dz) or 1.0
        ux, uy, uz = dx / length, dy / length, dz / length
        side = (1.0, 0.0, 0.0) if abs(ux) < 0.9 else (0.0, 0.0, 1.0)
        vx = uy * side[2] - uz * side[1]
        vy = uz * side[0] - ux * side[2]
        vz = ux * side[1] - uy * side[0]
        vlen = math.sqrt(vx * vx + vy * vy + vz * vz) or 1.0
        vx, vy, vz = vx / vlen, vy / vlen, vz / vlen
        wx = uy * vz - uz * vy
        wy = uz * vx - ux * vz
        wz = ux * vy - uy * vx
        start = len(vertices)
        for r, t in enumerate((0.0, 0.35, 0.7, 1.0)):
            cx, cy, cz = sx + dx * t, sy + dy * t, sz + dz * t
            for s in range(segments):
                u = math.tau * s / segments
                add_vertex((
                    cx + (math.cos(u) * vx + math.sin(u) * wx) * radius,
                    cy + (math.cos(u) * vy + math.sin(u) * wy) * radius,
                    cz + (math.cos(u) * vz + math.sin(u) * wz) * radius,
                ), bone, semantic)
        for r in range(3):
            for s in range(segments):
                a = start + r * segments + s
                b = start + r * segments + (s + 1) % segments
                c = start + (r + 1) * segments + (s + 1) % segments
                d = start + (r + 1) * segments + s
                faces.append([a, b, c, d])
        add_ellipsoid(start_point, (radius, radius, radius), bone, semantic, segments=segments, rings=6)
        add_ellipsoid(end_point, (radius, radius, radius), bone, semantic, segments=segments, rings=6)

    # SAO preview proxy: complete stylized model in Y-up coordinates. The FBX
    # remains the full MPFB mesh; this proxy is only for fast overlay drawing.
    add_ellipsoid((0.0, 0.70, 0.0), (0.22, 0.17, 0.13), "hips", "dress_olive", segments=28, rings=10)
    add_cone_proxy((0.0, 0.98, 0.0), 0.76, 0.34, 0.19, "spine", "dress_olive")
    add_ellipsoid((0.0, 1.25, -0.06), (0.20, 0.12, 0.08), "chest", "outfit_dark", segments=28, rings=10)
    add_ellipsoid((0.0, 1.58, -0.02), (0.22, 0.25, 0.17), "head", "skin", segments=32, rings=16)
    add_ellipsoid((0.0, 1.70, 0.03), (0.25, 0.22, 0.14), "head", "hair_white", segments=32, rings=12)
    add_ellipsoid((0.0, 1.16, 0.10), (0.34, 0.70, 0.09), "head", "hair_white", segments=30, rings=16)
    for side in (-1.0, 1.0):
        add_capsule((0.13 * side, 1.78, -0.02), (0.36 * side, 2.42, -0.05), 0.055, "head", "rabbit_ear", segments=18)
        add_capsule((0.13 * side, 1.78, -0.04), (0.34 * side, 2.34, -0.08), 0.030, "head", "rabbit_ear_inner", segments=14)
        add_ellipsoid((0.075 * side, 1.57, -0.18), (0.050, 0.025, 0.010), "head", "eye_green", segments=18, rings=6)
        add_ellipsoid((0.245 * side, 1.72, -0.02), (0.085, 0.055, 0.030), "head", "outfit_dark", segments=18, rings=8)
        add_capsule((0.21 * side, 1.22, 0.0), (0.43 * side, 0.92, 0.0), 0.055, "left_forearm" if side > 0 else "right_forearm", "sleeve", segments=18)
        add_capsule((0.10 * side, 0.67, 0.0), (0.15 * side, 0.22, -0.02), 0.070, "left_leg" if side > 0 else "right_leg", "skin", segments=18)
        add_ellipsoid((0.16 * side, 0.04, -0.08), (0.10, 0.035, 0.15), "left_foot" if side > 0 else "right_foot", "skin", segments=18, rings=6)
    for x in (-0.18, -0.09, 0.0, 0.09, 0.18):
        add_capsule((x, 1.67, -0.13), (x * 0.65, 1.36, -0.17), 0.022, "head", "hair_white", segments=12)
    return vertices[:4096], faces[:8192], skin[:4096]

    priority = {
        "eye": 0,
        "rabbit": 1,
        "ear": 1,
        "hair": 2,
        "dress": 3,
        "sleeve": 3,
        "choker": 3,
        "strap": 3,
        "human": 5,
    }

    def rank(obj: bpy.types.Object) -> int:
        text = obj.name.lower()
        return min((value for key, value in priority.items() if key in text), default=4)

    def influences_for(obj: bpy.types.Object, point: list[float]) -> list[dict[str, Any]]:
        text = obj.name.lower()
        y = point[1]
        x = point[0]
        bone = "spine"
        semantic = "skin"
        if "eye" in text:
            bone, semantic = "head", "eye_green"
        elif "rabbit" in text or "ear" in text:
            bone, semantic = "head", "rabbit_ear_inner" if "inner" in text else "rabbit_ear"
        elif "hair" in text or "bang" in text or "lock" in text:
            bone, semantic = "head", "hair_white"
        elif "dress" in text or "skirt" in text:
            bone, semantic = "spine", "dress_olive"
        elif "sleeve" in text or "shawl" in text:
            bone, semantic = "left_forearm" if x > 0 else "right_forearm", "sleeve"
        elif "strap" in text or "choker" in text or "bow" in text:
            bone, semantic = "head" if "bow" in text else "chest", "outfit_dark"
        elif y > 1.38:
            bone, semantic = "head", "skin"
        elif y > 1.08:
            bone, semantic = "chest", "skin"
        elif y > 0.72:
            bone, semantic = "spine", "dress_olive"
        elif x > 0.08:
            bone, semantic = "left_leg", "skin"
        elif x < -0.08:
            bone, semantic = "right_leg", "skin"
        return [{"joint": bone, "weight": 1.0}, {"joint": semantic, "weight": 1.0}]

    for obj in sorted((o for o in objects if o.type == "MESH"), key=rank):
        if len(vertices) >= 2048:
            break
        mesh = obj.data
        remaining = max(0, 2048 - len(vertices))
        if remaining <= 0:
            break
        source_count = max(1, len(mesh.vertices))
        stride = max(1, math.ceil(source_count / max(48, min(remaining, 512 if rank(obj) >= 5 else remaining))))
        local_to_preview: dict[int, int] = {}
        for vert in mesh.vertices:
            if vert.index % stride != 0:
                continue
            if len(vertices) >= 2048:
                break
            world = obj.matrix_world @ vert.co
            point = convert_point(world)
            local_to_preview[vert.index] = len(vertices)
            vertices.append(point)
            skin.append(influences_for(obj, point))
        for poly in mesh.polygons:
            mapped = [local_to_preview.get(index) for index in poly.vertices]
            if all(index is not None for index in mapped) and len(mapped) >= 3 and len(faces) < 4096:
                faces.append([int(index) for index in mapped if index is not None])
    return vertices, faces, skin


def rest_positions(armature: bpy.types.Object) -> dict[str, list[float]]:
    out: dict[str, list[float]] = {}
    for canonical, actual in CANONICAL_TO_MPFB.items():
        bone = armature.data.bones.get(actual)
        if bone:
            out[actual] = convert_point(bone.head_local)
            out[canonical] = convert_point(bone.head_local)
    return out


def write_sidecar(armature: bpy.types.Object, preview_objects: list[bpy.types.Object]) -> None:
    vertices, faces, skin = build_sidecar_preview(preview_objects)
    sidecar = {
        "profile": "mpfb_humanoid_asaki",
        "rest_pose": "soft_a_pose",
        "unit_scale": 1.0,
        "up_axis": "y",
        "skeleton": {
            "bone_map": CANONICAL_TO_MPFB,
            "bones": list(CANONICAL_TO_MPFB.values()),
            "rest_positions": rest_positions(armature),
        },
        "clips": ["idle", "wave", "walk", "jump"],
        "materials": {
            "profile": "asaki_anime",
            "style": "mtoon_soft",
        },
        "physics": {
            "enabled": True,
            "profile": "vrm_spring_like",
            "secondary_motion": {
                "enabled": True,
                "chains": [
                    {"name": "hair", "joints": ["hair_white", "silver_hair", "hair_secondary"], "axis": [0.05, 0.02, 1.0], "gravity": [0.0, -1.0, 0.0], "amplitude": 0.03, "frequency": 0.72, "damping": 0.22, "gravity_strength": 0.18, "wave": 0.85},
                    {"name": "rabbit_ears", "joints": ["rabbit_ear", "rabbit_ear_inner", "bunny_ear"], "axis": [0.10, 0.0, 1.0], "gravity": [0.0, -1.0, 0.0], "amplitude": 0.018, "frequency": 0.55, "damping": 0.35, "gravity_strength": 0.08, "wave": 0.5},
                    {"name": "dress_sleeves", "joints": ["dress_olive", "skirt_olive", "sleeve", "shawl"], "axis": [0.08, 0.0, 0.18], "gravity": [0.0, -1.0, 0.0], "amplitude": 0.016, "frequency": 0.82, "damping": 0.30, "gravity_strength": 0.22, "wave": 0.95},
                ],
            },
        },
        "secondary_motion": {
            "enabled": True,
            "schema": "sao.secondary_motion.v1",
            "chains": [
                {"name": "hair", "joints": ["hair_white", "silver_hair", "hair_secondary"], "axis": [0.05, 0.02, 1.0], "gravity": [0.0, -1.0, 0.0], "amplitude": 0.03, "frequency": 0.72, "damping": 0.22, "gravity_strength": 0.18, "wave": 0.85},
                {"name": "rabbit_ears", "joints": ["rabbit_ear", "rabbit_ear_inner", "bunny_ear"], "axis": [0.10, 0.0, 1.0], "gravity": [0.0, -1.0, 0.0], "amplitude": 0.018, "frequency": 0.55, "damping": 0.35, "gravity_strength": 0.08, "wave": 0.5},
                {"name": "dress_sleeves", "joints": ["dress_olive", "skirt_olive", "sleeve", "shawl"], "axis": [0.08, 0.0, 0.18], "gravity": [0.0, -1.0, 0.0], "amplitude": 0.016, "frequency": 0.82, "damping": 0.30, "gravity_strength": 0.22, "wave": 0.95},
            ],
        },
        "mesh": {
            "preview": {
                "source": "sidecar_mpfb_curated",
                "vertices": vertices,
                "faces": faces,
                "skin": skin,
            }
        },
    }
    SIDECAR_PATH.write_text(json.dumps(sidecar, ensure_ascii=False, indent=2), encoding="utf-8")


def create_animation(armature: bpy.types.Object) -> None:
    scene = bpy.context.scene
    scene.frame_start = 1
    scene.frame_end = 96
    bpy.context.view_layer.objects.active = armature
    bpy.ops.object.mode_set(mode="POSE")
    action = bpy.data.actions.new("wave")
    armature.animation_data_create()
    armature.animation_data.action = action
    animated = {
        "lowerarm01.L": (0.0, 0.0, 0.0),
        "upperarm01.L": (0.0, 0.0, 0.0),
        "head": (0.0, 0.0, 0.0),
    }
    for frame in (1, 24, 48, 72, 96):
        scene.frame_set(frame)
        t = (frame - 1) / 95.0
        for name in animated:
            bone = armature.pose.bones.get(name)
            if not bone:
                continue
            bone.rotation_mode = "XYZ"
            if name == "lowerarm01.L":
                bone.rotation_euler = (math.radians(-8), math.radians(18 + 16 * math.sin(t * math.tau * 2)), math.radians(8))
            elif name == "upperarm01.L":
                bone.rotation_euler = (math.radians(0), math.radians(0), math.radians(12))
            elif name == "head":
                bone.rotation_euler = (0.0, 0.0, math.radians(2.5 * math.sin(t * math.tau)))
            bone.keyframe_insert("rotation_euler", frame=frame)
    bpy.ops.object.mode_set(mode="OBJECT")


def build_character() -> dict[str, Any]:
    ensure_dirs()
    clear_scene()
    scene = bpy.context.scene
    scene.unit_settings.system = "METRIC"
    scene.render.engine = "CYCLES"
    scene.cycles.samples = 64
    scene.view_settings.view_transform = "Filmic"

    materials = {
        "skin": mat("MAT-asaki-skin-soft", (0.96, 0.78, 0.70, 1.0), 0.55),
        "hair": mat("MAT-asaki-white-hair", (0.90, 0.88, 0.82, 1.0), 0.62),
        "shadow": mat("MAT-asaki-lavender-hair-shadow", (0.62, 0.60, 0.70, 1.0), 0.72),
        "eye": mat("MAT-asaki-mint-eyes", (0.05, 0.92, 0.75, 1.0), 0.35),
        "ear": mat("MAT-asaki-rabbit-ear-outer", (0.74, 0.62, 0.56, 1.0), 0.66),
        "inner": mat("MAT-asaki-rabbit-ear-inner", (0.97, 0.68, 0.72, 1.0), 0.66),
        "dress": mat("MAT-asaki-olive-dress", (0.43, 0.42, 0.31, 1.0), 0.78),
        "dark": mat("MAT-asaki-dark-cloth", (0.06, 0.05, 0.08, 1.0), 0.74),
        "sleeve": mat("MAT-asaki-ivory-sleeve", (0.88, 0.85, 0.78, 1.0), 0.78),
        "stocking": mat("MAT-asaki-soft-stocking", (0.94, 0.66, 0.64, 1.0), 0.72),
    }

    bpy.ops.mpfb.create_human()
    human = bpy.data.objects.get("Human")
    if human is None:
        raise RuntimeError("MPFB did not create Human mesh")
    bpy.context.view_layer.objects.active = human
    human.select_set(True)
    bpy.ops.mpfb.add_standard_rig()
    armature = bpy.data.objects.get("Human.rig")
    if armature is None:
        raise RuntimeError("MPFB did not create standard rig")
    human.name = "GEO-asaki-body"
    human.data.name = "GEO-asaki-body_Mesh"
    human.data.materials.append(materials["skin"])
    armature.name = "ARM-asaki-standard"
    human.parent = armature

    accessory: list[bpy.types.Object] = []
    accessory.append(add_cone("GEO-asaki-dress-main", (0, -0.01, 1.02), 0.34, 0.20, 0.72, materials["dress"], scale=(0.85, 0.62, 1.0)))
    accessory.append(add_sphere("GEO-asaki-bodice-dark", (0, -0.075, 1.30), (0.20, 0.055, 0.12), materials["dark"], 48, 16))
    accessory.append(add_tube("GEO-asaki-choker", [(-0.10, -0.07, 1.48), (0, -0.10, 1.47), (0.10, -0.07, 1.48)], materials["dark"], 0.010))
    accessory.append(add_tube("GEO-asaki-left-strap", [(-0.14, -0.11, 1.31), (-0.03, -0.13, 1.12), (0.04, -0.10, 0.90)], materials["dark"], 0.009))
    accessory.append(add_tube("GEO-asaki-right-strap", [(0.14, -0.11, 1.31), (0.03, -0.13, 1.12), (-0.04, -0.10, 0.90)], materials["dark"], 0.009))
    accessory.append(add_sphere("GEO-asaki-hair-cap", (0, 0.03, 1.68), (0.19, 0.14, 0.17), materials["hair"], 64, 24))
    accessory.append(add_sphere("GEO-asaki-back-hair-volume", (0, 0.10, 1.18), (0.28, 0.08, 0.55), materials["shadow"], 48, 20))
    for i, x in enumerate((-0.14, -0.07, 0.0, 0.07, 0.14)):
        accessory.append(add_leaf(f"GEO-asaki-bang-{i}", (x, -0.16, 1.57 - abs(x) * 0.12), 0.095, 0.34, materials["hair"], angle=x * 1.5))
    for i, x in enumerate((-0.24, -0.13, 0.13, 0.24)):
        accessory.append(add_tube(f"GEO-asaki-long-hair-lock-{i}", [(x, 0.04, 1.53), (x * 1.08, 0.09, 1.02), (x * 1.18, 0.06, 0.38)], materials["hair"], 0.026))
    for side_name, side in (("left", 1.0), ("right", -1.0)):
        accessory.append(add_ear(f"GEO-asaki-{side_name}-rabbit-ear", side, materials["ear"], inner=False))
        accessory.append(add_ear(f"GEO-asaki-{side_name}-rabbit-ear-inner", side, materials["inner"], inner=True))
        accessory.append(add_sphere(f"GEO-asaki-{side_name}-eye", (0.062 * side, -0.165, 1.545), (0.040, 0.007, 0.020), materials["eye"], 32, 10))
        accessory.append(add_sphere(f"GEO-asaki-{side_name}-bow", (0.235 * side, -0.03, 1.71), (0.070, 0.026, 0.052), materials["dark"], 32, 10))
        attach_to_bone(add_cylinder_between(f"GEO-asaki-{side_name}-sleeve", (0.28 * side, -0.03, 1.20), (0.50 * side, -0.02, 0.89), 0.052, materials["sleeve"], 32), armature, "lowerarm01.L" if side > 0 else "lowerarm01.R")
        accessory.append(bpy.context.object)
        accessory.append(add_sphere(f"GEO-asaki-{side_name}-stocking", (0.13 * side, -0.02, 0.36), (0.075, 0.055, 0.25), materials["stocking"], 32, 16))

    for obj in accessory:
        text = obj.name.lower()
        if obj.parent is armature:
            continue
        if "sleeve" in text:
            bone = "lowerarm01.L" if "left" in text else "lowerarm01.R"
        elif "dress" in text or "strap" in text or "bodice" in text:
            bone = "spine03"
        elif "stocking" in text:
            bone = "upperleg01.L" if "left" in text else "upperleg01.R"
        else:
            bone = "head"
        attach_to_bone(obj, armature, bone)

    create_animation(armature)

    bpy.ops.object.light_add(type="AREA", location=(0, -3.0, 3.3))
    light = bpy.context.object
    light.name = "LGT-asaki-softbox"
    light.data.energy = 500
    light.data.size = 3.0
    bpy.ops.object.camera_add(location=(0, -4.2, 1.38), rotation=(math.radians(78), 0, 0))
    camera = bpy.context.object
    camera.name = "CAM-asaki-preview"
    camera.data.lens = 68
    scene.camera = camera

    export_objects = [human, armature] + accessory
    bpy.ops.object.select_all(action="DESELECT")
    for obj in export_objects:
        obj.hide_set(False)
        obj.hide_viewport = False
        obj.hide_render = False
        obj.select_set(True)
    bpy.context.view_layer.objects.active = armature

    write_sidecar(armature, [human] + accessory)
    bpy.ops.wm.save_as_mainfile(filepath=str(BLEND_PATH))
    bpy.ops.export_scene.fbx(
        filepath=str(FBX_PATH),
        use_selection=True,
        object_types={"MESH", "ARMATURE"},
        apply_unit_scale=True,
        apply_scale_options="FBX_SCALE_ALL",
        bake_space_transform=False,
        add_leaf_bones=False,
        primary_bone_axis="Y",
        secondary_bone_axis="X",
        use_armature_deform_only=True,
        bake_anim=True,
        bake_anim_use_all_bones=True,
        bake_anim_use_nla_strips=False,
        bake_anim_use_all_actions=True,
        bake_anim_force_startend_keying=True,
        bake_anim_step=1.0,
        bake_anim_simplify_factor=0.0,
        mesh_smooth_type="FACE",
        use_mesh_modifiers=True,
        path_mode="AUTO",
        axis_forward="-Z",
        axis_up="Y",
    )
    scene.render.filepath = str(RENDER_PATH)
    scene.render.resolution_x = 1200
    scene.render.resolution_y = 1600
    bpy.ops.render.render(write_still=True)

    mesh_objects = [obj for obj in export_objects if obj.type == "MESH"]
    return {
        "fbx": str(FBX_PATH),
        "blend": str(BLEND_PATH),
        "sidecar": str(SIDECAR_PATH),
        "render": str(RENDER_PATH),
        "mesh_objects": len(mesh_objects),
        "vertices": sum(len(obj.data.vertices) for obj in mesh_objects),
        "faces": sum(len(obj.data.polygons) for obj in mesh_objects),
    }


if __name__ == "__main__":
    print("ASAKI_MPFB_SUMMARY " + json.dumps(build_character(), ensure_ascii=False, sort_keys=True))
