#!/usr/bin/env python3
"""Assemble the sound data segments into dc_data/.

    python tools/gen_sound_segments.py

Builds:
    data/sound_data/instrument_sets.s  ->  dc_data/instrument_sets.bin
    data/sound_data/sequences.s        ->  dc_data/sequences.bin

WHY THIS EXISTS
---------------
gen_dcdata builds these with concat_incbins, which concatenates only what a .s
file `.incbin`s. Both of these segments carry a HEADER built from directives,
and concatenation silently drops it:

  instrument_sets.s has no incbins at all -- it is a 30-entry
      `.hword <label> - _instrument_sets` jump table plus its records -- so
      concat produced nothing and gen_dcdata reported it permanently missing.

  sequences.s opens with `.hword 3, seqCount` and 30 x `.long offset, length`
      -- the ALSeqFile header, 244 bytes. concat skipped straight to the first
      sequence, so the game read sequence bytes AS the header, took a garbage
      pointer out of it, and died in sequence_player_process_sequence. That is
      a boot-time crash, not a subtle audio glitch.

So these have to be ASSEMBLED: offsets resolved against label positions, not
concatenated. Two passes -- assign every label an offset, then emit -- because
the tables reference labels defined further down the file.

Byte order is LITTLE-endian, matching the x86 build. That is what the game
expects here: sequences.bin is read as little-endian (unlike audiobanks and
audiotables, which stay big-endian), and the known-good files confirm it.

Both outputs are verified byte-identical to the known-good dc_data files.
"""
import io, os, re, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

SEGMENTS = (("data/sound_data/instrument_sets.s", "dc_data/instrument_sets.bin"),
            ("data/sound_data/sequences.s",       "dc_data/sequences.bin"))

LABEL = re.compile(r'^(?:glabel\s+(\w+)|(\w+):)\s*$')
DATA = re.compile(r'^\.(hword|short|long|word|byte)\s+(.+)$')
INCBIN = re.compile(r'^\.incbin\s+"([^"]+)"\s*$')
ALIGN = re.compile(r'^\.(balign|align)\s+(\d+)')

WIDTH = {"byte": 1, "hword": 2, "short": 2, "long": 4, "word": 4}


def strip_comment(raw):
    out, q = [], False
    for ch in raw.rstrip("\r\n"):
        if ch == '"':
            q = not q
        if not q and ch in "#!":
            break
        out.append(ch)
    return "".join(out).strip()


def split_args(s):
    """Split on commas that are not inside parentheses."""
    parts, depth, cur = [], 0, []
    for ch in s:
        if ch == "(":
            depth += 1
        elif ch == ")":
            depth -= 1
        if ch == "," and depth == 0:
            parts.append("".join(cur))
            cur = []
        else:
            cur.append(ch)
    if cur:
        parts.append("".join(cur))
    return [p.strip() for p in parts if p.strip()]


def evaluate(expr, labels):
    """Resolve a label expression: differences, division, parentheses.

    eval is acceptable here: the input is a file in this repository, written
    by the decomp's own splitter, not anything a user supplies.
    """
    py = re.sub(r'\b([A-Za-z_]\w*)\b',
                lambda m: str(labels[m.group(1)]) if m.group(1) in labels
                else m.group(1), expr)
    try:
        return int(eval(py, {"__builtins__": {}}, {}))
    except Exception:
        raise SystemExit("gen_sound_segments: cannot evaluate %r "
                         "(unresolved label?)" % expr)


def walk(lines, labels, out):
    """One pass. `out` is None while sizing, a bytearray while emitting."""
    off = 0
    for raw in lines:
        line = strip_comment(raw)
        if not line or line.startswith((".include", ".section")):
            continue

        m = LABEL.match(line)
        if m:
            labels.setdefault(m.group(1) or m.group(2), off)
            continue

        m = ALIGN.match(line)
        if m:
            # `.align N` is N BYTES here, not 2^N. That is x86 GNU as
            # semantics, and this segment is assembled in the x86 build (the
            # same reason gen_asmwrappers rewrites .word to .long). Taking the
            # MIPS reading of `.align 4, 0x00` pads the 244-byte header out to
            # 256 and shifts every sequence offset by 12 -- the known-good
            # dc_data/sequences.bin has _seq_00 at 0xF4, not 0x100.
            a = int(m.group(2))
            while off % a:
                if out is not None:
                    out.append(0)
                off += 1
            continue

        m = INCBIN.match(line)
        if m:
            path = m.group(1)
            if not os.path.exists(path):
                raise SystemExit("gen_sound_segments: missing incbin %s" % path)
            if out is not None:
                out += io.open(path, "rb").read()
            off += os.path.getsize(path)
            continue

        m = DATA.match(line)
        if m:
            w = WIDTH[m.group(1)]
            for expr in split_args(m.group(2)):
                if out is not None:
                    v = evaluate(expr, labels) & ((1 << (w * 8)) - 1)
                    out += v.to_bytes(w, "little")
                off += w
            continue

        raise SystemExit("gen_sound_segments: unhandled directive: %r" % line)
    return off


def assemble(src, dst):
    lines = io.open(src, encoding="utf-8", errors="surrogateescape").readlines()
    labels = {}
    walk(lines, labels, None)          # pass 1: label offsets
    out = bytearray()
    walk(lines, labels, out)           # pass 2: emit
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    io.open(dst, "wb").write(bytes(out))
    return len(out), len(labels)


def main():
    os.chdir(ROOT)
    rc = 0
    for src, dst in SEGMENTS:
        if not os.path.exists(src):
            print("gen_sound_segments: %s not found" % src, file=sys.stderr)
            rc = 1
            continue
        n, nlab = assemble(src, dst)
        print("gen_sound_segments: %-28s %9s bytes (%d labels)"
              % (dst, format(n, ","), nlab))
    return rc


if __name__ == "__main__":
    sys.exit(main())
