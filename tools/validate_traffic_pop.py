#!/usr/bin/env python3
"""Differential regression for retail's TRAFFIC VIEW GATE -- the rule that
stops a traffic car appearing (or vanishing) inside the player's view.

The user report is "traffic still spawns in / out on RE".  Retail has exactly
one rule against that, and the harness had none of it:

    FUN_001A6070 @0x001A64E5..0x001A6566
        after FUN_001A2B20 has BUILT the body, the manager walks the local
        view list it was handed and, for the first view closer than 160.0 m
        (DAT_003A49FC), calls FUN_001A75A0 -- which frees the body straight
        back to the pool.  Retail pays for the car and throws it away rather
        than let it appear in shot.  The y lane of the difference is
        explicitly zeroed at 0x001A6505, so the gate is a HORIZONTAL distance.

and three ways to RETIRE one, none of which is a distance:

    FUN_001A3470 @0x001A38BA   the request stopped being stamped
    FUN_001A6B40 @0x001A6DC4   the DESCRIPTOR END -- but only when no
                               successor path is selected (body+0x118 == 0)
    FUN_00114910 @0x00114CE0   promotion to a real vehicle

The pop-OUT half of the report turns on the last two rules and on how retail
decides which requests are live at all, so this suite also pins:

    the terminus test          FUN_001A6B40 @0x001A6D8A..0x001A6DC9
    the branch column formula  FUN_001A0750 @0x001A07A6..0x001A07C6
    the branch budget's source FUN_001A09F0 @0x001A0A85 (per ROW CROSSING),
                               and the per-VIEW stack record in FUN_001A20F0
                               that was mistaken for it
    the sticky window index    FUN_001A28B0 @0x001A29B5..0x001A29EA
    the request DIRECTION      FUN_001A3470 @0x001A34AC..0x001A34EC, which
                               dispatches a request to STAMP or to UNSTAMP

Sections
    A  IMAGE BYTES     the constant, the gate's own instruction bytes, the
                       view list threaded down from FUN_001AA100, and the
                       structural fact that the retire path contains no
                       square root at all.
    B  UNICORN         retail's gate block executed out of build/burnout3.elf
                       over a grid of view/body offsets, against the model.
    C  PORT SOURCE     src/burnout3_full.c carries the same constant, applies
                       it at the one place retail applies it, and keeps its
                       GLUE watchdogs off the pool path.
    D  MODEL REPLAY    the recovered placement law replayed over the shipped
                       traffic_paths.bin with the gate on: it must bite, and
                       it must not starve a window group.

Run:  python3 tools/validate_traffic_pop.py
      B3_FULL_C=<path>  points section C at a candidate burnout3_full.c
"""
import os
import re
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ELF = os.path.join(REPO, 'build', 'burnout3.elf')
FULL_C = os.environ.get('B3_FULL_C',
                        os.path.join(REPO, 'src', 'burnout3_full.c'))

# ---------------------------------------------------------------- addresses
A_GATE_TOP = 0x001A64E5      # MOV ECX,[EBP+0x10]  -- the view-count test
A_GATE_CULL = 0x001A6564     # MOV ECX,ESI ; CALL FUN_001A75A0
A_GATE_KEEP = 0x001A656B     # the loop tail: the car survives
A_VIEW_CONST = 0x003A49FC    # 160.0f
A_DESTROY = 0x001A75A0       # FUN_001A75A0, the body teardown
F_1A3470 = (0x001A3470, 0x001A38EF)   # the stamp/unstamp walker
F_1A6070 = (0x001A6070, 0x001A658D)   # the population law + placement

VIEW_GATE_M = 160.0


# ------------------------------------------------- correctly-mapped image
class Image(object):
    def __init__(self, path):
        with open(path, 'rb') as handle:
            self.data = handle.read()
        d = self.data
        if d[:4] != b'\x7fELF':
            raise SystemExit('%s is not an ELF -- run tools/xbe2elf.py' % path)
        ph_off, = struct.unpack_from('<I', d, 0x1C)
        ph_size, = struct.unpack_from('<H', d, 0x2A)
        ph_num, = struct.unpack_from('<H', d, 0x2C)
        self.segs = []
        for i in range(ph_num):
            o = ph_off + i * ph_size
            (p_type, p_off, p_va, _pa, p_filesz,
             _memsz, _fl, _al) = struct.unpack_from('<8I', d, o)
            if p_type == 1:
                self.segs.append((p_va, p_off, p_filesz))

    def read(self, va, n):
        for base, off, size in self.segs:
            if base <= va and va + n <= base + size:
                return self.data[off + va - base:off + va - base + n]
        raise AssertionError('VA %#x+%d is not in the file' % (va, n))

    def f32(self, va):
        return struct.unpack('<f', self.read(va, 4))[0]


CHECKS = [0]
FAILED = [0]


def check(name, ok, detail=''):
    CHECKS[0] += 1
    if not ok:
        FAILED[0] += 1
        print('  FAIL %s  %s' % (name, detail))
    return ok


def hexs(b):
    return ' '.join('%02X' % x for x in b)


# ====================================================== A. IMAGE BYTES
# Every byte below was read out of build/burnout3.elf; the disassembly beside
# each line is what the bridge prints for the same address.
GATE_BYTES = (
    '8B 4D 10 '                       # MOV ECX,[EBP+0x10]      view count
    '33 C0 '                          # XOR EAX,EAX             i = 0
    '85 C9 '                          # TEST ECX,ECX
    '7E 7D '                          # JLE 0x1A656B            no views: keep
    '0F 28 96 A0 00 00 00 '           # MOVAPS XMM2,[ESI+0xA0]  the new body
    '8B 4D 0C '                       # MOV ECX,[EBP+0xC]       view list
    '8B 11 '                          # MOV EDX,[ECX]           -> vec4*
    '0F 28 02 '                       # MOVAPS XMM0,[EDX]
    '0F 5C C2 '                       # SUBPS XMM0,XMM2         view - body
    '0F 29 44 24 50 '                 # MOVAPS [ESP+0x50],XMM0
    '0F 57 C0 '                       # XORPS XMM0,XMM0
    'F3 0F 11 44 24 54 '              # MOVSS [ESP+0x54],XMM0   <-- dy := 0
    '0F 28 44 24 50 '                 # MOVAPS XMM0,[ESP+0x50]
    '0F 59 C0 '                       # MULPS XMM0,XMM0
    '0F 28 C8 '                       # MOVAPS XMM1,XMM0
    '0F C6 C8 39 '                    # SHUFPS XMM1,XMM0,0x39
    'F3 0F 58 C1 '                    # ADDSS XMM0,XMM1         dx2 + dy2
    '0F 28 D9 '                       # MOVAPS XMM3,XMM1
    '0F C6 D9 39 '                    # SHUFPS XMM3,XMM1,0x39
    'F3 0F 58 C3 '                    # ADDSS XMM0,XMM3         + dz2 (w out)
    '0F 29 44 24 60 '                 # MOVAPS [ESP+0x60],XMM0
    'F3 0F 51 C0 '                    # SQRTSS XMM0,XMM0
    'F3 0F 11 44 24 60 '              # MOVSS [ESP+0x60],XMM0
    '0F 28 44 24 60 '                 # MOVAPS XMM0,[ESP+0x60]
    '8D 54 24 38 '                    # LEA EDX,[ESP+0x38]
    'F3 0F 11 02 '                    # MOVSS [EDX],XMM0
    'F3 0F 10 05 FC 49 3A 00 '        # MOVSS XMM0,[0x003A49FC]  <-- 160.0
    '0F 2F 44 24 38 '                 # COMISS XMM0,[ESP+0x38]
    '77 0D '                          # JA 0x1A6564             cull if <160
    '8B 55 10 '                       # MOV EDX,[EBP+0x10]
    '40 '                             # INC EAX
    '83 C1 10 '                       # ADD ECX,0x10            next view
    '3B C2 '                          # CMP EAX,EDX
    '7C 96 '                          # JL 0x1A64F8
    'EB 07 '                          # JMP 0x1A656B            keep
    '8B CE '                          # MOV ECX,ESI             the new body
    'E8 35 10 00 00 '                 # CALL 0x001A75A0         destroy it
)

# FUN_001AA100 @0x001AA59A: where the list comes from.  `LEA EAX,[EBP+0x126948]`
# with EBP = 0x0060EA00 is &DAT_00735348, and the count is
# `*(DAT_0073552C + 0x3EC)`; the traffic manager itself is EBP+0x4DE0 =
# 0x006137E0, which is the address validate_traffic_mix already pins.
VIEW_LIST_BYTES = (
    '8B 8D 20 6B 12 00 '              # MOV ECX,[EBP+0x126B20]
    '33 C0 '                          # XOR EAX,EAX             count := 0
    '85 C9 '                          # TEST ECX,ECX
    '74 0C '                          # JZ  (leave count 0)
    '8B 95 2C 6B 12 00 '              # MOV EDX,[EBP+0x126B2C]
    '8B 82 EC 03 00 00 '              # MOV EAX,[EDX+0x3EC]     the view count
    '50 '                             # PUSH EAX
    '8D 85 48 69 12 00 '              # LEA EAX,[EBP+0x126948]  the view list
    '50 '                             # PUSH EAX
    '8D 8D E0 4D 00 00 '              # LEA ECX,[EBP+0x4DE0]    the manager
    '51 '                             # PUSH ECX
    'E8 EA 82 FF FF '                 # CALL 0x001A28B0
)

# FUN_001A3470 @0x001A3597: the same two words handed straight on to
# FUN_001A6070 (they are its param_4 / param_5).
THREAD_BYTES = (
    '8B 54 24 30 '                    # MOV EDX,[ESP+0x30]      count
    '8B 4C 24 2C '                    # MOV ECX,[ESP+0x2C]      list
    '8B 5C 24 28 '                    # MOV EBX,[ESP+0x28]
    '6A 01 '                          # PUSH 1                  param_6
    '52 '                             # PUSH EDX                param_5 count
    '8B 54 24 3C '                    # MOV EDX,[ESP+0x3C]
    '51 '                             # PUSH ECX                param_4 list
    '52 '                             # PUSH EDX                param_3
    '8B D6 '                          # MOV EDX,ESI
    'E8 BD 2A 00 00 '                 # CALL 0x001A6070
)

# FUN_001A3AE0 case 1 @0x001A3BA2: the STREAMING-SECTION load passes 0,0,0, so
# param_5 (the view count) is zero and the gate loop is skipped entirely -- a
# section that streams in ahead of the player fills freely.
STREAM_LOAD_BYTES = (
    '6A 00 '                          # PUSH 0                  param_5 = 0
    '6A 00 '                          # PUSH 0                  param_4 = 0
    '8D 14 40 '                       # LEA EDX,[EAX+EAX*2]
    '8B 03 '                          # MOV EAX,[EBX]
    '6A 00 '                          # PUSH 0                  param_3 = 0
    '8D 04 50 '                       # LEA EAX,[EAX+EDX*2]
    '55 '                             # PUSH EBP                the manager
    'E8 BA F8 FF FF '                 # CALL 0x001A3470
)

# FUN_001A75A0 -- the teardown the gate calls, and the same one the retire arm
# calls.  It sets body+0x17A bit 0, runs FUN_001A8C30, recurses on the trailer
# at +0x110 and ends in FUN_001A3970(1), the pool free.
DESTROY_BYTES = (
    '56 57 '                          # PUSH ESI / EDI
    '8B F9 '                          # MOV EDI,ECX             the body
    '8A 8F 7A 01 00 00 '              # MOV CL,[EDI+0x17A]
    '80 C9 01 '                       # OR CL,1
    '8B F7 '                          # MOV ESI,EDI
    '88 8F 7A 01 00 00 '              # MOV [EDI+0x17A],CL
    'E8 76 16 00 00 '                 # CALL 0x001A8C30
)

# FUN_001A3470 @0x001A38BA: the RETIRE arm calls the identical routine.
RETIRE_CALL_BYTES = 'E8 E1 3C 00 00 '          # CALL 0x001A75A0


def want(hexstr):
    return bytes(int(x, 16) for x in hexstr.split())


def test_image(img):
    got = img.f32(A_VIEW_CONST)
    check('A1 DAT_003A49FC == 160.0 (the view gate radius)',
          abs(got - VIEW_GATE_M) < 1e-6, 'got %r' % got)
    check('A1 DAT_003A49FC raw bytes are 0x43200000',
          img.read(A_VIEW_CONST, 4) == b'\x00\x00\x20\x43',
          hexs(img.read(A_VIEW_CONST, 4)))

    for name, va, hexstr in (
            ('A2 FUN_001A6070 @0x001A64E5 gate block', A_GATE_TOP, GATE_BYTES),
            ('A3 FUN_001AA100 @0x001AA59A view list + count', 0x001AA59A,
             VIEW_LIST_BYTES),
            ('A4 FUN_001A3470 @0x001A3597 list -> FUN_001A6070', 0x001A3597,
             THREAD_BYTES),
            ('A5 FUN_001A3AE0 @0x001A3BA2 section load passes count 0',
             0x001A3BA2, STREAM_LOAD_BYTES),
            ('A6 FUN_001A75A0 @0x001A75A0 body teardown', A_DESTROY,
             DESTROY_BYTES),
            ('A7 FUN_001A3470 @0x001A38BA retire calls FUN_001A75A0',
             0x001A38BA, RETIRE_CALL_BYTES)):
        expect = want(hexstr)
        got = img.read(va, len(expect))
        check('%s (%d bytes)' % (name, len(expect)), got == expect,
              'want %s\n       got  %s' % (hexs(expect), hexs(got)))

    # both CALL rel32s really land on FUN_001A75A0
    for site, name in ((0x001A6566, 'A8 gate call site 0x001A6566'),
                       (0x001A38BA, 'A8 retire call site 0x001A38BA')):
        rel, = struct.unpack('<i', img.read(site + 1, 4))
        check('%s -> FUN_001A75A0' % name, site + 5 + rel == A_DESTROY,
              'lands on 0x%08X' % (site + 5 + rel))

    # STRUCTURAL: the retire walker performs no distance computation at all.
    # SQRTSS is F3 0F 51; there is exactly one COMISS (0F 2F) in it and it is
    # register-to-register (a row index compare), not against a float literal.
    body = img.read(F_1A3470[0], F_1A3470[1] - F_1A3470[0])
    check('A9 FUN_001A3470 contains no SQRTSS -- retirement is not a distance',
          body.count(b'\xf3\x0f\x51') == 0,
          '%d found' % body.count(b'\xf3\x0f\x51'))
    check('A9 FUN_001A3470 never reads DAT_003A49FC',
          struct.pack('<I', A_VIEW_CONST) not in body)

    # and the placement path has exactly ONE square root: the view gate.
    body = img.read(F_1A6070[0], F_1A6070[1] - F_1A6070[0])
    check('A10 FUN_001A6070 has exactly one SQRTSS (the view gate)',
          body.count(b'\xf3\x0f\x51') == 1,
          '%d found' % body.count(b'\xf3\x0f\x51'))
    check('A10 FUN_001A6070 reads DAT_003A49FC exactly once',
          body.count(struct.pack('<I', A_VIEW_CONST)) == 1)


# --------------------------------------------- pop-OUT: the terminus rule
# FUN_001A6B40 @0x001A6D8A: retail DOES destroy a traffic car at the end of
# its descriptor, wherever it is -- there is no distance test anywhere near
# this -- but only when body+0x118, the SELECTED SUCCESSOR, is null.
TERMINUS_BYTES = (
    '38 58 46 '                      # CMP [EAX+0x46],BL       agent has a mode
    'F3 0F 2C 50 30 '                # CVTTSS2SI EDX,[EAX+0x30] (int)cursor
    '0F 84 03 03 00 00 '             # JZ  <full teardown>
    '8A 48 41 '                      # MOV CL,[EAX+0x41]       the path id
    '80 F9 FF '                      # CMP CL,0xFF
    '75 04 '                         # JNZ
    '33 C9 '                         # XOR ECX,ECX
    'EB 0C '                         # JMP
    '0F B6 C9 '                      # MOVZX ECX,CL
    '6B C9 4C '                      # IMUL ECX,ECX,0x4C
    '81 C1 2C EC 60 00 '             # ADD ECX,0x60EC2C        descriptor table
    '8B 09 '                         # MOV ECX,[ECX]
    '8B 49 10 '                      # MOV ECX,[ECX+0x10]      row count
    '49 '                            # DEC ECX                 rows - 1
    '3B D1 '                         # CMP EDX,ECX
    '7C 18 '                         # JL  <keep driving>
    '39 9F 18 01 00 00 '             # CMP [EDI+0x118],EBX     successor?
    '75 10 '                         # JNZ <keep driving>
    '8B CF '                         # MOV ECX,EDI
    'E8 D7 07 00 00 '                # CALL 0x001A75A0         DESTROY
)

# FUN_001A0750 @0x001A07A6: the selector's COLUMN is picked from TWO booleans.
#   agent+0x4C != 0 : col = ((flag != 0) - 1) & 2   ->  flag 0 -> 2, flag 1 -> 0
#   agent+0x4C == 0 : col = (flag == 0) * 2 + 1     ->  flag 0 -> 3, flag 1 -> 1
# Columns 2/3 are this track's lane changes; 0/1 are its junctions.
COLUMN_BYTES = (
    '8A 54 24 24 '                   # MOV DL,[ESP+0x24]       agent+0x4C
    '84 D2 '                         # TEST DL,DL
    '74 0D '                         # JZ  <0x4C == 0 arm>
    '33 D2 '                         # XOR EDX,EDX
    '84 C0 '                         # TEST AL,AL              flag
    '0F 95 C2 '                      # SETNZ DL
    '4A '                            # DEC EDX
    '83 E2 02 '                      # AND EDX,2               -> 2 or 0
    'EB 0B '                         # JMP
    '33 D2 '                         # XOR EDX,EDX
    '84 C0 '                         # TEST AL,AL
    '0F 94 C2 '                      # SETZ DL
    '8D 54 12 01 '                   # LEA EDX,[EDX+EDX*1+1]   -> 3 or 1
)

# FUN_001A09F0 @0x001A0A85: the agent's branch budget is WRITTEN FROM THE
# JUNCTION DATA, by the handler FUN_0019F1C0 runs on every row crossing.
BUDGET_BYTES = (
    'E8 06 5C 00 00 '                # CALL 0x001A6680
    '33 D2 '                         # XOR EDX,EDX
    '8A 53 4C '                      # MOV DL,[EBX+0x4C]
    '8B C8 '                         # MOV ECX,EAX
    '8B 44 24 10 '                   # MOV EAX,[ESP+0x10]
    '88 4B 48 '                      # MOV [EBX+0x48],CL       <-- the budget
)

# ...and the thing that is NOT the budget: FUN_001A20F0's per-LOCAL-VIEW
# record, built on its own stack.  A previous pass read `MOV [ESI+0x48],1`
# here as an agent field and pinned the branch budget to zero for the whole
# race, which made every traffic car die at its path terminus.
VIEWREC_BYTES = '8D B4 24 B0 04 00 00 '           # LEA ESI,[ESP+0x4B0]
VIEWBYTE_BYTES = (
    'A0 80 2D 75 00 '                # MOV AL,[0x00752D80]
    '84 C0 '                         # TEST AL,AL
    'C6 46 48 01 '                   # MOV byte [ESI+0x48],1   record+0x68
    '89 56 40 '                      # MOV [ESI+0x40],EDX
)
VIEWSTRIDE_BYTES = (
    '83 C7 30 '                      # ADD EDI,0x30
    '83 C6 70 '                      # ADD ESI,0x70            record stride
    '3B C1 '                         # CMP EAX,ECX
    '89 44 '                         # (MOV [ESP+0x14],EAX)
)
VIEWPARAM2_BYTES = (
    '56 '                            # PUSH ESI                the view count
    '8D 94 24 94 04 00 00 '          # LEA EDX,[ESP+0x494]     = ESI_0 - 0x20
    '52 '                            # PUSH EDX
    'E8 2B 45 00 00 '                # CALL 0x001A6B40
)

# FUN_001A28B0 @0x001A29B5: the window search writes the racer's remembered
# window byte ONLY on a hit; a miss leaves it alone and three windows are
# stamped regardless.  A racer never contributes nothing.
STICKY_BYTES = (
    '8B 4D 00 '                      # MOV ECX,[EBP]
    '8B B9 A8 00 00 00 '             # MOV EDI,[ECX+0xA8]      window count
    '33 D2 '                         # XOR EDX,EDX
    '85 FF '                         # TEST EDI,EDI
    '7E 26 '                         # JLE <no windows>
    '8B 89 A4 00 00 00 '             # MOV ECX,[ECX+0xA4]      window base
    '8D 9B 00 00 00 00 '             # LEA EBX,[EBX]           (align)
    '3B 41 08 '                      # CMP EAX,[ECX+0x8]       vs last
    '89 4C 24 18 '                   # MOV [ESP+0x18],ECX      every iteration
    '7F 05 '                         # JG  <next>
    '3B 41 04 '                      # CMP EAX,[ECX+0x4]       vs first
    '7D 0A '                         # JGE <hit>
    '42 '                            # INC EDX
    '83 C1 18 '                      # ADD ECX,0x18
    '3B D7 '                         # CMP EDX,EDI
    '7C EA '                         # JL  <loop>
    'EB 02 '                         # JMP <past the store>    MISS: keep it
    '88 16 '                         # MOV [ESI],DL            HIT: remember
)

# FUN_001A3470 @0x001A34AC: a request's DIRECTION byte, together with the
# manager's +0x363BC flag, decides whether the request STAMPS (and spawns) or
# UNSTAMPS (and retires).  With manager+0x363BC == 0, direction 0 RETIRES and
# directions 1 and 2 SPAWN.
DIRDISPATCH_BYTES = (
    '8A 40 05 '                      # MOV AL,[EAX+0x5]        direction
    '33 FF '                         # XOR EDI,EDI
    '66 8B 7B 04 '                   # MOV DI,[EBX+0x4]
    '89 4C 24 10 '                   # MOV [ESP+0x10],ECX
    '8D 14 37 '                      # LEA EDX,[EDI+ESI*1]
    '03 F9 '                         # ADD EDI,ECX
    '84 C0 '                         # TEST AL,AL              direction == 0?
    '75 0C '                         # JNZ <the 1/2 arm>
    '8B 4C 24 28 '                   # MOV ECX,[ESP+0x28]      the manager
    '38 81 BC 63 03 00 '             # CMP [ECX+0x363BC],AL
    '75 1E '                         # JNZ <STAMP>             363BC != 0
    '3C 01 '                         # CMP AL,1
    '74 08 '                         # JZ  <check 363BC>
    '3C 02 '                         # CMP AL,2
    '0F 85 37 01 00 00 '             # JNZ <UNSTAMP>
    '8B 44 24 28 '                   # MOV EAX,[ESP+0x28]
    '8A 88 BC 63 03 00 '             # MOV CL,[EAX+0x363BC]
    '84 C9 '                         # TEST CL,CL
    '0F 85 25 01 00 00 '             # JNZ <UNSTAMP>           363BC != 0
)


def test_popout_image(img):
    """The pop-OUT rules, byte for byte."""
    for name, va, hexstr in (
            ('F1 FUN_001A6B40 @0x001A6D8A terminus test', 0x001A6D8A,
             TERMINUS_BYTES),
            ('F2 FUN_001A0750 @0x001A07A6 four-column formula', 0x001A07A6,
             COLUMN_BYTES),
            ('F3 FUN_001A09F0 @0x001A0A75 writes the branch budget', 0x001A0A75,
             BUDGET_BYTES),
            ('F4 FUN_001A20F0 @0x001A2342 LEA ESI,[ESP+0x4B0] (a STACK record)',
             0x001A2342, VIEWREC_BYTES),
            ('F5 FUN_001A20F0 @0x001A2391 writes record+0x68, not an agent',
             0x001A2391, VIEWBYTE_BYTES),
            ('F6 FUN_001A20F0 @0x001A243B per-view record stride is 0x70',
             0x001A243B, VIEWSTRIDE_BYTES),
            ('F7 FUN_001A20F0 @0x001A2607 hands it to FUN_001A6B40 as param_2',
             0x001A2607, VIEWPARAM2_BYTES),
            ('F8 FUN_001A28B0 @0x001A29B5 sticky window index', 0x001A29B5,
             STICKY_BYTES),
            ('F9 FUN_001A3470 @0x001A34AC direction/363BC dispatch', 0x001A34AC,
             DIRDISPATCH_BYTES)):
        expect = want(hexstr)
        got = img.read(va, len(expect))
        check('%s (%d bytes)' % (name, len(expect)), got == expect,
              'want %s\n       got  %s' % (hexs(expect), hexs(got)))

    rel, = struct.unpack('<i', img.read(0x001A6DC4 + 1, 4))
    check('F1 the terminus call really is FUN_001A75A0',
          0x001A6DC4 + 5 + rel == A_DESTROY,
          'lands on 0x%08X' % (0x001A6DC4 + 5 + rel))
    rel, = struct.unpack('<i', img.read(0x001A2610 + 1, 4))
    check('F7 the per-view record goes to FUN_001A6B40',
          0x001A2610 + 5 + rel == 0x001A6B40,
          'lands on 0x%08X' % (0x001A2610 + 5 + rel))
    # no distance anywhere in the terminus decision
    body = img.read(0x001A6D8A, 0x001A6DC9 - 0x001A6D8A)
    check('F1 the terminus decision contains no SQRTSS and no float compare',
          b'\xf3\x0f\x51' not in body and b'\x0f\x2f' not in body
          and b'\x66\x0f\x2f' not in body)


# ====================================================== B. UNICORN
def model_gate(view, body):
    """The recovered rule: cull iff the HORIZONTAL distance < 160.0 m."""
    dx = struct.unpack('<f', struct.pack('<f', view[0] - body[0]))[0]
    dz = struct.unpack('<f', struct.pack('<f', view[2] - body[2]))[0]
    acc = struct.unpack('<f', struct.pack('<f', dx * dx))[0]
    acc = struct.unpack('<f', struct.pack('<f', acc + 0.0))[0]
    acc = struct.unpack('<f', struct.pack('<f',
                                          acc + struct.unpack(
                                              '<f', struct.pack('<f',
                                                                dz * dz))[0]))[0]
    import math
    dist = struct.unpack('<f', struct.pack('<f', math.sqrt(acc)))[0]
    return dist < VIEW_GATE_M, dist


def test_unicorn():
    """Run retail's own gate block; compare its verdict with the model.

    The block has two exits -- 0x001A6564 (`MOV ECX,ESI` / `CALL FUN_001A75A0`,
    the car is destroyed) and 0x001A656B (the loop tail, the car survives) --
    so a code hook stops at whichever it reaches.  Nothing past either exit is
    executed: the rest of FUN_001A6070 wants the whole traffic manager.
    """
    from unicorn import (Uc, UC_ARCH_X86, UC_MODE_32, UcError, UC_PROT_ALL,
                         UC_HOOK_MEM_UNMAPPED, UC_HOOK_CODE)
    from unicorn.x86_const import (UC_X86_REG_ESP, UC_X86_REG_EBP,
                                   UC_X86_REG_ESI)
    import emulate_vehicle as ev

    SCRATCH = 0x50000000
    uc = Uc(UC_ARCH_X86, UC_MODE_32)
    ev.load_elf(uc, ELF)
    uc.mem_map(SCRATCH, 0x10000, UC_PROT_ALL)
    faults = []
    exits = []

    def on_unmapped(u, access, address, size, value, user):
        faults.append(address)
        return False

    def on_code(u, address, size, user):
        if address in (A_GATE_CULL, A_GATE_KEEP):
            exits.append(address)
            u.emu_stop()

    uc.hook_add(UC_HOOK_MEM_UNMAPPED, on_unmapped)
    uc.hook_add(UC_HOOK_CODE, on_code)

    body = SCRATCH + 0x1000          # the freshly created traffic body
    views = SCRATCH + 0x2000         # the local view list, 16 bytes per view
    vpos = SCRATCH + 0x3000          # the vec4s the list points at
    stack = SCRATCH + 0x8000         # 16-byte aligned: MOVAPS [ESP+0x50]

    # Distances chosen to straddle the boundary, plus pure-Y separations that
    # the gate must ignore because 0x001A6505 zeroes that lane, and pure-W
    # separations it must ignore because the third ADDSS never reaches lane 3.
    cases = []
    for d in (0.0, 1.0, 25.0, 93.0, 129.0, 159.0, 159.9,
              160.0, 160.1, 161.0, 200.0, 420.0, 900.0):
        cases.append(([(d, 0.0, 0.0)], 1))              # pure +X
        cases.append(([(0.0, 0.0, -d)], 1))             # pure -Z
        cases.append(([(d * 0.6, 900.0, d * 0.8)], 1))  # 3-4-5, huge dy
    # two views: the car must be culled if EITHER is close (split screen)
    cases.append(([(500.0, 0.0, 0.0), (40.0, 0.0, 0.0)], 2))
    cases.append(([(500.0, 0.0, 0.0), (500.0, 0.0, 300.0)], 2))
    # a zero-length list is retail's streaming-section load: never culled
    cases.append(([(0.0, 0.0, 0.0)], 0))

    ok = 0
    for vecs, nview in cases:
        uc.mem_write(body + 0xA0, struct.pack('<4f', 0.0, 0.0, 0.0, 1.0))
        for i, off in enumerate(vecs):
            uc.mem_write(vpos + i * 0x10,
                         struct.pack('<4f', off[0], off[1], off[2], 7.0))
            uc.mem_write(views + i * 0x10,
                         struct.pack('<4I', vpos + i * 0x10, 0, 0, 0))
        # the synthetic FUN_001A6070 frame: [EBP+0xC] list, [EBP+0x10] count
        uc.mem_write(SCRATCH + 0x400C, struct.pack('<II', views, nview))
        uc.reg_write(UC_X86_REG_EBP, SCRATCH + 0x4000)
        uc.reg_write(UC_X86_REG_ESP, stack)
        uc.reg_write(UC_X86_REG_ESI, body)
        del faults[:]
        del exits[:]
        err = None
        try:
            uc.emu_start(A_GATE_TOP, 0, count=20000)
        except UcError as exc:
            err = str(exc)
        culled = exits[:1] == [A_GATE_CULL]
        if nview == 0:
            wantcull, dist = False, float('inf')
        else:
            verdicts = [model_gate(v, (0.0, 0.0, 0.0)) for v in vecs]
            wantcull = any(v[0] for v in verdicts)
            dist = min(v[1] for v in verdicts)
        if check('B nviews=%d d=%.1f -> %s' % (nview, dist,
                                               'CULL' if wantcull else 'keep'),
                 culled == wantcull and not faults and err is None
                 and len(exits) == 1,
                 'retail exits %r err %r faults %r'
                 % ([hex(e) for e in exits], err, faults)):
            ok += 1
    return ok


# ====================================================== C. PORT SOURCE
def test_port_source():
    try:
        src = open(FULL_C, encoding='utf-8').read()
    except OSError as exc:
        check('C burnout3_full.c readable', False, str(exc))
        return
    m = re.search(r'#define\s+B3_TRAFFIC_VIEW_GATE_M\s+([0-9.]+)f', src)
    check('C1 the port carries the 160 m gate constant',
          m is not None and abs(float(m.group(1)) - VIEW_GATE_M) < 1e-6,
          'found %s' % (m.group(1) if m else None))
    check('C2 the gate cites DAT_003A49FC',
          'DAT_003A49FC' in src or '0x003A49FC' in src)
    check('C3 the gate is a strict `< B3_TRAFFIC_VIEW_GATE_M` on the '
          'HORIZONTAL distance',
          re.search(r'sqrtf\(dx \* dx \+ dz \* dz\) < B3_TRAFFIC_VIEW_GATE_M',
                    src) is not None)
    # it has to be applied where retail applies it: in the pool placement,
    # after the reservation is stamped, releasing through the same path as a
    # retire (the port's FUN_001A75A0).
    m = re.search(r'static void traffic_pool_place\(.*?\n\}\n', src, re.S)
    check('C4 traffic_pool_place found', m is not None)
    if m:
        place = m.group(0)
        check('C5 traffic_pool_place applies the view gate',
              'traffic_view_gate_reject(traffic)' in place)
        check('C6 a rejected car goes back through the release path',
              re.search(r'traffic_view_gate_reject\(traffic\)\)\s*\{\s*'
                        r'g_pool_release_why = "view-gate";\s*'
                        r'traffic_pool_release_slot\(physical_slot\);',
                        place) is not None)
        check('C7 the gate sits AFTER the draws, so the RNG stream is '
              'unchanged (validate_traffic_mix stays valid)',
              'traffic_view_gate_reject' in place
              and place.index('traffic_pool_seed_at')
              < place.index('traffic_view_gate_reject'))
    # the GLUE watchdogs must not retire pool bodies -- retail has no radius
    # and no stall rule; c6275b3 confined the 420 m cull, this pins the rest.
    check('C8 the 420 m range cull is confined to the legacy lane path',
          re.search(r'if \(!raw_path\) \{[^}]*?420\.0f \* 420\.0f', src,
                    re.S) is not None)
    check('C9 the stall recycle is confined to the legacy lane path',
          re.search(r'if \(!raw_path && t->crashed_until <= g_race_time'
                    r' && t->speed < 1\.0f\)', src) is not None)
    # the pop trace has to watch `active` (what traffic_render() reads), not
    # `streamed` (which it does not).
    check('C10 the pop trace logs the pool release, i.e. the visible pop',
          re.search(r'traffic_pool_release_slot\(int slot\).*?'
                    r'traffic_pop_log\(traffic, tag\)', src, re.S) is not None)
    check('C11 traffic_render() still gates on `active` only '
          '(so `streamed` is not visibility)',
          re.search(r'static void traffic_render\(void\) \{.*?'
                    r'if \(!t->active\) continue;', src, re.S) is not None)

    # ---- the pop-OUT half ------------------------------------------------
    check('C12 the branch budget is no longer pinned by a race-mode constant '
          '(FUN_001A20F0 @0x001A2342 writes a per-VIEW stack record, not an '
          'agent field)',
          'B3_TRAFFIC_RACE_MODE' not in src)
    check('C13 the budget is re-armed per ROW CROSSING, as FUN_0019F1C0 -> '
          'FUN_001A09F0 @0x001A0A85 does',
          'branch_row_armed' in src
          and 'B3_TRAFFIC_BRANCH_ATTEMPTS_PER_ROW' in src)
    check('C14 the terminus asks the JUNCTION columns before giving up',
          'traffic_branch_select_terminus' in src
          and 'B3_BRANCH_JUNCTION_COL_A' in src
          and 'B3_BRANCH_JUNCTION_COL_B' in src)
    m = re.search(r'if \(traffic_path_advance\(t, t->speed \* dt\)\) \{'
                  r'(.*?)\n            \}', src, re.S)
    check('C15 the terminus still RELEASES when no successor exists -- '
          'FUN_001A6B40 @0x001A6DC4 destroys it, and that is a pop retail '
          'itself exhibits in view',
          m is not None and 'traffic_branch_select_terminus' in m.group(1)
          and '"path-end"' in m.group(1),
          'terminus block not recognised')
    m = re.search(r'static int traffic_pool_window_step\(.*?\n\}\n', src,
                  re.S)
    check('C16 the window advance is retail\'s +-1 stepper (FUN_001A33B0), '
          'not a jump -- one window per pass, and it holds when no window '
          'contains the progress',
          m is not None
          and '(*window + 1) % count' in m.group(0)
          and '(*window + count - 1) % count' in m.group(0)
          and 'if (target == count) return 0;' in m.group(0)
          and 'if (target == *window) return 0;' in m.group(0))
    check('C17 the owner is never dropped: the byte is held when the '
          'progress query fails',
          re.search(r'g_pool_trace_nowindow\+\+;\s*\}\s*'
                    r'sticky\[owner\] = \(unsigned char\)current;', src)
          is not None)

    # ---- the direction split -------------------------------------------
    check('C18 the port carries the manager\'s +0x363BC travel-sense flag, '
          'initialised to 1 as FUN_001A3EA0 @0x001A3F1F does',
          re.search(r'static int g_traffic_pool_forward = 1;', src) is not None)
    check('C19 the flag is FUN_001A3110\'s crossing direction, taken from '
          'the stepper rather than recomputed from two indices that a wrap '
          'makes ambiguous',
          'traffic_pool_forward_update(stepped_forward)' in src
          and re.search(r'g_traffic_pool_forward = forward;', src) is not None)
    m = re.search(r'static int traffic_request_stamps\(.*?\n\}', src, re.S)
    check('C20 the request dispatch is retail\'s truth table', m is not None
          and 'request->direction == 0' in m.group(0)
          and 'g_traffic_pool_forward != 0' in m.group(0)
          and 'g_traffic_pool_forward == 0' in m.group(0)
          and 'return 0;' in m.group(0))
    m = re.search(r'static void traffic_request_unstamp\(unsigned int '
                  r'request_index\).*?\n\}', src, re.S)
    check('C21 a non-stamping request RETIRES its row range '
          '(FUN_001A3470 @0x001A3611)',
          m is not None
          and 'traffic_pool_release_slot(slot)' in m.group(0)
          and '"unstamp"' in m.group(0)
          and re.search(r'if \(!traffic_request_stamps\(request\)\)', src)
          is not None)
    # C22 CORRECTED.  The row order is NOT a travel sense.  FUN_001A6070
    # @0x001A6098 min/maxes first_row/last_row before it does anything else,
    # so the order reaches nothing in retail, and FUN_0019F1C0 @0x0019F21D --
    # the whole traffic mover -- only ADDS to the agent's row cursor and only
    # INCREMENTS its row: there is no reverse agent to give a sense to.  The
    # opposition is in the DIRECTED path network instead.  Reading the order
    # as "drive this one backwards" turned around ~70% of the oncoming
    # carriageway's agents (the requests on a road that runs against the race
    # are authored descending) and the player saw traffic running his way on
    # both sides of the road.  The bit is still read -- it picks the
    # LANE-CHANGE link column, FUN_001A0750's agent+0x4C -- but it must not
    # reach the travel sense.
    m = re.search(r'static int traffic_pool_seed_at\(.*?\n\}', src, re.S)
    check('C22 the row order does NOT become a travel sense: every agent '
          'walks its descriptor forward (FUN_0019F1C0 @0x0019F21D)',
          m is not None
          and re.search(r't->path_dir\s*=\s*1\s*;', m.group(0)) is not None
          and re.search(r'path_dir\s*=\s*\(?\s*signed char\s*\)?\s*\(?'
                        r'\s*\w*reverse\w*\s*\?', m.group(0)) is None)
    m = re.search(r'static int traffic_path_advance\(.*?\n\}', src, re.S)
    check('C22b the mover has one direction: no agent is walked DOWN its '
          'descriptor rows',
          m is not None and re.search(r'path_dir\s*<\s*0', m.group(0)) is None)
    check('C22c the row-order bit is still read, as the LANE-CHANGE column '
          '(agent+0x4C, FUN_001A0750 @0x001A07A0)',
          re.search(r'request->first_row\s*\n?\s*> request->last_row',
                    src) is not None
          or 'request->first_row > request->last_row' in src)
    check('C23a the unstamp sweep is DEFERRED past the stamping walk and '
          'spares anything claimed in the same pass',
          'traffic_request_unstamp_flush' in src
          and re.search(r'if \(traffic->pool_seen\) continue;', src)
          is not None
          and re.search(r'g_pool_unstamp\[g_pool_unstamp_n\+\+\] = '
                        r'request_index;', src) is not None)
    m = re.search(r'static int traffic_pool_progress\(.*?\n\}\n', src, re.S)
    check('C24 the pool progress is derived from the car\'s POSITION, not '
          'from the AI ribbon cursor', m is not None
          and 'row->flags & 1u' in m.group(0)
          and 'vehicle->pos' in m.group(0)
          and 'B3_POOL_PROGRESS_WINDOW' in m.group(0))
    check('C25 it still falls back to the cursor on a graph with no main '
          'rows', m is not None and 'vehicle->nav_section' in m.group(0))
    check('C26 the pop trace carries the window and progress the pool was '
          'using, so an in-view retire can be attributed',
          'g_pool_dbg_window' in src and 'g_pool_dbg_progress' in src
          and 'win=%u prog=%u' in src)
    m = re.search(r'static int traffic_pool_window_step\(.*?\n\}\n', src,
                  re.S)
    check('C27 the window stepper picks its direction by CIRCULAR distance, '
          'so the lap wrap is one forward step',
          m is not None and 'fwd = (target - *window + count) % count' in m.group(0)
          and 'back = (*window - target + count) % count' in m.group(0)
          and 'out_forward' in m.group(0))
    m = re.search(r'static void traffic_pool_forward_update\(.*?\n\}\n', src,
                  re.S)
    check('C28 the travel-sense flag needs TWO crossings the same way before '
          'it inverts the dispatch',
          m is not None and 'pending' in m.group(0)
          and re.search(r'if \(pending != forward\)', m.group(0)) is not None)
    check('C29 the pop trace carries the flag, because an in-view retire is '
          'not explicable without it', 'fwd=%d' in src)
    check('C30 the pool is refreshed for the LOCAL VIEWS only, not for every '
          'car in the race (FUN_001A3EA0 @0x001A3ED9 keeps two window bytes)',
          re.search(r'owner < g_num_vehicles && owner < '
                    r'B3_TRAFFIC_LOCAL_VIEWS', src) is not None
          and re.search(r'#define B3_TRAFFIC_LOCAL_VIEWS\s+1', src) is not None)
    check('C31 the per-racer window state is sized like the manager\'s',
          re.search(r'static unsigned char sticky\[2\];', src) is not None)
    check('C23 the not-seen sweep is now a BACKSTOP that can never fire in '
          'view',
          re.search(r'!traffic->pool_seen\s*&&\s*'
                    r'traffic_view_gate_reject\(traffic\) == 0', src)
          is not None
          and '"backstop"' in src)


# ====================================================== D. MODEL REPLAY
class Rng(object):
    """FUN_00048760, seeded by FUN_001A3EA0 @0x001A3EA7/@0x001A3EB1."""

    def __init__(self, state=0xFD462907, carry=0x02B9D6F8):
        self.state = state
        self.carry = carry

    def u32(self):
        high = (self.state >> 16) & 0xFFFF
        if high >= 0x8000:
            high -= 0x10000
        nxt = ((self.state << 16) + high + self.carry) & 0xFFFFFFFF
        self.carry = (self.carry + nxt) & 0xFFFFFFFF
        self.state = nxt
        return nxt

    def f(self):
        return float(self.u32()) * (1.0 / 4294967296.0)


def load_paths(track):
    path = os.path.join(REPO, 'build', 'tracks', track, 'traffic_paths.bin')
    data = open(path, 'rb').read()
    magic, version, point_count, path_count = struct.unpack_from('<4sIII', data)
    assert magic == b'B3TP', magic
    if version < 4:
        return None
    window_count, request_count = struct.unpack_from('<II', data, 16)
    off = 24
    points = [struct.unpack_from('<3f', data, off + i * 12)
              for i in range(point_count)]
    off += point_count * 12
    paths = []
    for _ in range(path_count):
        (count,) = struct.unpack_from('<I', data, off)
        off += 4
        pairs = [struct.unpack_from('<HH', data, off + i * 4)
                 for i in range(count)]
        off += count * 4
        dist = struct.unpack_from('<%df' % (count * 2), data, off)[0::2]
        off += count * 8
        off += count * 0x12
        paths.append((pairs, dist))
    windows = [struct.unpack_from('<IIIBBH', data, off + i * 16)
               for i in range(window_count)]
    off += window_count * 16
    requests = [struct.unpack_from('<HHBB', data, off + i * 6)
                for i in range(request_count)]
    off += request_count * 6
    classes_n, entries_n, bindings_n, roads_n = struct.unpack_from(
        '<IIII', data, off)
    off += 16 + classes_n * 16 + entries_n * 32
    bindings = [struct.unpack_from('<BBBBI', data, off + i * 8)
                for i in range(bindings_n)]
    off += bindings_n * 8
    roads = {}
    for i in range(roads_n):
        row = struct.unpack_from('<BBHf6f', data, off + i * 32)
        roads[(row[0], row[1])] = (row[3], list(row[4:]))
    return points, paths, windows, requests, bindings, roads


def midpoint(points, pairs, row):
    a = points[pairs[row][0]]
    b = points[pairs[row][1]]
    return ((a[0] + b[0]) * 0.5, (a[1] + b[1]) * 0.5, (a[2] + b[2]) * 0.5)


def test_replay():
    track = os.environ.get('B3_TRACK',
                           os.environ.get('B3_POSTFX_TRACK', 'US_C3_V1'))
    loaded = load_paths(track)
    if loaded is None:
        print('  traffic_paths.bin is pre-v4: no spawn policy to replay')
        return
    points, paths, windows, requests, bindings, roads = loaded
    by_path = {}
    for path_id, record, slot, _pad, start in bindings:
        by_path.setdefault(path_id, []).append((start, record, slot))

    def owner(path_id, row):
        best = None
        for start, record, slot in by_path.get(path_id, []):
            if start <= row and (best is None or start > best[0]):
                best = (start, record, slot)
        return best

    def nxt(path_id, row):
        best = None
        for start, _r, _s in by_path.get(path_id, []):
            if start > row and (best is None or start < best):
                best = start
        return best

    import math
    rng = Rng()
    bitten = 0
    starved = []
    total_placed = total_culled = 0
    for w, (_first, _last, base, count, _refresh, _pad) in enumerate(windows):
        # the player is inside the CURRENT window by definition: stand it on
        # the middle row of that window's first request.
        if count == 0:
            continue
        first_row, last_row, path_id, _dir = requests[base]
        pairs, dist = paths[path_id]
        lo, hi = min(first_row, last_row), max(first_row, last_row)
        view = midpoint(points, pairs, (lo + hi) // 2)
        placed = culled = 0
        group = [(w - k) % len(windows) for k in range(3)]
        for wi in group:
            _f, _l, gbase, gcount, _r, _p = windows[wi]
            for index in range(gcount):
                first_row, last_row, path_id, _dir = requests[gbase + index]
                pairs, dist = paths[path_id]
                lo, hi = min(first_row, last_row), max(first_row, last_row)
                if hi >= len(pairs):
                    continue
                row = lo
                while True:
                    own = owner(path_id, row)
                    nx = nxt(path_id, row)
                    end = hi if (nx is None or nx - 1 >= hi) else nx - 1
                    road = roads.get((own[1], own[2])) if own else None
                    if road:
                        mph, rate = road
                        rate_sum = math.fsum(rate[:6])
                        speed = mph * 0.44704
                        span = abs(dist[end] - dist[row])
                        if rate_sum > 0.0 and speed > 0.0 and span > 0.0:
                            n_f = (span / speed) * (1.0 / 60.0) * rate_sum
                            if n_f >= 0.5:
                                spacing = span / n_f
                                n = int(n_f + 0.5)
                                for i in range(n):
                                    step = rng.f() * spacing
                                    r = row + i * step
                                    r += r * (2.0 * rng.f() - 1.0) * 0.30
                                    r = max(float(row), min(float(end), r))
                                    p = midpoint(points, pairs, int(r))
                                    placed += 1
                                    dx = p[0] - view[0]
                                    dz = p[2] - view[2]
                                    if math.sqrt(dx * dx + dz * dz) < VIEW_GATE_M:
                                        culled += 1
                    if end >= hi:
                        break
                    row = end + 1
        total_placed += placed
        total_culled += culled
        if culled:
            bitten += 1
        if placed and placed == culled:
            starved.append(w)
    check('D1 the gate bites: some window group has placements inside 160 m',
          bitten > 0, '%d of %d window groups' % (bitten, len(windows)))
    check('D2 no window group is emptied by the gate',
          not starved, 'starved windows %r' % starved[:8])
    check('D3 the gate culls a minority of placements '
          '(a runaway radius would not)',
          total_placed > 0 and total_culled < total_placed * 0.5,
          '%d of %d culled' % (total_culled, total_placed))
    print('  replay: %d placements over %d window groups, %d (%.1f%%) inside '
          'retail\'s 160 m view gate; %d groups affected'
          % (total_placed, len(windows), total_culled,
             100.0 * total_culled / max(1, total_placed), bitten))


# ============================ G. THE LINK TABLE (the terminus's way out)
def test_links():
    """Every handover is one road row, and the junctions live in columns 0/1.

    FUN_001A0750 reaches four columns; this harness used to reach only two.
    On this track columns 2 and 3 are the opposite-carriageway lane changes
    and columns 0 and 1 are the road network's junctions -- including the
    three that sit on a path's LAST row, which are the only thing standing
    between a car and FUN_001A6B40 @0x001A6DC4.
    """
    track = os.environ.get('B3_TRACK',
                           os.environ.get('B3_POSTFX_TRACK', 'US_C3_V1'))
    path = os.path.join(REPO, 'build', 'tracks', track, 'traffic_paths.bin')
    data = open(path, 'rb').read()
    magic, version, point_count, path_count = struct.unpack_from('<4sIII', data)
    if magic != b'B3TP' or version < 2:
        print('  traffic_paths.bin has no link section')
        return
    off = 24 if version >= 3 else 16
    points = [struct.unpack_from('<3f', data, off + i * 12)
              for i in range(point_count)]
    off += point_count * 12
    paths = []
    for _ in range(path_count):
        (n,) = struct.unpack_from('<I', data, off)
        off += 4
        pairs = [struct.unpack_from('<HH', data, off + i * 4) for i in range(n)]
        off += n * 4 + n * 8
        links = []
        for i in range(n):
            raw = data[off + i * 18:off + i * 18 + 18]
            links.append((struct.unpack_from('<4H', raw, 0), list(raw[12:16])))
        off += n * 18
        paths.append((pairs, links))

    import math

    def mid(pi, r):
        a = points[paths[pi][0][r][0]]
        b = points[paths[pi][0][r][1]]
        return ((a[0] + b[0]) * .5, (a[1] + b[1]) * .5, (a[2] + b[2]) * .5)

    per_col = [0, 0, 0, 0]
    worst = 0.0
    worst_dy = 0.0
    junction = []
    for pi, (pairs, links) in enumerate(paths):
        n = len(pairs)
        for r, (rows, tp) in enumerate(links):
            for col in range(4):
                dst = tp[col]
                if dst == 0xFF or dst >= path_count:
                    continue
                tgt = rows[col]
                if tgt >= len(paths[dst][0]):
                    continue
                per_col[col] += 1
                a, b = mid(pi, r), mid(dst, tgt)
                d = math.hypot(b[0] - a[0], b[2] - a[2])
                worst = max(worst, d)
                worst_dy = max(worst_dy, abs(b[1] - a[1]))
                if col in (0, 1):
                    junction.append((col, pi, r, n - 1, dst, tgt))
    check('G1 columns 2 and 3 carry the bulk of the links (lane changes)',
          per_col[2] > 100 and per_col[3] > 100,
          'per column %r' % per_col)
    check('G2 columns 0 and 1 carry the junctions, and they are NOT empty '
          '-- a selector that never reads them cannot leave a spur',
          per_col[0] + per_col[1] > 0, 'per column %r' % per_col)
    # a handover must look like driving, not like teleporting
    check('G3 every link is one road row: max horizontal jump <= 10 m',
          worst <= 10.0, 'worst %.2f m' % worst)
    check('G4 every link is level: max |dy| <= 1 m', worst_dy <= 1.0,
          'worst %.2f m' % worst_dy)
    terminal = [j for j in junction if j[2] == j[3]]
    check('G5 at least one junction sits on a path TERMINUS (that is the '
          'car FUN_001A6B40 @0x001A6DC4 would otherwise destroy)',
          len(terminal) > 0, '%d of %d junctions' % (len(terminal),
                                                     len(junction)))
    # no terminal row may need BOTH junction columns, or "try 1 then 0" would
    # be able to pick the wrong one
    ambiguous = 0
    for pi, (pairs, links) in enumerate(paths):
        n = len(pairs)
        for r in (0, n - 1):
            tp = links[r][1]
            if tp[0] != 0xFF and tp[1] != 0xFF:
                ambiguous += 1
    check('G6 no terminal row populates BOTH junction columns, so trying '
          'col 1 then col 0 reaches exactly retail\'s link',
          ambiguous == 0, '%d ambiguous terminal rows' % ambiguous)
    print('  links: per column %r, worst jump %.2f m, worst |dy| %.2f m, '
          '%d junctions (%d on a terminus)'
          % (per_col, worst, worst_dy, len(junction), len(terminal)))


# ================================ H. THE RECOVERED-BUT-NOT-IMPLEMENTED RULE
def test_direction_census():
    """FUN_001A3470's direction dispatch, measured on the shipped requests.

    Retail sends a request to the STAMP arm or the UNSTAMP arm by its
    direction byte; with manager+0x363BC == 1 -- the value FUN_001A3EA0
    @0x001A3F1F installs and the one a racer driving forwards keeps -- that is
    `0 -> spawn`, `1 or 2 -> retire`.  Pinned here with the shipped census.
    """
    track = os.environ.get('B3_TRACK',
                           os.environ.get('B3_POSTFX_TRACK', 'US_C3_V1'))
    loaded = load_paths(track)
    if loaded is None:
        return
    _points, _paths, windows, requests, _bindings, _roads = loaded
    census = {}
    for _f, _l, _p, direction in requests:
        census[direction] = census.get(direction, 0) + 1
    check('H1 the direction byte really takes three values, so it cannot be '
          'a plain forward/reverse flag',
          len(census) == 3 and set(census) == {0, 1, 2}, 'census %r' % census)
    check('H2 both arms are populated: retail spawns from some requests and '
          'retires with others',
          census.get(0, 0) > 0 and census.get(1, 0) + census.get(2, 0) > 0,
          'census %r' % census)
    # every row a spawn request covers should also be covered by a retire
    # request in a DIFFERENT window -- that separation is what keeps retail's
    # retires away from the player.
    spawn_w = {}
    retire_w = {}
    for w, (_f, _l, base, count, _r, _p) in enumerate(windows):
        for i in range(count):
            fr, lr, pp, direction = requests[base + i]
            lo, hi = min(fr, lr), max(fr, lr)
            table = spawn_w if direction == 0 else retire_w
            table.setdefault(pp, []).append((lo, hi, w))
    gaps = []
    for pp, spans in spawn_w.items():
        for lo, hi, w in spans:
            for rlo, rhi, rw in retire_w.get(pp, []):
                if rlo <= lo <= rhi or rlo <= hi <= rhi:
                    gaps.append(abs(rw - w))
    check('H3 a row\'s spawn request and its retire request are in '
          'DIFFERENT windows -- that separation is why retail\'s retires are '
          'never in shot',
          gaps and min(gaps) > 0,
          'window separations seen: %r' % sorted(set(gaps))[:8])
    print('  requests: %r; spawn/retire window separation min %d median %d '
          'max %d' % (census, min(gaps), sorted(gaps)[len(gaps) // 2],
                      max(gaps)))


# ================== I. THE DIRECTION SPLIT AND ITS TRAVEL-SENSE FLAG
# FUN_001A3EA0 @0x001A3F1F -- the manager CONSTRUCTOR sets +0x363BC to 1, in
# the same function that seeds the traffic RNG.
CTOR_FORWARD_BYTES = (
    'B1 63 03 00 '                   # (tail of `MOV byte [EAX+0x363B1],0`)
    'C6 80 BC 63 03 00 01 '          # MOV byte [EAX+0x363BC],1
)
# FUN_001A3110 @0x001A313C -- and the only other writer makes it `old < new`,
# i.e. the racer's direction of travel through the window sequence.
FORWARD_DECIDE_BYTES = (
    '8A 44 24 18 '                   # MOV AL,[ESP+0x18]        the OLD window
    '8B 91 A8 00 00 00 '             # MOV EDX,[ECX+0xA8]       window count
    '0F B6 F0 '                      # MOVZX ESI,AL
    '4A '                            # DEC EDX                  count - 1
    '3B F2 '                         # CMP ESI,EDX
    '74 16 '                         # JZ  <wrap arm>
    '84 C0 '                         # TEST AL,AL
    '74 12 '                         # JZ  <wrap arm>
    '3A 84 2B A7 63 03 00 '          # CMP AL,[EBX+EBP+0x363A7] old vs NEW
    '0F 92 C0 '                      # SETC AL                  old < new
    '88 85 BC 63 03 00 '             # MOV [EBP+0x363BC],AL
    'EB 1F '                         # JMP
    '8A 84 2B A7 63 03 00 '          # MOV AL,[EBX+EBP+0x363A7] the NEW window
    '84 C0 '                         # TEST AL,AL
    '74 0D '                         # JZ  -> 1
    '3C 01 '                         # CMP AL,1
    '74 09 '                         # JZ  -> 1
    'C6 85 BC 63 03 00 00 '          # MOV byte [EBP+0x363BC],0
    'EB 07 '                         # JMP
    'C6 85 BC 63 03 00 01 '          # MOV byte [EBP+0x363BC],1
)
# FUN_001A33B0 -- the window ADVANCE: a +-1 step off the racer's remembered
# byte, never a search, and never "no window".
STEPPER_BYTES = (
    '51 '                            # PUSH ECX
    '8B 44 24 0C '                   # MOV EAX,[ESP+0xC]        the manager
    '8A 8C 30 A7 63 03 00 '          # MOV CL,[EAX+ESI+0x363A7] the racer byte
    '8B 16 '                         # MOV EDX,[ESI]
    '53 '                            # PUSH EBX
    '8B 9A A4 00 00 00 '             # MOV EBX,[EDX+0xA4]       window base
    '0F B6 C1 '                      # MOVZX EAX,CL
    '55 57 '                         # PUSH EBP / EDI
    '8D 3C 40 '                      # LEA EDI,[EAX+EAX*2]
    '8D 3C FB '                      # LEA EDI,[EBX+EDI*8]      &windows[w]
    '89 7C 24 0C '                   # MOV [ESP+0xC],EDI
    '8B BA A8 00 00 00 '             # MOV EDI,[EDX+0xA8]       count
    '8D 6F FF '                      # LEA EBP,[EDI-1]          count - 1
    '3B C5 '                         # CMP EAX,EBP              w == count-1?
)


def test_direction_split(img):
    for name, va, hexstr in (
            ('I1 FUN_001A3EA0 @0x001A3F1F ctor sets +0x363BC = 1', 0x001A3F1B,
             CTOR_FORWARD_BYTES),
            ('I2 FUN_001A3110 @0x001A313C +0x363BC = (old < new)', 0x001A313C,
             FORWARD_DECIDE_BYTES),
            ('I3 FUN_001A33B0 @0x001A33B0 window advance is a +-1 step',
             0x001A33B0, STEPPER_BYTES)):
        expect = want(hexstr)
        got = img.read(va, len(expect))
        check('%s (%d bytes)' % (name, len(expect)), got == expect,
              'want %s\n       got  %s' % (hexs(expect), hexs(got)))

    # the flag has no other writer anywhere in the image, so nothing else can
    # flip the dispatch: six manager-relative accesses, two of them the reads
    # in FUN_001A3470, and no absolute reference to 0x00649B9C at all.
    disp = struct.pack('<I', 0x363BC)
    sites = []
    for base, off, size in img.segs:
        i = img.data.find(disp, off, off + size)
        while i != -1:
            sites.append(base + (i - off))
            i = img.data.find(disp, i + 1, off + size)
    check('I4 exactly six manager+0x363BC accesses exist image-wide',
          len(sites) == 6, '%r' % [hex(x) for x in sites])
    check('I5 they are the ctor, the three FUN_001A3110 writes and the two '
          'FUN_001A3470 reads -- nothing else can move the flag',
          sorted(sites) == [0x1A315E, 0x1A3175, 0x1A317E, 0x1A34C8, 0x1A34E0,
                            0x1A3F21],
          '%r' % [hex(x) for x in sorted(sites)])
    absref = struct.pack('<I', 0x00649B9C)          # manager 0x6137E0 + 0x363BC
    hits = 0
    for base, off, size in img.segs:
        i = img.data.find(absref, off, off + size)
        while i != -1:
            hits += 1
            i = img.data.find(absref, i + 1, off + size)
    check('I6 nothing addresses the flag absolutely either', hits == 0,
          '%d hits' % hits)


# ================== J. WHERE THE SPLIT PUTS RETAIL'S RETIRES
def test_split_geometry():
    """Spawn and retire are different requests, a long way apart.

    That separation -- not a distance rule -- is retail's answer to "never
    despawn in shot".  Measured over the shipped table with the spawn arm's own
    rows as the stand-in for where the player is when a window is current.
    """
    track = os.environ.get('B3_TRACK',
                           os.environ.get('B3_POSTFX_TRACK', 'US_C3_V1'))
    loaded = load_paths(track)
    if loaded is None:
        return
    points, paths, windows, requests, _bindings, _roads = loaded
    import math

    def mid(pi, r):
        a = points[paths[pi][0][r][0]]
        b = points[paths[pi][0][r][1]]
        return ((a[0] + b[0]) * .5, (a[2] + b[2]) * .5)

    def rows_of(q):
        fr, lr, pp, _d = q
        return pp, min(fr, lr), min(max(fr, lr), len(paths[pp][0]) - 1)

    count = len(windows)
    dists = []
    for w in range(count):
        spawn, retire = [], []
        for g in ((w - k) % count for k in range(3)):
            _f, _l, base, cnt, _r, _p = windows[g]
            for i in range(cnt):
                q = requests[base + i]
                (spawn if q[3] == 0 else retire).append(rows_of(q))
        if not spawn or not retire:
            continue
        xs, zs = [], []
        for pp, lo, hi in spawn:
            for r in range(lo, hi + 1, max(1, (hi - lo) // 10 or 1)):
                m = mid(pp, r)
                xs.append(m[0])
                zs.append(m[1])
        cx, cz = sum(xs) / len(xs), sum(zs) / len(zs)
        best = None
        for pp, lo, hi in retire:
            for r in range(lo, hi + 1):
                m = mid(pp, r)
                d = math.hypot(m[0] - cx, m[1] - cz)
                if best is None or d < best:
                    best = d
        dists.append(best)
    med = sorted(dists)[len(dists) // 2]
    inside = sum(1 for d in dists if d < VIEW_GATE_M)
    check('J1 the retire arm is far from the road the spawn arm fills '
          '(median >= 300 m)', med >= 300.0, 'median %.1f m' % med)
    check('J2 hardly any window group has a retire row inside the 160 m view '
          'gate', inside <= 4, '%d of %d groups' % (inside, len(dists)))
    # the inverted flag would put them on top of each other -- that is the
    # check that pins the POLARITY, not just the separation
    inv = []
    for w in range(count):
        spawn, retire = [], []
        for g in ((w - k) % count for k in range(3)):
            _f, _l, base, cnt, _r, _p = windows[g]
            for i in range(cnt):
                q = requests[base + i]
                (retire if q[3] == 0 else spawn).append(rows_of(q))
        if not spawn or not retire:
            continue
        xs, zs = [], []
        for pp, lo, hi in spawn:
            for r in range(lo, hi + 1, max(1, (hi - lo) // 10 or 1)):
                m = mid(pp, r)
                xs.append(m[0])
                zs.append(m[1])
        cx, cz = sum(xs) / len(xs), sum(zs) / len(zs)
        best = None
        for pp, lo, hi in retire:
            for r in range(lo, hi + 1):
                m = mid(pp, r)
                d = math.hypot(m[0] - cx, m[1] - cz)
                if best is None or d < best:
                    best = d
        inv.append(best)
    med_inv = sorted(inv)[len(inv) // 2]
    # the median barely moves under a swap; what discriminates is the WORST
    # group -- with the arms the right way round no retire range sits on top
    # of the road the spawn arm is filling.
    check('J3 the polarity is the right way round: with the arms swapped the '
          'worst group puts a retire range on top of the spawn road',
          min(inv) < min(dists) / 2.0,
          'forward worst %.1f m vs inverted worst %.1f m'
          % (min(dists), min(inv)))
    check('J4 forwards, even the worst group keeps its retire rows well '
          'outside the view gate', min(dists) >= 60.0,
          'worst %.1f m' % min(dists))
    # the two arms DO collide on some rows, which is why the port's sweep is
    # deferred and carries the claim guard.
    coll_groups = coll_cells = 0
    for w in range(count):
        sp, rt = set(), set()
        for g in ((w - k) % count for k in range(3)):
            _f, _l, base, cnt, _r, _p = windows[g]
            for i in range(cnt):
                q = requests[base + i]
                pp, lo, hi = rows_of(q)
                tgt = sp if q[3] == 0 else rt
                for r in range(lo, hi + 1):
                    tgt.add((pp, r))
        ov = sp & rt
        if ov:
            coll_groups += 1
            coll_cells += len(ov)
    check('J5 the two arms collide on some rows, so the sweep MUST be '
          'order-safe (retail spares a row the other racer still claims, '
          '@0x001A3745)',
          coll_cells > 0,
          '%d cells in %d groups' % (coll_cells, coll_groups))
    print('  collisions: %d (path,row) cells covered by BOTH arms in %d of %d '
          'window groups -- the claim guard is what keeps them stable'
          % (coll_cells, coll_groups, count))
    print('  split: retire arm a median %.1f m from the spawn arm\'s rows '
          '(worst group %.1f, best %.1f); %d of %d groups inside 160 m; '
          'inverted polarity would put the worst group at %.1f m'
          % (med, min(dists), max(dists), inside, len(dists), min(inv)))


# ============= K. THE PROGRESS THE POOL IS FED MUST BE THE CAR'S POSITION
def test_progress_source():
    """route.bin's graph, and why an AI ribbon cursor cannot supply progress.

    The pool window table indexes the SAME anchor space route.bin's links
    carry, and it tiles that space with no gaps -- so a progress that really is
    the car's always lands in exactly one window and FUN_001A33B0's +-1 stepper
    tracks it exactly.  What breaks is taking the anchor off whatever ribbon
    the AI walker's cursor is on: the junction rows carry their own, unrelated
    anchor ranges over their own node indices, so a cursor that has slipped
    onto one reports a progress that has nothing to do with the car.
    """
    track = os.environ.get('B3_TRACK',
                           os.environ.get('B3_POSTFX_TRACK', 'US_C3_V1'))
    path = os.path.join(REPO, 'build', 'tracks', track, 'route.bin')
    try:
        data = open(path, 'rb').read()
    except OSError:
        print('  no route.bin for %s' % track)
        return
    if data[:4] != b'B3RT':
        return
    ver, wall, cen, onc, route, _rstart = struct.unpack_from('<6I', data, 4)
    _flags, strip = struct.unpack_from('<2I', data, 32)
    off = 40 + (wall * 2 + cen + onc + route + strip * 2) * 12
    np_, ns, npair, nl, _npl = struct.unpack_from('<5I', data, off)
    off += 20 + np_ * 12
    secs = [struct.unpack_from('<IIHH', data, off + i * 12) for i in range(ns)]
    off += ns * 12 + npair * 4
    links = [struct.unpack_from('<HHBBHH', data, off + i * 10)
             for i in range(nl)]

    main, junction = [], []
    for pb, lb, nc, fl in secs:
        anchors = [links[lb + n][0] for n in range(nc)]
        (main if (fl & 1) else junction).append((nc, min(anchors),
                                                 max(anchors)))
    check('K1 route.bin marks the main circuit rows with flags bit 0',
          len(main) > 0, '%d main, %d junction' % (len(main), len(junction)))
    check('K2 every main row spans the whole anchor space, so a nearest-node '
          'search over them is a total progress function',
          all(lo == 0 for _n, lo, _h in main)
          and len(set(h for _n, _l, h in main)) == 1,
          'main rows %r' % main)
    check('K3 the junction rows carry their OWN anchor sub-ranges over their '
          'own node indices -- a cursor that slips onto one reports a '
          'progress that is not the car\'s',
          any(lo > 0 or h < main[0][2] for _n, lo, h in junction),
          'junction rows %r' % junction)

    loaded = load_paths(track)
    if loaded is None:
        return
    _points, _paths, windows, _requests, _b, _r = loaded
    ranges = sorted((w[0], w[1]) for w in windows)
    gaps = [(ranges[i][1], ranges[i + 1][0])
            for i in range(len(ranges) - 1)
            if ranges[i + 1][0] != ranges[i][1] + 1]
    check('K4 the pool window table TILES the anchor space with no gaps, so a '
          'correct progress can never fall between windows',
          not gaps, 'gaps %r' % gaps[:5])
    check('K5 the window table covers the same anchor space as the graph',
          ranges[0][0] == 0 and ranges[-1][1] == main[0][2],
          'windows %d..%d, anchors 0..%d'
          % (ranges[0][0], ranges[-1][1], main[0][2]))
    print('  progress: %d main rows spanning anchors 0..%d, %d junction rows '
          '%s; %d windows tile the space with no gaps'
          % (len(main), main[0][2], len(junction),
             [(lo, h) for _n, lo, h in junction], len(windows)))


# ============ L. THE TRAVEL-SENSE FLAG IS THE MOST DANGEROUS BIT IN THE POOL
def test_flag_blast_radius():
    """What +0x363BC costs when it is wrong, and that the wrap cannot flip it.

    The flag inverts FUN_001A3470's dispatch.  With it at 1 the direction-0
    requests SPAWN and their ranges lie on the road the player is driving; with
    it at 0 those same ranges RETIRE.  So a single spurious backward window
    crossing turns the spawn arm into a retire arm directly under the player --
    which is what the nine in-view DESPAWN(unstamp) events were.  This pins
    both halves: how near the direction-0 ranges are (so the cost is explicit),
    and that the window table wraps in one step (so the lap boundary cannot
    produce the spurious crossing).
    """
    track = os.environ.get('B3_TRACK',
                           os.environ.get('B3_POSTFX_TRACK', 'US_C3_V1'))
    loaded = load_paths(track)
    if loaded is None:
        return
    points, paths, windows, requests, _b, _r = loaded
    path_route = os.path.join(REPO, 'build', 'tracks', track, 'route.bin')
    try:
        rd = open(path_route, 'rb').read()
    except OSError:
        return
    if rd[:4] != b'B3RT':
        return
    import math
    _ver, wall, cen, onc, route, _rs = struct.unpack_from('<6I', rd, 4)
    _fl, strip = struct.unpack_from('<2I', rd, 32)
    off = 40 + (wall * 2 + cen + onc + route + strip * 2) * 12
    np_, ns, npair, nl, _npl = struct.unpack_from('<5I', rd, off)
    off += 20
    rpts = [struct.unpack_from('<3f', rd, off + i * 12) for i in range(np_)]
    off += np_ * 12
    secs = [struct.unpack_from('<IIHH', rd, off + i * 12) for i in range(ns)]
    off += ns * 12
    rpairs = [struct.unpack_from('<HH', rd, off + i * 4) for i in range(npair)]
    off += npair * 4
    rlinks = [struct.unpack_from('<HHBBHH', rd, off + i * 10) for i in range(nl)]

    anchor_pos = {}
    for pb, lb, nc, fl in secs:
        if not (fl & 1):
            continue
        for n in range(nc):
            a = rlinks[lb + n][0]
            p, q = rpairs[pb + n]
            A, B = rpts[p], rpts[q]
            anchor_pos.setdefault(a, []).append(((A[0] + B[0]) * .5,
                                                 (A[2] + B[2]) * .5))

    def mid(pi, r):
        a = points[paths[pi][0][r][0]]
        b = points[paths[pi][0][r][1]]
        return ((a[0] + b[0]) * .5, (a[2] + b[2]) * .5)

    count = len(windows)
    d0, d12 = [], []
    for w in range(count):
        road = []
        for a in range(windows[w][0], windows[w][1] + 1):
            road.extend(anchor_pos.get(a, []))
        if not road:
            continue
        near = {0: None, 1: None}
        for g in ((w - k) % count for k in range(3)):
            _f, _l, base, cnt, _r2, _p = windows[g]
            for i in range(cnt):
                fr, lr, pp, direction = requests[base + i]
                lo = min(fr, lr)
                hi = min(max(fr, lr), len(paths[pp][0]) - 1)
                key = 0 if direction == 0 else 1
                for r in range(lo, hi + 1):
                    m = mid(pp, r)
                    for px, pz in road:
                        dd = math.hypot(m[0] - px, m[1] - pz)
                        if near[key] is None or dd < near[key]:
                            near[key] = dd
        if near[0] is not None:
            d0.append(near[0])
        if near[1] is not None:
            d12.append(near[1])
    med0 = sorted(d0)[len(d0) // 2]
    med12 = sorted(d12)[len(d12) // 2]
    check('L1 the direction-0 ranges lie ON the road the player is driving',
          med0 < 60.0, 'median %.1f m' % med0)
    check('L2 the direction-1/2 ranges lie far from it', med12 > 200.0,
          'median %.1f m' % med12)
    check('L3 so inverting the flag moves the retire arm from %.0f m to %.0f m '
          'from the player -- it is the single most damaging bit in the pool'
          % (med12, med0), med0 * 3.0 < med12)
    # and the wrap: the table tiles the space, so target-window stepping puts
    # the lap boundary one step forward instead of `count-1` steps backward.
    ranges = sorted((w[0], w[1]) for w in windows)
    contiguous = all(ranges[i + 1][0] == ranges[i][1] + 1
                     for i in range(len(ranges) - 1))
    check('L4 the window table tiles the anchor space, so the window holding '
          'a progress is always defined and the step direction can be chosen '
          'by circular distance', contiguous)
    fwd = (0 - (count - 1) + count) % count
    back = ((count - 1) - 0 + count) % count
    check('L5 a lap wrap is ONE forward step that way, against %d backward '
          'steps under an exact-match wrap test' % back,
          fwd == 1 and back == count - 1)
    print('  flag: direction-0 ranges a median %.1f m from the player\'s road, '
          'direction-1/2 a median %.1f m; lap wrap costs 1 step forward vs %d '
          'backward' % (med0, med12, back))


# ================= M. THE POOL IS DRIVEN BY THE LOCAL PLAYERS, NOT THE FIELD
# FUN_001A3EA0 @0x001A3ED3: the manager owns exactly TWO per-racer window
# bytes.  A third racer's window byte would land on the first racer's request
# countdown, so the racer list FUN_001A28B0 walks cannot be the 8-car field.
TWO_RACERS_BYTES = (
    '8D 90 A7 63 03 00 '             # LEA EDX,[EAX+0x363A7]   the window bytes
    'BE 02 00 00 00 '                # MOV ESI,2               <-- TWO
    '8B FF '                         # MOV EDI,EDI             (align)
    '88 4A 02 '                      # MOV [EDX+2],CL          countdown = 0
    '88 0A '                         # MOV [EDX],CL            window = 0
    '42 '                            # INC EDX
    '4E '                            # DEC ESI
    '75 F7 '                         # JNZ
)
# FUN_001A3470 @0x001A3511: and the occupancy map is two bits per row in a
# BYTE, indexed by the racer -- index 2 shifts the stamp out of the byte.
TWO_BITS_BYTES = (
    '8A 44 24 34 '                   # MOV AL,[ESP+0x34]       the racer index
    'D0 E1 '                         # SHL CL,1                (row & 3) * 2
    '02 C8 '                         # ADD CL,AL               + racer
    'B0 01 '                         # MOV AL,1
    'D2 E0 '                         # SHL AL,CL               1 << (0..7)
)


def test_local_views(img):
    for name, va, hexstr in (
            ('M1 FUN_001A3EA0 @0x001A3ED3 keeps TWO per-racer window bytes',
             0x001A3ED3, TWO_RACERS_BYTES),
            ('M2 FUN_001A3470 @0x001A350D the occupancy map is two bits per '
             'row in a byte, indexed by the racer', 0x001A350D,
             TWO_BITS_BYTES)):
        expect = want(hexstr)
        got = img.read(va, len(expect))
        check('%s (%d bytes)' % (name, len(expect)), got == expect,
              'want %s\n       got  %s' % (hexs(expect), hexs(got)))
    # a racer index of 2 cannot be represented: prove the shift overflows
    worst = (3 * 2) + 2
    check('M3 a racer index of 2 shifts the stamp out of the byte '
          '((row&3)*2 + 2 reaches bit %d)' % worst, worst > 7)


def test_ai_owner_reach():
    """What an AI-owned window group does to the player, measured.

    Each racer stamps {c, c-1, c-2} of its own.  A racer's retire ranges are a
    median 309 m from ITS road -- and essentially unbounded from anybody
    else's, so an AI elsewhere on the circuit sweeps rows next to the player.
    """
    track = os.environ.get('B3_TRACK',
                           os.environ.get('B3_POSTFX_TRACK', 'US_C3_V1'))
    loaded = load_paths(track)
    if loaded is None:
        return
    points, paths, windows, requests, _b, _r = loaded
    import math

    def mid(pi, r):
        a = points[paths[pi][0][r][0]]
        b = points[paths[pi][0][r][1]]
        return ((a[0] + b[0]) * .5, (a[2] + b[2]) * .5)

    count = len(windows)
    # for every ordered pair of windows (player at P, an AI at A), how close do
    # the AI's retire rows come to the player's own spawn rows?
    spawn_rows = {}
    retire_rows = {}
    for w in range(count):
        sp, rt = [], []
        for g in ((w - k) % count for k in range(3)):
            _f, _l, base, cnt, _r2, _p = windows[g]
            for i in range(cnt):
                fr, lr, pp, direction = requests[base + i]
                lo = min(fr, lr)
                hi = min(max(fr, lr), len(paths[pp][0]) - 1)
                step = max(1, (hi - lo) // 6 or 1)
                tgt = sp if direction == 0 else rt
                for r in range(lo, hi + 1, step):
                    tgt.append(mid(pp, r))
        spawn_rows[w] = sp
        retire_rows[w] = rt
    # only pairs where the other racer is genuinely elsewhere: three or more
    # windows away, so its group cannot legitimately be describing this road.
    far_close, far_pairs, worst_far = 0, 0, None
    for P in range(count):
        for A in range(count):
            sep = min((A - P) % count, (P - A) % count)
            if sep < 3:
                continue
            best = None
            for m in retire_rows[A]:
                for q in spawn_rows[P]:
                    d = math.hypot(m[0] - q[0], m[1] - q[1])
                    if best is None or d < best:
                        best = d
            if best is None:
                continue
            far_pairs += 1
            if best < VIEW_GATE_M:
                far_close += 1
            worst_far = best if worst_far is None else min(worst_far, best)
    check('M4 a racer three or more windows away still has retire ranges that '
          'reach inside the 160 m gate of the road another racer is filling '
          '-- which is why the pool must run for the LOCAL VIEWS only',
          far_close > 0,
          '%d of %d distant pairs' % (far_close, far_pairs))
    check('M5 and they reach all the way in, not just to the edge',
          worst_far is not None and worst_far < 60.0,
          'closest %.1f m' % (worst_far if worst_far is not None else -1))
    print('  owners: %d of %d racer pairs three or more windows apart have the '
          'far racer\'s retire arm inside 160 m of the near racer\'s spawn '
          'rows (closest %.1f m)'
          % (far_close, far_pairs, worst_far))


# ====================================================== main
def main():
    img = Image(ELF)
    print('A. image bytes')
    test_image(img)
    print('F. pop-out rules: terminus, branch columns, sticky window')
    test_popout_image(img)
    print('B. retail\'s gate block under Unicorn')
    test_unicorn()
    print('C. port source (%s)' % os.path.relpath(FULL_C, REPO))
    test_port_source()
    print('D. model replay over the shipped spawn policy')
    test_replay()
    print('G. the link table: the terminus\'s way out')
    test_links()
    print('H. the request direction dispatch (recovered baseline)')
    test_direction_census()
    print('I. the direction split and its travel-sense flag')
    test_direction_split(img)
    print('J. where the split puts retail\'s retires')
    test_split_geometry()
    print('K. the progress the pool is fed')
    test_progress_source()
    print('L. the travel-sense flag and the lap wrap')
    test_flag_blast_radius()
    print('M. the pool is driven by the local players')
    test_local_views(img)
    test_ai_owner_reach()
    print('traffic view gate: %s (%d checks, %d failed)'
          % ('OK' if not FAILED[0] else 'FAILED', CHECKS[0], FAILED[0]))
    return 1 if FAILED[0] else 0


if __name__ == '__main__':
    sys.exit(main())
