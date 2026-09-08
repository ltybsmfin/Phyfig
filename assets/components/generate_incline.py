"""Reproducible example asset. Runtime components use the same generic evaluator as user assets."""
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
objects, bindings = [], []
base = dict(x=0.0, y=0.0, stroke=2.0, color=[0.16, 0.22, 0.3, 1])


def add(identifier, kind, name, properties, expressions=None, template=None, step=0):
    objects.append(dict(id=identifier, type=kind, name=name, visible=True, locked=False,
                        step=step, properties={**base, **properties}))
    for prop, expression in (expressions or {}).items():
        bindings.append(dict(object=identifier, property=prop, expression=expression))
    if template:
        bindings.append(dict(object=identifier, property="text", template=template))


cx, cy = "-R*sin(angle)", "-R*cos(angle)"
ax, ay = f"{cx}-L*cos(angle)", f"{cy}+L*sin(angle)"
bx = "-R/sin(angle)"
add("smooth", "line", "光滑 AB", dict(dx=1, dy=0), dict(x=ax, y=ay, dx=f"{bx}-({ax})", dy=f"-({ay})"))
add("rough", "ground", "粗糙 BC", dict(dx=1, dy=0), dict(x=bx, dx=f"{cx}-({bx})", dy=cy))
add("track", "arc", "光滑圆弧 CDE", dict(radius=.3, start=233, end=360), dict(radius="R", start="270-angle"))
add("same_height", "line", "B O E 同高", dict(dx=1, dy=0, color=[.55, .63, .7, 1], stroke=1), dict(x=bx, dx=f"R-({bx})"), step=1)
add("radius", "line", "半径 OC", dict(dx=0, dy=-.3, color=[.45, .55, .65, 1], stroke=1), dict(dx=cx, dy=cy), step=1)
add("vertical", "line", "半径 OD", dict(dx=0, dy=-.3, color=[.45, .55, .65, 1], stroke=1), dict(dy="-R"), step=1)
add("angle_mark", "arc", "圆心角", dict(radius=.10, start=233, end=270, color=[.35, .45, .6, 1], stroke=1), dict(radius="R*0.4", start="270-angle"), step=1)
add("block", "rectangle", "物块 P", dict(width=.10, height=.07, rotation=-37, color=[.15, .46, .75, 1]), dict(x=ax, y=ay, rotation="-angle"))
for label, x, y, ox, oy in [
    ("A", ax, ay, -.12, .12), ("B", bx, "0", -.09, .11),
    ("C", cx, cy, -.02, -.065), ("D", "0", "-R", -.02, -.035),
    ("E", "R", "0", .04, .08), ("O", "0", "0", -.04, .12),
    ("P", f"{ax}+.12*cos(angle)", f"{ay}+.12*cos(angle)", .015, .06),
]:
    add("label_" + label, "text", label, dict(text=label, size=21), dict(x=f"({x})+({ox})", y=f"({y})+({oy})"))
add("angle_label", "text", "角度值", dict(text="37°", size=18), dict(x="R*0.13", y="-R*0.44"), template="{angle}°", step=1)
add("radius_label", "text", "半径值", dict(text="R = 0.3 m", size=18), dict(x="R+.12", y="-R*.35"), template="R = {R} m", step=2)
dim_x, dim_y = f"{ax}-.17*sin(angle)", f"{ay}-.17*cos(angle)"
add("dimension", "arrow", "斜面长度", dict(dx=1, dy=-1, stroke=1.5), dict(x=dim_x, y=dim_y, dx="L*cos(angle)", dy="-L*sin(angle)"), step=2)
add("dimension_back", "arrow", "长度反向箭头", dict(dx=-.08, dy=.06, stroke=1.5), dict(x=f"({dim_x})+.08*cos(angle)", y=f"({dim_y})-.08*sin(angle)", dx="-.08*cos(angle)", dy=".08*sin(angle)"), step=2)
add("length_label", "text", "长度值", dict(text="L = 1.9 m", size=18), dict(x=f"({dim_x})+L*.4*cos(angle)-.13", y=f"({dim_y})-L*.4*sin(angle)-.08"), template="L = {L} m", step=2)

definition = dict(version=1, title="斜面与圆弧轨道", duration=3, steps=4, objects=objects,
    componentDefinition=dict(id="incline_arc", parameters={
        "R": dict(default=.30, min=.05, max=10, unit="m"),
        "L": dict(default=1.90, min=.1, max=30, unit="m"),
        "angle": dict(default=37, min=5, max=85, unit="deg"),
    }, bindings=bindings, constraints=[dict(expression="L-R*cos(angle)/sin(angle)", message="AC must exceed BC so that A lies above B")] ))
scene = dict(version=1, title="题目：斜面接圆弧轨道", duration=3, steps=4,
    components={"incline_arc": definition}, objects=[dict(id="apparatus", type="component", name="完整装置",
        visible=True, locked=False, step=0, properties={**base, "definition":"incline_arc", "parameters":{}, "rotation":0, "scale":1})])
for destination, data in [(Path(__file__).with_name("incline-arc.json"), definition), (ROOT / "scenes/incline-arc.json", scene)]:
    destination.write_text(json.dumps(data, ensure_ascii=False, indent=2)+"\n", encoding="utf-8")
