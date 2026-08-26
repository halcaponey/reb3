"""Blender addon: export meshes to Burnout 3 (Xbox) .bgv vehicle geometry.

INSTALL
    Copy this file AND bgv_write.py into the same folder, then
    Edit > Preferences > Add-ons > Install... and pick io_export_bgv.py.
    (Both files must sit side by side; the addon imports the writer from
    its own directory.)

USE
    File > Export > Burnout 3 geometry (.bgv)

    Point "Template .bgv" at a shipped car (e.g. pveh/COMP/Car1.bgv) unless
    you know what you are giving up: the exporter can author GEOMETRY, but
    the paint/texture directory, the convex collision hull, the deformation/
    wheel/panel matrices and several header regions that are still [?] are
    carried over from the template verbatim.  Without one you get a
    structurally valid file with no paint page and only a box collision
    volume.

    ALL byte packing lives in bgv_write.py so it can be validated without
    Blender -- see tools/validate_bgv.py.  Read bgv_write.py's module
    docstring for the recovered format and the per-field evidence.

AXES.  .bgv is GAME space: left-handed, +X = the car's right, +Y = up,
+Z = the NOSE (bgv_write.py cites src/burnout3_panels.h:109 and :336).
Blender is right-handed Z-up, so the basis change always has determinant
-1 and this exporter always reverses triangle winding to compensate.  Set
"Nose axis" to whichever Blender axis your car's nose points down.

PER-OBJECT / PER-MATERIAL KEYS
    Object custom property  b3_slot   int, which part object the mesh
                                      becomes:
                                        -1  embedded intact car only
                                         0  aperture body (default)
                                       1..6 damage panels, PIVOT-LOCAL
                                       7..9 wheel: slow / blur / fast blur
                            (or name the object body / panel0..panel5 /
                             wheel / wheel_slot8 / wheel_slot9)
    Material custom props   b3_mask     int pass mask (default 0x0001)
                            b3_texslot  int 0..4 (default 0)
                            b3_shader   int 0 or 1 (default 0)
                            b3_alpha    float glass tint (default 1.0)
    Material name fallbacks "b3tex<N>" sets the texture slot -- which is
    exactly what tools/extract_bgv.py writes as `usemtl b3tex<slot>`, so an
    extracted car re-imported through OBJ keeps its slots; a name
    containing "glass" defaults to mask 0x0100 / texture slot 2.
"""

bl_info = {
    "name": "Burnout 3 vehicle geometry (.bgv)",
    "author": "Burnout 3 reverse-engineering port",
    "version": (1, 0, 0),
    "blender": (4, 0, 0),
    "location": "File > Export > Burnout 3 geometry (.bgv)",
    "description": "Export mesh objects to the Xbox Burnout 3 .bgv format",
    "category": "Import-Export",
}

import os
import sys

import bpy
from bpy.props import (BoolProperty, EnumProperty, FloatProperty,
                       IntProperty, StringProperty)
from bpy_extras.io_utils import ExportHelper

_HERE = os.path.dirname(os.path.abspath(__file__))
if _HERE not in sys.path:
    sys.path.insert(0, _HERE)
import bgv_write as bgv                                    # noqa: E402

if "bgv_write" in sys.modules:                             # dev convenience
    import importlib
    bgv = importlib.reload(bgv)


# ---------------------------------------------------------------------------
# Object / material -> .bgv keys
# ---------------------------------------------------------------------------
_NAME_SLOTS = (
    ("wheel_slot9", bgv.SLOT_WHEEL_BLUR2),
    ("wheel_slot8", bgv.SLOT_WHEEL_BLUR1),
    ("wheel_slot7", bgv.SLOT_WHEEL_SLOW),
    ("wheel", bgv.SLOT_WHEEL_SLOW),
    ("panel0", 1), ("panel1", 2), ("panel2", 3),
    ("panel3", 4), ("panel4", 5), ("panel5", 6),
    ("shell", bgv.SLOT_APERTURE_BODY),
    ("body", bgv.SLOT_APERTURE_BODY),
    ("intact", -1),
)


def slot_for_object(obj, default_slot):
    v = obj.get("b3_slot")
    if v is not None:
        try:
            iv = int(v)
        except (TypeError, ValueError):
            iv = default_slot
        if -1 <= iv < bgv.NUM_SLOTS:
            return iv
    low = obj.name.lower()
    for key, slot in _NAME_SLOTS:
        if key in low:
            return slot
    return default_slot


def keys_for_material(mat, default_mask, default_tex):
    mask, tex, shader, alpha = default_mask, default_tex, 0, 1.0
    if mat is not None:
        low = mat.name.lower()
        i = low.find("b3tex")
        if i >= 0:
            digits = ""
            for ch in low[i + 5:]:
                if ch.isdigit():
                    digits += ch
                else:
                    break
            if digits:
                tex = int(digits)
        if "glass" in low:
            mask = bgv.MASK_GLASS_A
            if tex == bgv.TEX_PAINT:
                tex = bgv.TEX_GLASS_OK
        for prop, setter in (("b3_mask", "mask"), ("b3_texslot", "tex"),
                             ("b3_shader", "shader")):
            v = mat.get(prop)
            if v is not None:
                iv = int(v)
                if setter == "mask":
                    mask = iv
                elif setter == "tex":
                    tex = iv
                else:
                    shader = iv
        v = mat.get("b3_alpha")
        if v is not None:
            alpha = float(v)
    return mask, tex, shader, alpha


# ---------------------------------------------------------------------------
# Mesh extraction
# ---------------------------------------------------------------------------
def _corner_normals(mesh):
    """Blender 4.1+ exposes split normals as mesh.corner_normals; 4.0 needs
    calc_normals_split() and reads them off the loops."""
    try:
        return [tuple(n.vector) for n in mesh.corner_normals]
    except AttributeError:
        pass
    try:
        mesh.calc_normals_split()
    except AttributeError:
        pass
    return [tuple(l.normal) for l in mesh.loops]


def _mat3_mul(M, v):
    return (M[0][0] * v[0] + M[0][1] * v[1] + M[0][2] * v[2],
            M[1][0] * v[0] + M[1][1] * v[1] + M[1][2] * v[2],
            M[2][0] * v[0] + M[2][1] * v[1] + M[2][2] * v[2])


def collect_groups(context, objects, opts):
    """Turn Blender objects into bgv.MeshGroup, already in .bgv space."""
    B = bgv.axis_matrix(opts["nose"])
    depsgraph = context.evaluated_depsgraph_get()
    groups = []
    stats = {"objects": 0, "tris": 0, "verts": 0}

    for obj in objects:
        if obj.type != "MESH":
            continue
        src = obj.evaluated_get(depsgraph) if opts["apply_modifiers"] else obj
        try:
            mesh = src.to_mesh()
        except RuntimeError:
            continue
        if mesh is None:
            continue
        try:
            mesh.calc_loop_triangles()
            normals = _corner_normals(mesh)
            mw = obj.matrix_world
            nm = mw.inverted_safe().transposed().to_3x3()
            det_mw = mw.to_3x3().determinant()
            # det(B) is -1 for every mode (Blender RH -> .bgv LH); a
            # mirrored object flips again.
            reverse = (bgv.axis_det(B) * det_mw) < 0.0

            uv_data = None
            if mesh.uv_layers:
                layer = (mesh.uv_layers.get(opts["uv_layer"])
                         if opts["uv_layer"] else None) or mesh.uv_layers.active
                if layer is not None:
                    uv_data = layer.data

            slot = slot_for_object(obj, opts["default_slot"])
            buckets = {}
            for tri in mesh.loop_triangles:
                mi = tri.material_index
                bucket = buckets.get(mi)
                if bucket is None:
                    bucket = buckets[mi] = {"verts": [], "tris": [],
                                            "map": {}}
                idx = []
                for li in tri.loops:
                    lo = mesh.loops[li]
                    co = mw @ mesh.vertices[lo.vertex_index].co
                    p = bgv.apply_axis(B, (co.x, co.y, co.z))
                    p = (p[0] * opts["scale"], p[1] * opts["scale"],
                         p[2] * opts["scale"])
                    n = _mat3_mul(nm, normals[li]) if li < len(normals) \
                        else (0.0, 1.0, 0.0)
                    ln = (n[0] ** 2 + n[1] ** 2 + n[2] ** 2) ** 0.5 or 1.0
                    n = bgv.apply_axis(B, (n[0] / ln, n[1] / ln, n[2] / ln))
                    if uv_data is not None:
                        uv = uv_data[li].uv
                        u, v = uv[0], uv[1]
                    else:
                        u = v = 0.0
                    if opts["flip_v"]:
                        v = 1.0 - v
                    key = (round(p[0], 6), round(p[1], 6), round(p[2], 6),
                           round(n[0], 4), round(n[1], 4), round(n[2], 4),
                           round(u, 6), round(v, 6))
                    vi = bucket["map"].get(key)
                    if vi is None:
                        vi = len(bucket["verts"])
                        bucket["map"][key] = vi
                        bucket["verts"].append(
                            (p[0], p[1], p[2], n[0], n[1], n[2], u, v))
                    idx.append(vi)
                a, b, c = idx
                if a == b or b == c or a == c:
                    continue
                bucket["tris"].append((a, c, b) if reverse else (a, b, c))

            mats = list(mesh.materials) if mesh.materials else []
            for mi, bucket in sorted(buckets.items()):
                if not bucket["tris"]:
                    continue
                mat = mats[mi] if mi < len(mats) else None
                mask, tex, shader, alpha = keys_for_material(
                    mat, opts["default_mask"], opts["default_tex"])
                groups.append(bgv.MeshGroup(
                    "%s.%d" % (obj.name, mi), bucket["verts"], bucket["tris"],
                    slot=slot, mask=mask, tex_slot=tex, shader=shader,
                    alpha0=alpha, alpha1=alpha if mask & bgv.MASK_GLASS else 0.0))
                stats["tris"] += len(bucket["tris"])
                stats["verts"] += len(bucket["verts"])
            stats["objects"] += 1
        finally:
            src.to_mesh_clear()
    return groups, stats


# ---------------------------------------------------------------------------
# Operator
# ---------------------------------------------------------------------------
class EXPORT_OT_bgv(bpy.types.Operator, ExportHelper):
    """Export mesh objects as Burnout 3 (Xbox) .bgv vehicle geometry"""

    bl_idname = "export_scene.burnout3_bgv"
    bl_label = "Export Burnout 3 geometry"
    bl_options = {"PRESET"}

    filename_ext = ".bgv"
    filter_glob: StringProperty(default="*.bgv", options={"HIDDEN"})

    use_selection: BoolProperty(
        name="Selected objects only",
        description="Export only the selected mesh objects; off exports the "
                    "whole scene",
        default=True)

    template_path: StringProperty(
        name="Template .bgv",
        description="Donor file supplying everything the exporter cannot "
                    "author: the paint/texture directory, the convex "
                    "collision hull, the deformation / wheel / panel "
                    "matrices, and the header regions that are still "
                    "unrecovered. Strongly recommended",
        subtype="FILE_PATH",
        default="")

    nose_axis: EnumProperty(
        name="Nose axis",
        description="Which Blender axis the car's nose points down. .bgv "
                    "stores GAME space: +X right, +Y up, +Z nose",
        items=[("+Y", "+Y", "Nose along Blender +Y"),
               ("-Y", "-Y", "Nose along Blender -Y"),
               ("+X", "+X", "Nose along Blender +X"),
               ("-X", "-X", "Nose along Blender -X")],
        default="+Y")

    global_scale: FloatProperty(
        name="Scale",
        description="Multiplier applied to every position. .bgv is in "
                    "metres (COMP/Car1 measures 2.46 x 1.73 x 4.15)",
        default=1.0, min=1e-4, max=1e4)

    uv_layer: StringProperty(
        name="UV layer",
        description="UV map to write; empty uses the active one",
        default="")

    flip_v: BoolProperty(
        name="Flip V",
        description="Convert Blender's bottom-left UV origin to the "
                    "top-left origin the game samples with (v=0 is texel "
                    "row 0 -- tools/extract_textures.py)",
        default=True)

    apply_modifiers: BoolProperty(
        name="Apply modifiers",
        description="Export the evaluated mesh",
        default=True)

    lod_count: IntProperty(
        name="LOD sections",
        description="How many LOD sections to write. Retail always ships 4; "
                    "the same geometry is written to each",
        default=4, min=1, max=4)

    default_slot: IntProperty(
        name="Default part slot",
        description="Slot for objects with no b3_slot property and no "
                    "recognised name (0 = aperture body)",
        default=0, min=-1, max=17)

    default_mask: IntProperty(
        name="Default pass mask",
        description="Record +0x18 for materials with no b3_mask "
                    "(1 = body, 2 = unlit body, 0x100 = glass)",
        default=1, min=0, max=0xFFFF)

    default_tex_slot: IntProperty(
        name="Default texture slot",
        description="Record +0x1A: 0 = this car's paint page, "
                    "1 = VehicleUnderside, 2..4 = the shared glass tiers",
        default=0, min=0, max=4)

    update_body_box: BoolProperty(
        name="Rebuild collision box",
        description="Recompute the .bgv +0xE80/+0xE90 body box from the "
                    "exported mesh bounds. The convex hull at +0x1060 is "
                    "NOT regenerated -- it comes from the template",
        default=True)

    def draw(self, context):
        layout = self.layout
        layout.use_property_split = True
        col = layout.column()
        col.prop(self, "use_selection")
        col.prop(self, "template_path")
        col.prop(self, "nose_axis")
        col.prop(self, "global_scale")
        col.prop(self, "apply_modifiers")
        col.prop(self, "lod_count")
        col.separator()
        col.prop(self, "uv_layer")
        col.prop(self, "flip_v")
        col.separator()
        col.prop(self, "default_slot")
        col.prop(self, "default_mask")
        col.prop(self, "default_tex_slot")
        col.prop(self, "update_body_box")

    def execute(self, context):
        objects = (context.selected_objects if self.use_selection
                   else context.scene.objects)
        objects = [o for o in objects if o.type == "MESH"]
        if not objects:
            self.report({"ERROR"}, "no mesh objects to export")
            return {"CANCELLED"}

        template = None
        path = bpy.path.abspath(self.template_path) if self.template_path else ""
        if path:
            try:
                template = bgv.read_bgv_file(path)
            except (OSError, bgv.BgvError) as exc:
                self.report({"ERROR"}, "template: %s" % exc)
                return {"CANCELLED"}

        opts = {
            "nose": self.nose_axis,
            "scale": self.global_scale,
            "uv_layer": self.uv_layer,
            "flip_v": self.flip_v,
            "apply_modifiers": self.apply_modifiers,
            "default_slot": self.default_slot,
            "default_mask": self.default_mask,
            "default_tex": self.default_tex_slot,
        }
        groups, stats = collect_groups(context, objects, opts)
        if not groups:
            self.report({"ERROR"}, "no triangles to export")
            return {"CANCELLED"}

        try:
            model = bgv.build_bgv(groups, template=template,
                                  lod_count=self.lod_count,
                                  body_box=self.update_body_box)
            size = bgv.write_bgv_file(self.filepath, model)
        except bgv.BgvError as exc:
            self.report({"ERROR"}, str(exc))
            return {"CANCELLED"}

        self.report({"INFO"},
                    "%s: %d objects, %d tris, %d verts, %d LODs, %d bytes"
                    % (os.path.basename(self.filepath), stats["objects"],
                       stats["tris"], stats["verts"], self.lod_count, size))
        if template is None:
            self.report({"WARNING"},
                        "no template: the file has no paint texture and only "
                        "a box collision volume")
        return {"FINISHED"}


def menu_func_export(self, context):
    self.layout.operator(EXPORT_OT_bgv.bl_idname,
                         text="Burnout 3 geometry (.bgv)")


_classes = (EXPORT_OT_bgv,)


def register():
    for cls in _classes:
        bpy.utils.register_class(cls)
    bpy.types.TOPBAR_MT_file_export.append(menu_func_export)


def unregister():
    bpy.types.TOPBAR_MT_file_export.remove(menu_func_export)
    for cls in reversed(_classes):
        bpy.utils.unregister_class(cls)


if __name__ == "__main__":
    register()
