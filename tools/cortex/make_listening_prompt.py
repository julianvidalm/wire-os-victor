#!/usr/bin/env python3
"""
Generates the PC cortex "are you there?" prompt as one native animation clip (design option B):

  resources/config/engine/animations/anim_cortex_listening_prompt_01.json
    ProceduralFaceKeyFrame track: from the listening pose (anim_eyepose_curious) to an asymmetric "huh?" look
    (screen-left eye a bit higher and squinted, screen-right eye higher and slightly taller) and back, and
    SpriteBoxKeyFrame track: the question mark sprite sequences above the eyes.
  resources/config/sprites/spriteSequences/cortex_question_mark_in/   the mark draws itself in one stroke, then
                                                                      the dot pops (held on its last frame)
  resources/config/sprites/spriteSequences/cortex_question_mark_out/  the mark fades out (cleared after it)

The mark is white (RGBA render method, not the eye color). The face's sprite compositor copies every sprite
pixel except opaque black (Vision::ImageBase::DrawSubImage), with no alpha blending, so the frames are drawn
over opaque black and fade by getting darker.

Pure Python 3 (no third party modules). Run from anywhere:  python3 tools/cortex/make_listening_prompt.py
"""

import json
import math
import os
import struct
import zlib

REPO = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
CLIP_NAME = "anim_cortex_listening_prompt_01"
CLIP_PATH = os.path.join(REPO, "resources/config/engine/animations", CLIP_NAME + ".json")
SEQ_DIR = os.path.join(REPO, "resources/config/sprites/spriteSequences")
SEQ_IN = "cortex_question_mark_in"
SEQ_OUT = "cortex_question_mark_out"

STEP_MS = 33                      # ANIM_TIME_STEP_MS: one sprite sequence frame per animation tick
FACE_W, FACE_H = 184, 96

# ---- timeline: the design's 4 s loop scaled to the 3 s prompt (fractions of the design loop) ---------------
CLIP_MS = 3000
def tick(fraction):
    return int(round(fraction * CLIP_MS / STEP_MS))

EYES_UP = (0.0, 0.12)             # ease-in-out
EYES_DOWN = (0.82, 0.95)          # ease-in-out
DRAW = (0.10, 0.26)               # stroke draws itself, ease-out
DOT = (0.26, 0.31, 0.35)          # dot scale 0 -> 1.5 -> 1, ease-out
FADE = (0.80, 0.90)               # fade out, ease-out
LAST_FACE_TICK = CLIP_MS // STEP_MS - 1   # the face track runs to the clip's end, past the sprite's clear

# ---- the mark (design SVG: viewBox -30 -40 60 80, rendered 92x122 at 3x) -----------------------------------
SPRITE_W, SPRITE_H = 31, 41
UNIT_PX = 0.512                   # 92 px / 60 units / 3
STROKE_R = 6.5                    # stroke-width 13
DOT_C, DOT_R = (1.0, 26.0), 7.5
PATH = [((-13, -14), (-13, -33), (15, -33), (15, -14)),
        ((15, -14), (15, -3), (1, -3), (1, 8))]
# sprite box: the mark's glyph is centered on the gap between the eyes (x = 92), at the top of the screen
SPRITE_X, SPRITE_Y = 92 - 16, 0

# ---- the eyes: listening pose = anim_eyepose_curious's keyframe ---------------------------------------------
BASE = {"Name": "ProceduralFaceKeyFrame", "faceScaleY": 1.0651487803082709, "scanlineOpacity": 1.0,
        "faceScaleX": 0.9749010158432679, "faceAngle": 0.0, "durationTime_ms": 0, "faceCenterY": 0.0,
        "faceCenterX": 0.0,
        "leftEye": [8.107326060759803, 0.0, 1.5989466956463083, 1.1453089027930028, 0.0, 0.4939051626438526,
                    0.5523085770003507, 0.5608125567086282, 0.5791440668907788, 0.6936300157286882,
                    0.780807132929412, 0.5374504378663092, 0.5961370824995641, 0.0, -0.7970601913435532,
                    -0.14583334377849466, 0.0, 0.0, 0.0, 1, 1.0, 0.5, 0.0, 0.0, 0],
        "rightEye": [-7.315991464547729, 1.0219525223344306, 1.5989466956463083, 1.1196454976440522, 0.0,
                     0.4939051626438526, 0.5523085770003507, 0.5608125567086282, 0.5791440668907788,
                     0.6936300157286882, 0.780807132929412, 0.5374504378663092, 0.5961370824995641, 0.0,
                     -0.7970601913435532, -0.14583334377849466, 0.0, 0.0, 0.0, 1, 1.0, 0.5, 0.0, 0.0, 0]}
CX, CY, SX, SY = 0, 1, 2, 3       # ProceduralEyeParameter EyeCenterX, EyeCenterY, EyeScaleX, EyeScaleY
NOMINAL_EYE_H = 57
# design (screen px = 3x px / 3): left eye rises 16/3 px and squints to 0.82 (bottom anchored); right eye
# rises 34/3 px and grows to 1.04 (bottom anchored)
LEFT_RISE_PX, LEFT_SCALE = 16 / 3, 0.82
RIGHT_RISE_PX, RIGHT_SCALE = 34 / 3, 1.04
# robot only: the listening eyes are much bigger than the mockup's, so they move 6 px apart and get 6% narrower
# to open a ~24 px gap for the mark (the mockup's gap is 23 px)
SPREAD_PX = 6.0
WIDTH_SCALE = 1.5 / 1.5989466956463083


def cubic_bezier_ease(x1, y1, x2, y2):
    def bez(t, a, b):
        return 3 * a * t * (1 - t) ** 2 + 3 * b * t * t * (1 - t) + t ** 3
    def ease(x):
        lo, hi = 0.0, 1.0
        for _ in range(40):
            mid = (lo + hi) / 2
            if bez(mid, x1, x2) < x:
                lo = mid
            else:
                hi = mid
        return bez((lo + hi) / 2, y1, y2)
    return ease

EASE_IN_OUT = cubic_bezier_ease(0.42, 0.0, 0.58, 1.0)
EASE_OUT = cubic_bezier_ease(0.0, 0.0, 0.58, 1.0)


# ---- face track ---------------------------------------------------------------------------------------------
def huh_face(amount):
    face = json.loads(json.dumps(BASE))
    sy = BASE["faceScaleY"]
    for eye, rise_px, scale, spread in (("leftEye", LEFT_RISE_PX, LEFT_SCALE, -SPREAD_PX),
                                        ("rightEye", RIGHT_RISE_PX, RIGHT_SCALE, SPREAD_PX)):
        p = face[eye]
        height = NOMINAL_EYE_H * p[SY]
        # bottom anchored scale: the center moves by half the height change; then the rise
        target_cy = p[CY] + height * (1 - scale) / 2 - rise_px / sy
        target = {CX: p[CX] + spread, CY: target_cy, SX: p[SX] * WIDTH_SCALE, SY: p[SY] * scale}
        for idx, value in target.items():
            p[idx] = p[idx] + (value - p[idx]) * amount
    return face


def face_track():
    frames = []
    def add(t, amount):
        kf = huh_face(amount)
        kf["triggerTime_ms"] = t * STEP_MS
        frames.append(kf)
    up0, up1 = tick(EYES_UP[0]), tick(EYES_UP[1])
    down0, down1 = tick(EYES_DOWN[0]), tick(EYES_DOWN[1])
    # eased transitions sampled every 2 ticks (keyframes interpolate linearly), held in between
    for t in list(range(up0, up1, 2)) + [up1]:
        add(t, EASE_IN_OUT((t - up0) / (up1 - up0)))
    for t in list(range(down0, down1, 2)) + [down1]:
        add(t, 1 - EASE_IN_OUT((t - down0) / (down1 - down0)))
    add(LAST_FACE_TICK, 0.0)
    return frames


# ---- question mark frames -----------------------------------------------------------------------------------
def to_px(u, v):
    return ((u + 30) * UNIT_PX + (SPRITE_W - 60 * UNIT_PX) / 2, (v + 40) * UNIT_PX + (SPRITE_H - 80 * UNIT_PX) / 2)


def polyline():
    pts = []
    for (p0, p1, p2, p3) in PATH:
        for i in range(0 if not pts else 1, 61):
            t = i / 60
            pts.append(tuple(
                (1 - t) ** 3 * p0[k] + 3 * (1 - t) ** 2 * t * p1[k] + 3 * (1 - t) * t * t * p2[k] + t ** 3 * p3[k]
                for k in range(2)))
    return [to_px(*p) for p in pts]


def seg_dist(px, py, ax, ay, bx, by):
    dx, dy = bx - ax, by - ay
    l2 = dx * dx + dy * dy
    t = 0.0 if l2 == 0 else max(0.0, min(1.0, ((px - ax) * dx + (py - ay) * dy) / l2))
    qx, qy = ax + t * dx - px, ay + t * dy - py
    return math.sqrt(qx * qx + qy * qy)


class MarkRenderer:
    SS = 4  # supersampling per axis

    def __init__(self):
        self.pts = polyline()
        self.cum = [0.0]
        for a, b in zip(self.pts, self.pts[1:]):
            self.cum.append(self.cum[-1] + math.dist(a, b))
        self.length = self.cum[-1]
        self.samples = []
        n = self.SS
        for y in range(SPRITE_H):
            for x in range(SPRITE_W):
                for j in range(n):
                    for i in range(n):
                        self.samples.append((x + (i + 0.5) / n, y + (j + 0.5) / n))
        # prefix minimum of the distance to the first k segments, per sample
        self.prefix = []
        segs = list(zip(self.pts, self.pts[1:]))
        for (sx, sy) in self.samples:
            best, row = float("inf"), []
            for (a, b) in segs:
                best = min(best, seg_dist(sx, sy, a[0], a[1], b[0], b[1]))
                row.append(best)
            self.prefix.append(row)

    def render(self, draw=1.0, dot_scale=1.0, brightness=1.0):
        r_px = STROKE_R * UNIT_PX
        cut = draw * self.length
        k = 0
        while k + 1 < len(self.cum) - 1 and self.cum[k + 1] < cut:
            k += 1
        a, b = self.pts[k], self.pts[k + 1]
        seg_len = self.cum[k + 1] - self.cum[k]
        f = 0.0 if seg_len == 0 else max(0.0, min(1.0, (cut - self.cum[k]) / seg_len))
        bx, by = a[0] + (b[0] - a[0]) * f, a[1] + (b[1] - a[1]) * f
        dcx, dcy = to_px(*DOT_C)
        dr = DOT_R * UNIT_PX * dot_scale
        n2 = self.SS * self.SS
        cover = [0] * (SPRITE_W * SPRITE_H)
        for idx, (sx, sy) in enumerate(self.samples):
            inside = False
            if draw > 0:
                d = seg_dist(sx, sy, a[0], a[1], bx, by)
                if k > 0:
                    d = min(d, self.prefix[idx][k - 1])
                inside = d <= r_px
            if not inside and dot_scale > 0:
                inside = (sx - dcx) ** 2 + (sy - dcy) ** 2 <= dr * dr
            if inside:
                cover[idx // n2] += 1
        return [round(255 * brightness * c / n2) for c in cover]


def write_png(path, values):
    rows = b""
    for y in range(SPRITE_H):
        row = bytearray([0])
        for x in range(SPRITE_W):
            v = values[y * SPRITE_W + x]
            row += bytes((v, v, v, 255))
        rows += bytes(row)
    def chunk(tag, data):
        return struct.pack(">I", len(data)) + tag + data + struct.pack(">I", zlib.crc32(tag + data) & 0xffffffff)
    png = (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", SPRITE_W, SPRITE_H, 8, 6, 0, 0, 0)) +
           chunk(b"IDAT", zlib.compress(rows, 9)) + chunk(b"IEND", b""))
    with open(path, "wb") as fh:
        fh.write(png)


def write_sequence(name, frames):
    folder = os.path.join(SEQ_DIR, name)
    os.makedirs(folder, exist_ok=True)
    for old in os.listdir(folder):
        if old.endswith(".png"):
            os.remove(os.path.join(folder, old))
    for i, values in enumerate(frames):
        write_png(os.path.join(folder, "%s_%02d.png" % (name, i)), values)
    return len(frames)


def dot_scale_at(t):
    t0, t1, t2 = (tick(f) for f in DOT)
    if t <= t0:
        return 0.0
    if t <= t1:
        return 1.5 * EASE_OUT((t - t0) / (t1 - t0))
    if t <= t2:
        return 1.5 - 0.5 * EASE_OUT((t - t1) / (t2 - t1))
    return 1.0


def clean_floats(value):
    """4 decimals, and float noise (|x| < 1e-9) snapped to 0, so equal poses are written identically"""
    if isinstance(value, float):
        value = round(value, 4)
        return 0.0 if abs(value) < 1e-9 else value
    if isinstance(value, list):
        return [clean_floats(v) for v in value]
    if isinstance(value, dict):
        return {k: clean_floats(v) for k, v in value.items()}
    return value


def main():
    mark = MarkRenderer()
    d0, d1 = tick(DRAW[0]), tick(DRAW[1])
    dot_end = tick(DOT[2])
    frames_in = []
    for t in range(d0, dot_end + 1):
        draw = 1.0 if t >= d1 else EASE_OUT((t - d0 + 1) / (d1 - d0 + 1))
        frames_in.append(mark.render(draw, dot_scale_at(t)))
    f0, f1 = tick(FADE[0]), tick(FADE[1])
    frames_out = [mark.render(1.0, 1.0, 1 - EASE_OUT((t - f0 + 1) / (f1 - f0 + 1)))
                  for t in range(f0, f1)]
    n_in = write_sequence(SEQ_IN, frames_in)
    n_out = write_sequence(SEQ_OUT, frames_out)

    def box(t, asset, end):
        return {"Name": "SpriteBoxKeyFrame", "triggerTime_ms": t * STEP_MS, "spriteBoxName": "SpriteBox_1",
                "layer": "Layer_6", "assetName": asset, "renderMethod": "RGBA", "spriteSeqEndType": end,
                "alpha": 100, "xPos": SPRITE_X, "yPos": SPRITE_Y, "width": SPRITE_W, "height": SPRITE_H}
    keyframes = clean_floats(face_track() + [box(d0, SEQ_IN, "Hold"), box(f0, SEQ_OUT, "Clear")])
    with open(CLIP_PATH, "w") as fh:
        json.dump({CLIP_NAME: keyframes}, fh, indent=2)
        fh.write("\n")

    # report: eye boxes at the "huh?" pose (screen px) and the mark's ink box
    face = huh_face(1.0)
    for eye, nominal_x in (("leftEye", 46), ("rightEye", 138)):
        p = face[eye]
        cx = 92 + (nominal_x + p[CX] - 92) * face["faceScaleX"]
        cy = 48 + p[CY] * face["faceScaleY"]
        hw = 43 * p[SX] / 2 * face["faceScaleX"]
        hh = NOMINAL_EYE_H * p[SY] / 2 * face["faceScaleY"]
        print("%s x %.1f..%.1f y %.1f..%.1f" % (eye, cx - hw, cx + hw, cy - hh, cy + hh))
    full = frames_in[-1]
    xs = [x for y in range(SPRITE_H) for x in range(SPRITE_W) if full[y * SPRITE_W + x] > 0]
    ys = [y for y in range(SPRITE_H) for x in range(SPRITE_W) if full[y * SPRITE_W + x] > 0]
    print("mark ink x %d..%d y %d..%d (sprite %dx%d at %d,%d)" % (
        SPRITE_X + min(xs), SPRITE_X + max(xs), SPRITE_Y + min(ys), SPRITE_Y + max(ys),
        SPRITE_W, SPRITE_H, SPRITE_X, SPRITE_Y))
    print("%s: %d frames from %d ms (Hold); %s: %d frames from %d ms (Clear at %d ms); face keyframes to %d ms"
          % (SEQ_IN, n_in, d0 * STEP_MS, SEQ_OUT, n_out, f0 * STEP_MS, (f0 + n_out) * STEP_MS,
             LAST_FACE_TICK * STEP_MS))


if __name__ == "__main__":
    main()
