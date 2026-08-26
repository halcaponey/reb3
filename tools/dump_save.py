#!/usr/bin/env python3
"""Decode build/save/progress.b3sv -- the race-flow progress file.

Format (see the "race flow (agent)" block in src/burnout3_full.c):

  header, 32 bytes
    0   char[4]  magic 'B3SV'
    4   u32      version (1)
    8   u32      record_count
    12  u32      record_size (sizeof B3SaveRec == 148)
    16  u32      checksum, FNV-1a over the record array
    20  u32[3]   reserved

  record, 148 bytes, keyed by (track_id, game_type)
    0   char[16] track_id
    16  u32      game_type      0 = RACE
    20  u32      best_position  1-based, 0 = never finished
    24  u32      medal          0 none / 1 bronze / 2 silver / 3 gold
    28  f32      best_total_time
    32  f32      best_lap_time
    36  u32      runs
    40  u32      last_ncars
    44  u32      last_laps
    48  u32      reserved
    52  car[8]   12 bytes each: u8 pos, u8 laps, u8 finished, u8 is_player,
                 f32 total_time, f32 best_lap

Usage: dump_save.py [path]
"""
import struct
import sys

HDR = 32
REC = 148
CAR = 12
MEDAL = ["none", "bronze", "silver", "gold"]
GAME_TYPE = ["RACE", "ROAD_RAGE", "CRASH", "BURNING_LAP",
             "ELIMINATOR", "FACE_OFF", "GRAND_PRIX"]


def fnv1a(b):
    h = 2166136261
    for x in b:
        h = ((h ^ x) * 16777619) & 0xFFFFFFFF
    return h


def tstr(s):
    return "%d:%05.2f" % (int(s // 60), s - 60 * int(s // 60)) if s > 0 else "--"


def main(path):
    d = open(path, "rb").read()
    if len(d) < HDR:
        raise SystemExit("%s: too short" % path)
    magic, ver, n, rsz, csum = struct.unpack_from("<4sIIII", d, 0)
    print("file        %s (%d bytes)" % (path, len(d)))
    print("magic       %s" % magic.decode("ascii", "replace"))
    print("version     %d" % ver)
    print("records     %d" % n)
    print("record_size %d" % rsz)
    body = d[HDR:HDR + n * rsz]
    got = fnv1a(body)
    print("checksum    0x%08X  (computed 0x%08X)  %s"
          % (csum, got, "OK" if got == csum else "MISMATCH"))
    if magic != b"B3SV":
        raise SystemExit("not a B3SV file")
    if rsz != REC:
        print("note: record_size %d != this decoder's %d" % (rsz, REC))
        return
    for i in range(n):
        o = HDR + i * rsz
        tid = d[o:o + 16].split(b"\0")[0].decode("ascii", "replace")
        (gt, bp, med, btt, blt, runs, nc, laps, _res) = \
            struct.unpack_from("<IIIffIIII", d, o + 16)
        gtn = GAME_TYPE[gt] if gt < len(GAME_TYPE) else "?%d" % gt
        print("\n[%d] %-12s game_type=%s" % (i, tid, gtn))
        print("    medal          %d (%s)" % (med, MEDAL[med] if med < 4 else "?"))
        print("    best_position  %s" % (bp if bp else "-- (never finished)"))
        print("    best_total     %s" % tstr(btt))
        print("    best_lap       %s" % tstr(blt))
        print("    runs           %d   grid %d   laps %d" % (runs, nc, laps))
        print("    last standings:")
        for c in range(8):
            co = o + 52 + c * CAR
            pos, lp, fin, me = struct.unpack_from("<BBBB", d, co)
            tot, bl = struct.unpack_from("<ff", d, co + 4)
            if not pos and not lp and not fin:
                continue
            print("      car%d  pos %d  laps %d  %-9s total %-8s best %-8s%s"
                  % (c, pos, lp, "finished" if fin else "DNF",
                     tstr(tot), tstr(bl), "   <- player" if me else ""))


if __name__ == "__main__":
    main(sys.argv[1] if len(sys.argv) > 1 else "build/save/progress.b3sv")
