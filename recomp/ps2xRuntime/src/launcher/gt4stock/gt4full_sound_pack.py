#!/usr/bin/env python3
"""GT4 Custom Sounds: build, check and preview engine sound packs.

Standard-library Python only (runs on the laptop's Python 3.14 as-is).
WAV files are read directly; MP3/OGG/FLAC/M4A need ffmpeg on PATH.

  python sound_pack.py import  FILES... --out sound-packs/<car> [--name N] [--car ID] [--cylinders 6]
  python sound_pack.py sweep   RECORDING --load on|off --cylinders 6 --out sound-packs/<car>
  python sound_pack.py check   sound-packs/<car>
  python sound_pack.py preview sound-packs/<car> preview.wav [--seed 1]

File names tell the importer what each recording is, e.g.
  on_3000.wav  4500_off.ogg  idle.wav  limiter.wav  pop1.wav  backfire_big.mp3
Words: on/onload/power/accel/throttle = on-throttle, off/offload/decel/coast/overrun
= off-throttle, idle, limiter/redline, pop/backfire/crackle/bang. A number from
500 to 12000 is the rpm. If a loop has no rpm in its name, the importer guesses it
from the pitch (needs --cylinders) and marks it "rpm_guessed" in pack.json.

"sweep" takes one long recording instead: rev slowly from low to the limiter on
the throttle (and, separately, coast back down off it). It follows the revs by
pitch and cuts a pitch-steadied loop every 1500 rpm. This works with sound
recorded from any game you play, such as Forza, captured with OBS or similar.
"""
import argparse, array, json, math, os, random, re, shutil, struct, subprocess, sys, tempfile, wave

SR = 44100
FORMAT = 1
LOOP_SECONDS = 3.0        # longest loop kept per layer
XFADE_SECONDS = 0.15      # crossfade that makes each loop seamless
TARGET_RMS_DB = -18.0     # every layer is levelled to this
POP_PEAK = 0.9

ON_WORDS = ("onload", "on", "power", "accel", "throttle", "load")
OFF_WORDS = ("offload", "off", "decel", "coast", "overrun")
POP_WORDS = ("pop", "backfire", "crackle", "bang")


# ---------------------------------------------------------------- audio io

def read_wav(path):
    """Return mono float samples at the file's rate, and the rate."""
    with open(path, "rb") as f:
        data = f.read()
    if data[:4] != b"RIFF" or data[8:12] != b"WAVE":
        raise ValueError("not a WAV file")
    pos, fmt, raw = 12, None, None
    while pos + 8 <= len(data):
        cid, size = data[pos:pos + 4], struct.unpack("<I", data[pos + 4:pos + 8])[0]
        body = data[pos + 8:pos + 8 + size]
        if cid == b"fmt ":
            tag, ch, rate, _, _, bits = struct.unpack("<HHIIHH", body[:16])
            if tag == 0xFFFE and len(body) >= 26:          # WAVE_FORMAT_EXTENSIBLE
                tag = struct.unpack("<H", body[24:26])[0]
            fmt = (tag, ch, rate, bits)
        elif cid == b"data":
            raw = body
        pos += 8 + size + (size & 1)
    if not fmt or raw is None:
        raise ValueError("WAV has no fmt or data chunk")
    tag, ch, rate, bits = fmt
    w = bits // 8
    n = len(raw) // (w * ch)
    if tag == 3 and bits == 32:
        vals = array.array("f", raw[:n * ch * 4]); scale = 1.0
    elif tag == 3 and bits == 64:
        vals = array.array("d", raw[:n * ch * 8]); scale = 1.0
    elif tag == 1 and bits == 16:
        vals = array.array("h", raw[:n * ch * 2]); scale = 1 / 32768
    elif tag == 1 and bits == 32:
        vals = array.array("i", raw[:n * ch * 4]); scale = 1 / 2147483648
    elif tag == 1 and bits == 24:
        vals = [int.from_bytes(raw[i:i + 3], "little", signed=True) for i in range(0, n * ch * 3, 3)]
        scale = 1 / 8388608
    elif tag == 1 and bits == 8:
        vals = [b - 128 for b in raw[:n * ch]]; scale = 1 / 128
    else:
        raise ValueError(f"unsupported WAV format (tag {tag}, {bits}-bit)")
    if sys.byteorder != "little" and isinstance(vals, array.array):
        vals.byteswap()
    if ch == 1:
        return [v * scale for v in vals], rate
    return [sum(vals[i:i + ch]) * scale / ch for i in range(0, n * ch, ch)], rate


def write_wav(path, samples, rate=SR):
    pcm = array.array("h", (max(-32767, min(32767, int(round(s * 32767)))) for s in samples))
    if sys.byteorder != "little":
        pcm.byteswap()
    with wave.open(path, "wb") as w:
        w.setnchannels(1); w.setsampwidth(2); w.setframerate(rate); w.writeframes(pcm.tobytes())


def load_any(path):
    if path.lower().endswith(".wav"):
        try:
            return read_wav(path)
        except ValueError:
            pass                       # odd WAV flavour: let ffmpeg try
    ff = shutil.which("ffmpeg")
    if not ff:
        raise SystemExit(f"{os.path.basename(path)}: only WAV works without ffmpeg. "
                         "Install ffmpeg or convert the file to WAV first.")
    with tempfile.TemporaryDirectory() as d:
        tmp = os.path.join(d, "x.wav")
        subprocess.run([ff, "-loglevel", "error", "-y", "-i", path, "-ac", "1", "-ar", str(SR),
                        "-c:a", "pcm_s16le", tmp], check=True)
        return read_wav(tmp)


# ---------------------------------------------------------------- dsp helpers

def resample(x, src, dst=SR):
    if src == dst or not x:
        return list(x)
    step, n = src / dst, int(len(x) * dst / src)
    out, last = [], len(x) - 1
    for i in range(n):
        p = i * step; j = int(p); f = p - j
        out.append(x[j] + (x[min(j + 1, last)] - x[j]) * f)
    return out


def remove_dc(x):
    m = sum(x) / len(x)
    return [v - m for v in x]


def rms(x):
    return math.sqrt(sum(v * v for v in x) / max(1, len(x))) or 1e-9


def gain(x, g):
    return [v * g for v in x]


def make_loop(x):
    """Take the steadiest LOOP_SECONDS from the middle and crossfade its end into its start."""
    want = int(LOOP_SECONDS * SR); xf = int(XFADE_SECONDS * SR)
    if len(x) < xf * 3:
        raise ValueError("recording is too short to loop (needs about half a second)")
    if len(x) > want + xf:
        start = (len(x) - want - xf) // 2
        x = x[start:start + want + xf]
    body, tail = x[:-xf], x[-xf:]
    for i in range(xf):                       # equal-power crossfade tail -> head
        a = i / xf
        body[i] = body[i] * math.sin(a * math.pi / 2) + tail[i] * math.cos(a * math.pi / 2)
    return body


def _decimate(x, dec=4):
    return [sum(x[i:i + dec]) / dec for i in range(0, len(x) - dec, dec)]


def _firing_lag(seg, lo, hi):
    """Autocorrelation period in samples (fractional), or None.

    Real engines repeat strongly every full cycle as well as every firing, so take the
    shortest lag that is nearly as strong as the best one rather than the best one."""
    e0 = sum(v * v for v in seg) or 1e-9
    hi = min(hi, len(seg) // 2)
    if hi <= lo + 2:
        return None
    cs = [sum(seg[i] * seg[i + lag] for i in range(0, len(seg) - lag, 2)) * 2 / e0
          for lag in range(lo, hi)]
    top = max(cs)
    if top < 0.2:
        return None
    for k in range(1, len(cs) - 1):
        if cs[k] >= 0.7 * top and cs[k] >= cs[k - 1] and cs[k] >= cs[k + 1]:
            a, b, c = cs[k - 1], cs[k], cs[k + 1]
            d = a - 2 * b + c
            return lo + k + (0.5 * (a - c) / d if d else 0.0)
    return None


def _lag_range(rate, cylinders, rpm_min=500, rpm_max=12000):
    f_lo, f_hi = rpm_min * cylinders / 120, rpm_max * cylinders / 120
    return max(2, int(rate / f_hi)), int(rate / f_lo) + 2


def guess_rpm(x, cylinders):
    """Estimate firing frequency by autocorrelation, convert to rpm (4-stroke)."""
    n = min(len(x), 8192); s = max(0, len(x) // 2 - n // 2)
    seg = _decimate(x[s:s + n]); rate = SR / 4
    lag = _firing_lag(seg, *_lag_range(rate, cylinders))
    if not lag:
        return None
    return round(rate / lag * 120 / cylinders / 50) * 50


def track_rpm(x, cylinders, rpm_min, rpm_max, hop=0.1, win=0.15):
    """rpm every `hop` seconds through a recording (None where unsure), median-smoothed."""
    rate = SR / 4; d = _decimate(x)
    lo, hi = _lag_range(rate, cylinders, rpm_min, rpm_max)
    w, h = int(win * rate), int(hop * rate)
    raw = []
    for s in range(0, len(d) - w, h):
        lag = _firing_lag(d[s:s + w], lo, hi)
        v = rate / lag * 120 / cylinders if lag else None
        prev = next((p for p in reversed(raw[-5:]) if p), None)
        if v and prev:                         # revs move smoothly: undo octave jumps
            for m in (2, 0.5, 3, 1 / 3):
                if abs(v * m - prev) < abs(v - prev) * 0.5 and rpm_min <= v * m <= rpm_max:
                    v *= m; break
        raw.append(v)
    out = []
    for i in range(len(raw)):
        near = sorted(v for v in raw[max(0, i - 2):i + 3] if v)
        out.append(near[len(near) // 2] if len(near) >= 3 else None)
    return out, hop, win


# ---------------------------------------------------------------- naming

def classify(name):
    base = os.path.splitext(os.path.basename(name))[0].lower()
    words = re.split(r"[^a-z0-9]+", base)
    joined = "_".join(words)
    kind = "loop"
    if any(w.startswith(p) for w in words for p in POP_WORDS):
        kind = "pop"
    elif "idle" in words:
        kind = "idle"
    elif any(w in ("limiter", "redline", "rev", "revlimit", "limit") for w in words):
        kind = "limiter"
    load = None
    if any(w in OFF_WORDS for w in words) or "off_load" in joined:
        load = "off"
    elif any(w in ON_WORDS for w in words) or "on_load" in joined:
        load = "on"
    rpm = None
    for m in re.finditer(r"(\d{3,5})", base):
        v = int(m.group(1))
        if 500 <= v <= 12000:
            rpm = v; break
    return kind, load, rpm


# ---------------------------------------------------------------- commands

def new_pack(a):
    return {"format": FORMAT, "name": a.name or os.path.basename(os.path.normpath(a.out)),
            "car": a.car or "all", "cylinders": a.cylinders, "layers": [], "idle": None,
            "limiter": None, "pops": [],
            "settings": {"volume": 1.0, "bass_db": 0.0, "pop_chance": 0.6, "pop_volume": 0.8},
            "source_note": a.note or ""}


def cmd_import(a):
    os.makedirs(a.out, exist_ok=True)
    pack = new_pack(a)
    target = 10 ** (TARGET_RMS_DB / 20)
    used = set()
    for path in a.files:
        kind, load, rpm = classify(path)
        x, rate = load_any(path)
        x = remove_dc(resample(x, rate))
        stem = re.sub(r"[^A-Za-z0-9_-]+", "_", os.path.splitext(os.path.basename(path))[0])
        fname = stem + ".wav"
        while fname in used:
            fname = stem + f"_{len(used)}.wav"
        used.add(fname)
        if kind == "pop":
            peak = max(abs(v) for v in x) or 1e-9
            x = gain(x, POP_PEAK / peak)
            write_wav(os.path.join(a.out, fname), x)
            pack["pops"].append({"file": fname})
            print(f"  pop      {fname}")
            continue
        x = make_loop(x)
        x = gain(x, target / rms(x))
        peak = max(abs(v) for v in x)
        if peak > 0.98:
            x = gain(x, 0.98 / peak)
        write_wav(os.path.join(a.out, fname), x)
        guessed = False
        if rpm is None:
            rpm = guess_rpm(x, a.cylinders); guessed = rpm is not None
        entry = {"file": fname, "rpm": rpm}
        if guessed:
            entry["rpm_guessed"] = True
        if kind in ("idle", "limiter"):
            pack[kind] = entry
            print(f"  {kind:8} {fname}  rpm {rpm}")
            continue
        entry["load"] = load or "on"
        if load is None:
            entry["load_guessed"] = True
        pack["layers"].append(entry)
        print(f"  {entry['load']+'-throttle':12} {fname}  rpm {rpm}{' (guessed from pitch)' if guessed else ''}")
    pack["layers"].sort(key=lambda e: (e["load"], e["rpm"] or 0))
    with open(os.path.join(a.out, "pack.json"), "w") as f:
        json.dump(pack, f, indent=2)
    print(f"Wrote {os.path.join(a.out, 'pack.json')}")
    return report(a.out, pack)


def load_pack(folder):
    with open(os.path.join(folder, "pack.json")) as f:
        return json.load(f)


def report(folder, pack):
    problems, notes = [], []
    if pack.get("format") != FORMAT:
        problems.append(f"unknown pack format {pack.get('format')}")
    on = [l for l in pack["layers"] if l["load"] == "on"]
    off = [l for l in pack["layers"] if l["load"] == "off"]
    if not on:
        problems.append("needs at least one on-throttle loop")
    for l in pack["layers"] + [e for e in (pack.get("idle"), pack.get("limiter")) if e]:
        if not os.path.exists(os.path.join(folder, l["file"])):
            problems.append(f"missing file {l['file']}")
        if not l.get("rpm"):
            problems.append(f"{l['file']}: no rpm (put it in the file name, e.g. on_4000.wav)")
        elif l.get("rpm_guessed"):
            notes.append(f"{l['file']}: rpm {l['rpm']} was guessed from the pitch, check it")
        if l.get("load_guessed"):
            notes.append(f"{l['file']}: treated as on-throttle (name didn't say on or off)")
    for group in (on, off):
        rpms = [l["rpm"] for l in group if l.get("rpm")]
        if len(rpms) != len(set(rpms)):
            problems.append("two loops share the same rpm and load")
    if len(on) == 1:
        notes.append("only one on-throttle loop: it works, but 3 to 6 across the rev range sound much better")
    if not off:
        notes.append("no off-throttle loops: lifting off will use the on-throttle sound, quieter")
    if not pack["pops"]:
        notes.append("no pops: backfires will use the built-in ones")
    for p in problems:
        print("  PROBLEM:", p)
    for n in notes:
        print("  note:", n)
    print("  OK" if not problems else f"  {len(problems)} problem(s)")
    return 1 if problems else 0


def cmd_check(a):
    return report(a.pack, load_pack(a.pack))


class Voice:
    def __init__(self, data, rpm):
        self.d, self.rpm, self.pos = data, rpm, 0.0

    def next(self, ratio):
        d, n = self.d, len(self.d)
        j = int(self.pos); f = self.pos - j
        v = d[j % n] + (d[(j + 1) % n] - d[j % n]) * f
        self.pos = (self.pos + ratio) % n
        return v


def blend(voices, rpm):
    """Equal-power crossfade between the two loops either side of rpm."""
    if not voices:
        return 0.0
    if rpm <= voices[0].rpm or len(voices) == 1:
        v = voices[0]; return v.next(rpm / v.rpm)
    if rpm >= voices[-1].rpm:
        v = voices[-1]; return v.next(rpm / v.rpm)
    for a, b in zip(voices, voices[1:]):
        if a.rpm <= rpm <= b.rpm:
            w = (rpm - a.rpm) / (b.rpm - a.rpm)
            out = a.next(rpm / a.rpm) * math.cos(w * math.pi / 2) + b.next(rpm / b.rpm) * math.sin(w * math.pi / 2)
            for o in voices:                  # keep the others moving so they don't jump in later
                if o is not a and o is not b:
                    o.pos = (o.pos + rpm / o.rpm) % len(o.d)
            return out
    return 0.0


def cmd_preview(a):
    """Render a test drive: rev up, lift off with downshifts, accelerate out, twice."""
    folder, pack = a.pack, load_pack(a.pack)
    rnd = random.Random(a.seed)
    def wav(fn):
        x, r = read_wav(os.path.join(folder, fn)); return resample(x, r)
    on = [Voice(wav(l["file"]), l["rpm"]) for l in sorted(pack["layers"], key=lambda l: l["rpm"]) if l["load"] == "on"]
    off = [Voice(wav(l["file"]), l["rpm"]) for l in sorted(pack["layers"], key=lambda l: l["rpm"]) if l["load"] == "off"]
    pops = [wav(p["file"]) for p in pack["pops"]]
    s = pack["settings"]
    if not pops:                               # built-in pop, so lift-offs still pop
        pops = [[math.exp(-i / 900) * (rnd.uniform(-1, 1) * 0.8 + math.sin(i * 2 * math.pi * 60 / SR) * 0.6)
                 for i in range(int(0.12 * SR))]]

    # build rpm/throttle script at 1 kHz control rate
    ctl = []
    def hold(sec, rpm0, rpm1, thr):
        n = int(sec * 1000)
        for i in range(n):
            ctl.append((rpm0 + (rpm1 - rpm0) * i / max(1, n - 1), thr))
    hold(0.8, 1000, 1000, 0.1)                  # idle
    for _ in range(2):
        hold(2.0, 3000, 7400, 1.0)              # pull hard
        hold(0.5, 7400, 5600, 0.0)              # lift
        for top in (7000, 6800):                # two downshifts with a blip
            hold(0.12, 5600, top, 0.7); hold(0.55, top, 5400, 0.0)
        hold(1.6, 4200, 7000, 1.0)              # accelerate out
    hold(1.0, 7000, 1100, 0.0)

    vol, pv = s.get("volume", 1.0), s.get("pop_volume", 0.8)
    bass_g = 10 ** (s.get("bass_db", 0.0) / 20) - 1
    a_lp = 1 - math.exp(-2 * math.pi * 120 / SR)
    out, lp, rpm_s, thr_s = [], 0.0, ctl[0][0], ctl[0][1]
    active, prev_thr = [], 1.0
    per = SR // 1000
    for k, (rpm_t, thr_t) in enumerate(ctl):
        lifted = prev_thr > 0.6 and thr_t < 0.2 and rpm_t > 4000
        blipped = prev_thr < 0.2 and 0.5 < thr_t < 0.9
        if (lifted or blipped) and rnd.random() < s.get("pop_chance", 0.6):
            t = len(out) + int(SR * rnd.uniform(0.05, 0.2))
            for _ in range(rnd.randint(1, 5 if lifted else 3)):
                active.append([t, rnd.choice(pops), rnd.uniform(0.4, 1.0) * pv])
                t += int(SR * rnd.uniform(0.03, 0.15))
        prev_thr = thr_t
        for _ in range(per):
            rpm_s += (rpm_t - rpm_s) * 0.002
            thr_s += (thr_t - thr_s) * 0.004
            e_on = blend(on, rpm_s)
            e_off = blend(off, rpm_s) if off else e_on * 0.35
            v = (e_on * thr_s + e_off * (1 - thr_s)) * vol
            n = len(out)
            for p in active:
                i = n - p[0]
                if 0 <= i < len(p[1]):
                    v += p[1][i] * p[2]
            lp += a_lp * (v - lp)
            v += lp * bass_g
            out.append(math.tanh(v * 1.2) * 0.85)
        active = [p for p in active if len(out) - p[0] < len(p[1])]
    write_wav(a.out, out)
    print(f"Wrote {a.out} ({len(out) / SR:.1f} s)")
    return 0


def cmd_sweep(a):
    """Cut rpm layers out of one slow rev-sweep recording (on throttle up, or coasting down)."""
    x, rate = load_any(a.file)
    x = remove_dc(resample(x, rate))
    track, hop, win = track_rpm(x, a.cylinders, a.rpm_min, a.rpm_max)
    good = [(i, r) for i, r in enumerate(track) if r]
    if len(good) < 10:
        raise SystemExit("Couldn't follow the revs in this recording. Check --cylinders, "
                         "or record a slower, cleaner sweep with less wind and tyre noise.")
    lo_r, hi_r = min(r for _, r in good), max(r for _, r in good)
    print(f"  followed the revs from about {lo_r:.0f} to {hi_r:.0f} rpm")
    # rpm at every sample, linear between track points (track point i sits at i*hop + win/2)
    pts = [(int((i * hop + win / 2) * SR), r) for i, r in good]
    def rpm_at(n):
        if n <= pts[0][0]: return pts[0][1]
        if n >= pts[-1][0]: return pts[-1][1]
        lo, hi = 0, len(pts) - 1
        while hi - lo > 1:
            mid = (lo + hi) // 2
            if pts[mid][0] <= n: lo = mid
            else: hi = mid
        (n0, r0), (n1, r1) = pts[lo], pts[hi]
        return r0 + (r1 - r0) * (n - n0) / max(1, n1 - n0)
    targets = [int(v) for v in a.points.split(",")] if a.points else \
        [r for r in range(1500, 12001, 1500) if lo_r * 1.04 <= r <= hi_r * 0.96]
    os.makedirs(a.out, exist_ok=True)
    pj = os.path.join(a.out, "pack.json")
    pack = load_pack(a.out) if os.path.exists(pj) else new_pack(a)
    pack["cylinders"] = a.cylinders
    target_lvl = 10 ** (TARGET_RMS_DB / 20)
    made = 0
    for t in targets:
        # longest stretch around the closest point that stays within 12% of the target
        best = min(pts, key=lambda p: abs(p[1] - t))
        if abs(best[1] - t) > t * 0.06:
            print(f"  {t} rpm: not in this recording, skipped"); continue
        c = best[0]; s = e = c
        while s > 0 and abs(rpm_at(s) - t) < t * 0.12 and c - s < SR * 1.5: s -= 256
        while e < len(x) and abs(rpm_at(e) - t) < t * 0.12 and e - c < SR * 1.5: e += 256
        # read faster where the revs are below the target and slower above, so the pitch stays flat
        out, pos = [], float(s)
        while pos < e - 1:
            j = int(pos); f = pos - j
            out.append(x[j] + (x[j + 1] - x[j]) * f)
            pos += t / rpm_at(j)
        if len(out) < SR * 0.5:
            print(f"  {t} rpm: the revs went through too fast here, skipped (sweep more slowly)"); continue
        loop = make_loop(out)
        loop = gain(loop, target_lvl / rms(loop))
        pk = max(abs(v) for v in loop)
        if pk > 0.98: loop = gain(loop, 0.98 / pk)
        fname = f"sweep_{a.load}_{t}.wav"
        write_wav(os.path.join(a.out, fname), loop)
        pack["layers"] = [l for l in pack["layers"] if not (l["load"] == a.load and l["rpm"] == t)]
        pack["layers"].append({"file": fname, "rpm": t, "load": a.load, "from_sweep": True})
        made += 1
        print(f"  {a.load}-throttle {fname}  {len(loop) / SR:.1f} s loop")
    pack["layers"].sort(key=lambda e: (e["load"], e["rpm"] or 0))
    with open(pj, "w") as f:
        json.dump(pack, f, indent=2)
    print(f"Made {made} loops. Wrote {pj}")
    return report(a.out, pack)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("import"); p.add_argument("files", nargs="+"); p.add_argument("--out", required=True)
    p.add_argument("--name"); p.add_argument("--car"); p.add_argument("--note")
    p.add_argument("--cylinders", type=int, default=6)
    p = sub.add_parser("sweep"); p.add_argument("file"); p.add_argument("--out", required=True)
    p.add_argument("--load", choices=("on", "off"), default="on")
    p.add_argument("--cylinders", type=int, required=True)
    p.add_argument("--points", help="rpm list, e.g. 1500,3000,4500 (default: every 1500 the recording covers)")
    p.add_argument("--rpm-min", type=int, default=600); p.add_argument("--rpm-max", type=int, default=10000)
    p.add_argument("--name"); p.add_argument("--car"); p.add_argument("--note")
    p = sub.add_parser("check"); p.add_argument("pack")
    p = sub.add_parser("preview"); p.add_argument("pack"); p.add_argument("out"); p.add_argument("--seed", type=int)
    a = ap.parse_args()
    return {"import": cmd_import, "sweep": cmd_sweep, "check": cmd_check, "preview": cmd_preview}[a.cmd](a)


if __name__ == "__main__":
    sys.exit(main())
