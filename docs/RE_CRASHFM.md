# Crash FM — the radio and the DJ (Burnout 3: Takedown, Xbox)

Recovered from `burnout3.elf` via Ghidra, cross-checked against the disc's own
wave banks. This document covers the whole Crash FM object: it turns out the
"announcer" is not a separate system bolted onto the music player — **retail
models one radio station**, and the DJ, the station idents and the EA TRAX
songs are three voices of the same state machine.

Provenance legend, as elsewhere in `docs/`:

- **[C]** — decompiled from the executable and cited by address, or read
  directly off the disc's bytes.
- **[S]** — structural inference: the shape is forced by the code but the
  *name*/intent is ours.
- **[?]** — observed and unexplained.

All addresses are file VAs in `burnout3.elf`. All offsets hex.

---

## 0. The one-paragraph answer

Crash FM is a single object (the "radio") living inside the sound manager at
`soundmgr+0x34C8`; in the retail build it is statically allocated at
**`0x004115E8`** [C]. It owns the music stream, the DJ speech stream and the
station-ident jingle. It runs a 14-state machine that plays a song, waits out a
**60–70 second** timer, fades the song, plays an **ident jingle**, plays **one
DJ line**, and starts the next song. **The DJ never talks over the music.**
There is no dynamic ducking law to recover, because retail never ducks: it
*sequences*. The only gain relationship is a fixed balance — speech `1.00`,
ident `0.85`, music `0.455` (menus) / `0.60` (in race) [C].

That last point is the single most important finding for the port, and it is
the opposite of what the task assumed. See §6.

---

## 1. Where the lines live

### 1.1 The bank name table — `0x003AF1F8` … `0x003AF284` [C]

`.rdata` holds the DJ bank basenames in one run, 8 bytes apart:

```
003af1f8  "djmcr"     003af200  "djmbl"     003af208  "djmrr"
003af210  "djmel"     003af218  "djmra"     003af220  "djmgp"
003af228  "djmfo"
003af26c  "djrace"    003af274  "djgen"     003af27c  "djwww"
003af284  "ident"
```

Immediately above and below them, in the same region:

```
003af1ac  "Sound/Radio"
003af1b8  "../export/ValueDB/Sound/Radio.cfg"
003af1e0  ".xwb"
003af1ec  "EATraxD"      003af1f4  "_EATrax"
003af230  "%s%s%d.awd"
003af23c  "I_" "S_" "F_" "G_" "J_" "E_"        <- language prefixes
003af254  "DJAS"  "DJEU"  "DJUS"               <- region banks
003af28c  "tracks/"  "/"  "pveh/"
```

`Radio.cfg` is a ValueDB config path with **no code xref** in the shipped
executable — the values were baked at build time. That is why the tuning
constants below appear as `.rdata` floats rather than as a parsed file [S].

### 1.2 Bank file naming [C]

Every bank is opened as `<prefix><lang><basename>.xwb`, assembled by `sprintf`
with the format `"%s%s%s%s"`. The language table is indexed by `DAT_0045B9BC`;
the shipped US disc uses `E_`.

- **Global banks**: `tracks/` + `E_` + `<basename>` + `.xwb`
  → `Tracks/E_DJGEN.xwb`, `Tracks/E_DJMRR.xwb`, …
- **Per-track race banks**: built by `FUN_001550B0` (§2.1) as
  `tracks\<RR>\<TT>_1\` + `E_` + `djrace` + `.xwb`
  → `Tracks/US/C1_V1/E_DJRACE.xwb`, …

### 1.3 The bank inventory, counted off the disc [C]

| Bank | Entries | Role (inferred, §3) |
|---|---:|---|
| per-track `E_DJRACE.xwb` × 18 | **38** each | in-race race chatter, 3 sets |
| `E_DJGEN.xwb` | 18 | generic station chatter |
| `E_DJWWW.xwb` | 10 | web/plug stings |
| `E_DJAS` / `E_DJEU` / `E_DJUS` | 20 each | region-flavoured chatter |
| `E_DJMRA` (Race) | 5 | mode intro |
| `E_DJMGP` (Grand Prix) | 5 | mode intro |
| `E_DJMFO` (Face Off) | 5 | mode intro |
| `E_DJMEL` (Eliminator) | 5 | mode intro |
| `E_DJMRR` (Road Rage) | 5 | mode intro |
| `E_DJMBL` (Burning Lap) | 5 | mode intro |
| `E_DJMCR` (Crash) | 5 | mode intro |

**Total 807 speech payloads** — `18×38 + 3×20 + 18 + 10 + 7×5 = 807`, which is
exactly the count of non-music entries across the disc's 33 XWBs recorded in
`docs/AUDIO_NOTES.md`. The inventory closes with nothing left over.

All 807 are **48000 Hz, stereo, WMAv2, 160 kb/s CBR** — see
`docs/AUDIO_NOTES.md` §1. The `ENTRYNAMES` segment is empty in every bank, so
**no line has a name on disc**; a line is addressed only by its integer index.

> ### The 38 that pins the whole recovery
>
> The per-track banks hold **38 = 0x26** entries. Independently, the
> executable's radio constructor fills a **`0x26`-byte** table
> (`FUN_00153B00`, §4.2), walks it modulo `0x26`, and partitions the bank into
> three ranges that sum to exactly 38 (`FUN_001550B0`, §2.1). The disc's entry
> count and the ELF's loop bound agree without either having been fitted to the
> other. That is the cross-check that makes the rest of this document
> load-bearing rather than plausible.

---

## 2. Choosing a bank and a range

Selection is two-stage and uniform across every trigger: **pick a bank (a path
string) and a half-open index range `[lo, hi)` inside it**, then pick one index
in that range (§4). `lo = hi = 0` is the sentinel for "the whole bank" — the
consumer rewrites `hi` to the bank's real entry count on open.

### 2.1 `FUN_001550B0` — the per-track race bank and its three sets [C]

Builds the `E_DJRACE.xwb` path and returns a range. The range outputs come back
in `EBX`/`ESI` (the function is `__regparm`; Ghidra shows them as
`unaff_EBX`/`unaff_ESI`), and the callers store them into the radio's
`+0x608` (lo) and `+0x60C` (hi).

```c
/* FUN_001550B0(trackA, trackB, char *out_path, char flagA, char flagB) */
sprintf(out_path, "%s%s%s%s", "tracks\\<RR>\\<TT>_1\\", lang, "djrace", ".xwb");

if (flagA == 0)          { lo = 0x00; hi = 0x16; }   /* [ 0, 22)  set 0 — 22 lines */
else if (flagB != 0)     { lo = 0x16; hi = 0x1E; }   /* [22, 30)  set 1 —  8 lines */
else                     { lo = 0x1E; hi = 0x26; }   /* [30, 38)  set 2 —  8 lines */
```

Three disjoint sets, 22 + 8 + 8 = 38, exhausting the bank [C].

### 2.2 `FUN_001551E0` — the game-mode intro bank [C]

Maps the current game mode to one of the seven `djm*` banks. Mode comes from a
virtual call `(*(vtbl+0x90))()` on the session object at `DAT_004D5370+0x1B8`;
mode `7` is a container that re-dispatches on `(*(vtbl+0x94))()`.

```c
mode = session->vtbl->get_mode();
if (mode == 7) {                       /* sub-mode remap */
    switch (session->vtbl->get_submode()) {
        case 0: mode = 0; break;  case 1: mode = 2; break;
        case 2: mode = 3; break;  default: mode = 6; break;
    }
}
switch (mode) {
case 0: bank = (DAT_0073A1C4 == 1)          ? "djmfo"    /* Face Off   */
             : (is_grand_prix_series())      ? "djmgp"    /* Grand Prix */
             :                                 "djmra";   /* Race       */
        break;
case 1: bank = "djmel"; break;    /* Eliminator  */
case 3: bank = "djmrr"; break;    /* Road Rage   */
case 4: bank = "djmbl"; break;    /* Burning Lap */
case 6: bank = "djmcr"; break;    /* Crash       */
}
sprintf(out, "%s%s%s%s", "tracks/", lang, bank, ".xwb");
```

The Grand Prix test is `session->field_0x1BC->[4] == 1 && field->[8]->[0x38] > 1`
— "series mode with more than one event" [S]. Range is left `(0,0)` = whole
bank by every caller, i.e. **all 5 mode-intro lines are eligible** [C].

### 2.3 The three selectors [C]

Each trigger family has its own selector that chooses among §2.1, §2.2, and the
flat banks. `rng()` is the radio PRNG of §4.1.

**`FUN_001554A0`** — the idle / between-songs selector, called from the in-race
state machine:

```c
switch (rng() & 0xF) {
case 0x0: bank = "djwww"; lo = hi = 0;            break;  /*  1/16 */
case 0x7: bank = "djgen"; lo = hi = 0;            break;  /*  1/16 */
case 0xF: FUN_001551E0(path); lo = hi = 0;        break;  /*  1/16  mode intro */
default:  FUN_001550B0(trk, path, 0, 0);          break;  /* 13/16  race set 0 */
}
```

So **13 of every 16 idle lines are per-track race chatter**, and one in sixteen
each is a web plug, generic chatter, or a mode intro [C].

**`FUN_00155590`** — the loading/attract selector:

```c
if (FUN_00017310())              FUN_001550B0(trk, path, 1, 0);  /* set 2 [30,38) */
else if (rng() & 1)              FUN_001551E0(path);             /* mode intro    */
else                             FUN_001550B0(trk, path, 0, 0);  /* set 0 [0,22)  */
```

**`FUN_00155630`** — the results/post-race selector:

```c
if (DAT_00550886)                FUN_001550B0(trk, path, 1, 1);  /* set 1 [22,30) */
else if ((rng() & 7) == 0)       FUN_00155060(trk, path);        /* region bank   */
else                             FUN_001550B0(trk, path, 0, 1);  /* set 2 [30,38) */
```

`FUN_00155060` picks the region bank (`DJAS`/`DJEU`/`DJUS`) via
`FUN_001586A0()` returning an index into `PTR_DAT_003EC8EC` [C].

`FUN_00017310()` and `DAT_00550886` are the two predicates that steer towards
the 8-line sets. They are **[?]** — not decompiled here; from context they are
"is this a multi-event series / did the player win" [S].

---

## 3. Trigger map

The radio has a **mode** at `+0x5F8` set through `FUN_00154110`
(`Radio::SetMode`) [C], and the per-frame driver `FUN_00153E50` dispatches on
it:

| mode | dispatches to | selector | what it is [S] |
|---:|---|---|---|
| 0 | — | — | off |
| 1, 2 | `FUN_001544C0` | — | frontend / menus |
| 3 | — | — | suppressed (guarded out everywhere) |
| 4 | `FUN_00154800` | `FUN_00155590` | loading / attract |
| **5** | **`FUN_00154A70`** | **`FUN_001554A0`** | **in race** |
| 6 | ident only | — | post-race / results |

Mode 5 = in race is confirmed [C] at `FUN_00026D30:0x26DA0`, which sets
`DAT_00411BE0 = 5` (the radio's `+0x5F8`) when the session state becomes `2`.

`FUN_00154110` also swaps the music gain with the mode: modes **4, 5 and 6 get
`DAT_003EC934` = 0.60**, every other mode gets `DAT_003EC930` = 0.455 [C].

### 3.2 The race-start intro — mode 4 speaks immediately [C]

This is the second trigger, and it is the one a player actually notices.

**`FUN_00154800`, the mode 4 machine, has no cooldown gate at all.** Its
switch falls straight through:

```c
case 0:   FUN_00153310();      /* stop the music        */
case 0xB: radio->state = 4;    /* ...no timer test...   */
case 4:   FUN_00154270();      /* start the ident       */
          voice = FUN_001CE140();
          FUN_00155590(...);   /* pick bank + range     */
          radio->state = 5;
```

Every entry state ≤ 0xB lands in case 4 in the same frame. So **entering mode
4 speaks at once, whatever the 60–70 s timer says**, and it stops the music
first.

The game enters it on the pre-race branch of `FUN_00026D30`, immediately
before the session starts running:

```
0002703F  B8 04 00 00 00    MOV EAX, 4              ; the mode
          B9 E8 15 41 00    MOV ECX, 0x004115E8     ; the radio object
          E8 CC D0 12 00    CALL 0x00154110         ; Radio::SetMode
```

and only later, when the session state reaches 2, sets mode 5
(`FUN_00026D30:0x26DA0`, §3). So retail's order is:

> **mode 4 → ident → one speech line → mode 5 → the first song → the 60–70 s
> cycle.**

The DJ introduces the event and *then* the music starts. That is why players
remember him talking at the start of a race, and it is entirely separate from
the between-songs chatter of §5.

Two consequences the first version of this port got wrong, both now fixed:

- It never entered mode 4, so the first line could not arrive before ~60 s —
  usually after the race had already ended.
- It reset the cooldown on every mode change. **The whole executable writes
  `+0x610` in exactly three places** — `FUN_00153BE0:0x153CCA` (boot, once)
  and `FUN_00154A70:0x154C5F` / `:0x154EF9` (after a line). No mode change
  touches it. The reset was invention, and it restarted the wait every time
  the radio changed mode.

The intro plays at the **0.75** non-race multiplier (`_DAT_003EC960`, §6.1),
not 1.00 — `FUN_00154800` is not the in-race path.

### 3.1 What retail does *not* have

This is the part worth stating plainly, because the port's event set is much
richer than the thing being ported to it.

**There is no per-event DJ trigger.** No takedown fires a line. No crash, no
overtake, no position change, no final lap, no near miss. The recovered
executable contains exactly three selectors (§2.3), and all three are reached
from the radio's own state machine on its own timer — never from gameplay code.
Searching every write to the radio's mode field finds only `FUN_00154110`, its
two callers, and the constructor; nothing in the takedown, score, crash or race
flow code touches the radio at all [C].

The sub-event field `+0x5FC`, which `FUN_001544C0` switches on (cases 0/1/2 vs
3/4), is **never written non-zero anywhere in the executable** — the only writes
found are the constructor and the reset, both storing 0 [C]. Cases 3 and 4 of
the frontend path are dead code in the shipped build **[?]**. They are the only
hint that a per-event path once existed.

So the honest recovered answer to "what triggers a line" is exactly two
things: **entering mode 4, which speaks immediately (§3.2), and the expiry of
the 60–70 s timer while in mode 5 (§5).** Nothing else. Anything richer than
that in the port is our invention, and §7 marks it as such.

---

## 4. Selection within a bank

### 4.1 The radio PRNG — `DAT_004A1BE0` / `DAT_004A1BE4` [C]

A two-word generator, stepped identically at every site:

```c
static unsigned rng_x, rng_y;    /* DAT_004A1BE0, DAT_004A1BE4 */

static unsigned rng(void)
{
    rng_x = rng_x * 0x10000u + (rng_x >> 16) + rng_y;
    rng_y = rng_y + rng_x;
    return rng_x;
}
```

This is the same generator the port already carries as `b3_rand01`
(`src/burnout3_vehicle_sim.c:244`, retail `FUN_00048760`), with retail's seeds
`0xFD462907` / `0x02B9D6F8`. The radio shares the global instance.

### 4.2 The 38-byte shuffle table [C]

The radio holds a byte array at `+0x5AC`, length `0x26`, and a cursor at
`+0x615`.

- **Constructor `FUN_00153B00`** fills it with the identity `0,1,…,0x25`:
  ```c
  for (i = 0; i < 0x26; i++) radio->shuf[i] = (char)i;
  ```
- **`FUN_001553D0`** re-seeds the cursor and shuffles:
  ```c
  radio->shuf_idx = rng() % 0x26;
  FUN_001556C0();                    /* Fisher-Yates over the table */
  ```
- **`FUN_001556C0`** is a textbook Fisher-Yates using the same PRNG:
  ```c
  for (i = 0; i < n; i++) {
      j = rng() % (n - i) + i;
      swap(tab[j], tab[i]);
  }
  ```

### 4.3 The pick, with anti-repeat [C]

The in-race path (`FUN_00154A70:0x154D4A`) and the frontend path
(`FUN_001544C0:0x15461C`) both use the shuffle table, walked cyclically, with
**one retry if the draw would repeat the previous line**:

```c
lo   = radio->lo;                       /* +0x608 */
span = radio->hi - lo;                  /* +0x60C - +0x608 */
prev = radio->cur;                      /* +0x600 */

radio->shuf_idx = (radio->shuf_idx + 1) % 0x26;
radio->cur      = radio->shuf[radio->shuf_idx] % span + lo;

if (prev == radio->cur) {               /* immediate-repeat rejection, ONE retry */
    radio->shuf_idx = (radio->shuf_idx + 1) % 0x26;
    radio->cur      = radio->shuf[radio->shuf_idx] % span + lo;
}
```

Note the retry is unconditional-once, not a loop: a second collision is
accepted. This matters for parity — a "retry until different" implementation
would diverge [C].

The loading/attract path (`FUN_00154800:0x154920`) does **not** use the table
and has **no anti-repeat** — it draws straight from the PRNG:

```c
radio->cur = radio->lo + (rng() % (radio->hi - radio->lo));
```

---

## 5. The state machine

`FUN_00154A70`, the in-race driver, on `radio->state` at `+0x5F4`. This is the
full Crash FM cycle.

```
 9  post-line: start ident jingle; cooldown = rand01()*10 + 60
    pick the next song (FUN_00155420) unless one is already queued
    start music (FUN_00153230)                                 -> 0
 0  music running; when FUN_001538D0() (music actually playing) -> 1
 1  wait for the ident to finish; stop it, unload it, load the
    NEXT ident (FUN_001543B0); start/park the music             -> 2
 2  music playing; when FUN_00153890() (song ended / cue)       -> 0xB
0xB cooldown -= dt.        while cooldown > 0: stay.
    when expired:  if FUN_00153580() < 20000   (song position)
                       cooldown = rand01()*10 + 60; stay        -> 0xB
                   else FUN_00153390()  (advance the playlist)  -> 3
 3  start ident jingle (FUN_00154270)                           -> 4
 4  allocate the speech stream voice (FUN_001CE140)
    choose bank + range (FUN_001554A0)                          -> 5
 5  open the bank (FUN_001CE500); pick the line index (§4.3)    -> 6
 6  start the decode at that index (FUN_001CE6A0)               -> 7
 7  wait for the ident to finish; stop + reload ident;
    set the speaker matrix, set gain, category 0x0E; PLAY       -> 8
 8  wait for the line to finish (FUN_001CED90);
    stop (FUN_001CE920), free the voice (FUN_001CE180)          -> 9
0xC crash-cinema hold: wait out FUN_00154350()                  -> 0xB
0xD idle / disabled
```

### 5.1 The cooldown — 60 to 70 seconds [C]

Written in two places, identically:

- `FUN_00153BE0:0x153C90` (radio boot):
  `radio->cooldown = rng() * DAT_0054F46C * 10.0f + 60.0f;`
- `FUN_00154A70:0x154EEB` and `:0x154C51`:
  `radio->cooldown = (float)(rand01() * 10.0 + 60.0);`

`DAT_0054F46C` is `1/2^32`, the same scale constant `b3_rand01` uses. So the
gap between DJ lines is **uniform on [60, 70) seconds** [C].

The decrement is `radio->cooldown -= DAT_0060EA1C` per frame, where
`DAT_0060EA1C` is the frame delta [C].

### 5.2 The station idents [C]

`FUN_001543B0` builds the jingle path with the `"%s%s%d.awd"` format found at
`0x003AF230`:

```c
sprintf(radio->ident_path, "%s%s%d.awd", "sound/", "ident", radio->ident_n);
...
radio->ident_n++;
if (radio->ident_n == 10) radio->ident_n = 0;      /* cycles 0..9 */
```

`sound/ident0.awd` … `sound/ident9.awd`, **strictly sequential, wrapping at
10** — not random [C]. These are the ten AWD files already recorded in
`docs/AUDIO_NOTES.md` §2. An ident is started *before* the line is chosen and
must finish before the line plays (state 7), so every DJ line is introduced by
a jingle.

### 5.3 Suppression [C]

The line is skipped, and the state machine short-circuits to `0xD`, when any of:

- `radio->suppress` (`+0x62C`) is set
- `DAT_0073A1C0 > 1` — more than one active player (split screen)
- `DAT_005A3759` — a global mute
- `DAT_00463AF4 != 0` — a modal/pause state

(`FUN_00154800:0x154803`, `FUN_00154A70:0x154A8E`) [C].

---

## 6. Gain, and the absence of ducking

### 6.1 The constants [C]

Read from `.rdata` at `0x003EC920`:

| Address | Value | Role |
|---|---:|---|
| `DAT_003EC928` | **1.000** | speech (DJ) gain |
| `DAT_003EC92C` | **0.850** | ident jingle gain |
| `DAT_003EC930` | **0.455** | music gain, frontend modes |
| `DAT_003EC934` | **0.600** | music gain, modes 4/5/6 (in race) |

and at `0x003EC950`, the per-path speech multipliers:

| Address | Value | Applied by |
|---|---:|---|
| `_DAT_003EC958` | **1.00** | `FUN_00154A70` (in race) |
| `_DAT_003EC95C` | **0.75** | `FUN_001544C0` (frontend) |
| `_DAT_003EC960` | **0.75** | `FUN_00154800` (loading) |

### 6.2 How they are applied [C]

`FUN_00153BE0:0x153DC4` derives everything from one user setting,
`DAT_004AE1D5`, a 0–100 radio volume:

```c
v                = (float)DAT_004AE1D5 * 0.01f;
radio->dj_vol    = DAT_003EC928 * v;    /* +0x620 */
radio->music_vol = v;                   /* +0x000 */
radio->ident_vol = DAT_003EC92C * v;    /* +0x628 */
```

and at play time:

```c
/* DJ line  — FUN_00154A70:0x154E5A */
voice->gain = radio->dj_vol * (DAT_0040B314 ? 0.75f : 1.00f);
voice->category = 0x0E;                 /* speech bus */

/* music    — FUN_00152F30:0x153016 */
voice->gain = radio->music_gain * radio->music_vol;   /* music_gain = 0.455 | 0.60 */
voice->category = 0x0D;                 /* music bus  */
```

Two mixer categories: **`0x0D` music, `0x0E` speech** [C].

### 6.3 There is no duck [C]

`FUN_001CEDB0` — the function called immediately before every DJ line, and the
obvious candidate for a ducking law — is **not** a ducking function. It takes
four floats, `log2`-converts each, and writes six voice fields at `+0x58`,
`+0x60`, `+0x68`, `+0x70`, `+0x78`, `+0x80`. It is a **5.1 speaker matrix**
(front pair, rear pair, centre, LFE). The DJ passes
`(1.0, 0.0, 0.125, 0.0)`; the music passes `(1.0, 0.83, 0.0, 0.14)` [C].

Nothing anywhere reads the "DJ is speaking" flag (`+0x618` bit 1) to scale a
music voice. The music gain changes only with the radio *mode* (§3), never with
DJ activity. What actually happens is the sequencing in §5: state `0xB`
advances the playlist (`FUN_00153390`) *before* the line, and state 9 starts
the next song *after* it.

**So: retail's "ducking" is a fixed balance plus a hard sequence.** The music
sits ~2.4 dB under the speech in race (0.60 vs 1.00) and ~6.8 dB under it in
menus (0.455 vs 1.00), permanently; the DJ speaks in the gap between songs.

### 6.4 The hand-over, and what the port now does [C]

The port implements the sequence, not a duck. `src/burnout3_dj.c` takes the
bus at `S_IDENT_PRE` — exactly where retail advances the playlist on its way
from state 0xB to state 3 — and gives it back at `S_POST`, where retail
starts the next song:

| retail | port |
|---|---|
| `FUN_00153390` advance (state 0xB → 3) | `b3_music_set_hold(1)` |
| `FUN_00153310` stop (mode 4, case 0) | same call, same place |
| `FUN_00153230` start (state 9) | `b3_music_set_hold(0)` + `b3_music_skip()` |

`b3_music_skip()` and not a resume, because retail's playlist has moved on:
the song that was interrupted does not come back.

Two things this needed on the music side, both of which were bugs waiting to
happen rather than design:

- **`b3_music_stop()` alone does not hold.** `b3_music_pump()` treats "no file
  open" as "the track ran out" and rolls straight on to the next one, so the
  song reappeared two frames later and the DJ talked over it after all. The
  hold flag now stops the pump rolling on as well.
- **A race can end mid-line.** `b3_dj_tick()` only runs inside the sim's inner
  loop, so a race that finished while the radio held the bus would leave the
  song stopped for good. `b3_dj_release_if_idle()`, called every frame from
  outside that loop, hands it back after two sweeps with no tick.

Retail's `FUN_00154A70 @0x00154C40` also refuses to cut a song that has only
just started — under 20000 on `FUN_00153580()` re-arms the cooldown instead.
The port carries that as a 20 s floor: the **20000 is [C]**, reading its unit
as seconds is **[S]**, since `stream+0x238 * stream+0x20` was not pinned down.

`B3_DJ_DUCK=1` restores the retired 0.7583 duck for A/B listening. It is [S]
and is no longer the default.

### 6.5 The crash cinematic — [S] by request

**Retail leaves the song playing under the crash bed.** `RE_MUSIC.md` §6.3:
the bed plays at `0.70` against the song's `0.30` (`mgr+0x83C` =
`[0x003EC424]` @`0x0014B59E`), summing to 1.0, and the song is *never ducked*.
`FUN_00150E80`, the bed's state machine, only ever touches its own two layer
voices at `mgr+0x884`/`+0x888` — it does not reach the radio at all, and no
mode change accompanies a crash, so the radio's music gain does not move
either. That is the recovered behaviour and it is **[C]**.

The user asked for the song to be **paused** during a cinematic instead. That
ships as the default and is **[S]** — `B3_MUSIC_CRASH=retail` restores the
recovered mix. It is a true pause, not a mute: `b3_music_next_sample()`
returns the bed alone and does not advance `g_rd`, so the song resumes on the
sample it stopped on, and the EA TRAX ticker keeps naming it throughout.

**The crash pause does NOT freeze the radio.** This is deliberate and it
matches retail: the cooldown runs on `DAT_0060EA1C`, the dilated per-tick
delta, and a crash cinematic stretches that delta rather than stopping it —
retail's radio counts straight through a crash and so does ours, because
`b3_dj_tick()` is inside the sim loop, which runs while `CRASHED`. So a
cinematic lengthens the wait in wall-clock terms by exactly the dilation, and
nothing about the pause touches the radio's clock. If the DJ's timer happens
to expire mid-cinematic he speaks over it, which is also what retail would do.

---

## 7. What the port does, and how it differs

Implemented in `src/burnout3_dj.c` / `.h`, gated by `B3_DJ` (default on).

### Carried over verbatim [C]

| Law | Source |
|---|---|
| Bank inventory and entry counts | §1.3 |
| Three race sets `[0,22) [22,30) [30,38)` | `FUN_001550B0` |
| Mode → `djm*` bank map | `FUN_001551E0` |
| Idle selector weights 13/16, 1/16, 1/16, 1/16 | `FUN_001554A0` |
| Two-word PRNG and its seeds | `FUN_00048760`, §4.1 |
| 38-byte identity table + Fisher-Yates + cursor `% 0x26` | §4.2 |
| Cyclic pick with exactly one repeat retry | §4.3 |
| Cooldown uniform on `[60, 70)` s | §5.1 |
| **Race-start intro: mode 4 speaks immediately, at 0.75** | §3.2 |
| **The cooldown is set at boot and after a line — never by a mode change** | §3.2 |
| Idents `sound/ident0..9.awd`, sequential, wrap at 10 | §5.2 |
| Ident must finish before the line starts | §5, state 7 |
| Gains 1.00 / 0.85 / 0.455 / 0.60 and the 0.75 non-race multiplier | §6.1–6.2 |
| **The hand-over: stop before the line, next song after — no duck** | §6.4 |
| **No cutting a song under 20000 old (the 20 s floor)** | §6.4 |
| Suppression conditions | §5.3 |

### Glue — required by the port, no retail counterpart [S]

- **The crash-cinematic music pause** (§6.5). Retail plays the song at 0.30
  under the bed; pausing it is a user request. `B3_MUSIC_CRASH=retail`.
- **The stuck-music watchdog** (§6.4). Retail's radio is driven by the sound
  manager, which never stops; ours is driven from the sim loop, which does.
- **48000 → 44100 resampling and stereo → mono downmix.** Retail's mixer takes
  the bank's native rate; ours is fixed at 44100 mono. Done at materialise time
  with the same linear cursor the crash bed uses.
- **Lazy per-bank materialisation.** Retail streams from the disc; we decode a
  bank on first use through the `T:eatrax:<n>` lazy-unit mechanism, because
  eagerly decoding 807 WMA payloads is not viable.
- **A private voice** rather than a slot in the 24-voice SFX pool, so a DJ line
  cannot be stolen mid-sentence.
- **`b3_dj_seed()` / `B3_DJ_SEED`**, so line selection is reproducible for the
  pinned-frame and audio gates. Retail had no such need.
- **The mode 4 → mode 5 handoff.** Retail's *game* drives it: the session
  moves 1 → 2 and the radio follows. This port has no distinct loading phase
  to hang mode 4 on — it goes straight to RACING — so the module hands itself
  over when the intro line ends. The audible result is retail's order; only
  the thing doing the sequencing differs.
- **The `CRASH FM` mixer row.** Retail's equivalent is the single 0–100
  `DAT_004AE1D5` radio volume; ours is a fourth slider next to ENGINE/SFX/MUSIC
  and multiplies the recovered balance rather than replacing it.

### Deliberately not implemented

- **Per-event DJ lines** (takedown, crash, overtake, final lap, results).
  Retail has none (§3.1); inventing them would be a new feature wearing
  recovered evidence's clothes. The port emits all of these events already, so
  the hook-up is a small change if it is ever *wanted* — but it would be
  labelled `[S]`, not parity.

### Open — `[?]`

- `FUN_00017310()` and `DAT_00550886`, the predicates steering §2.3 towards the
  8-line sets. The port currently treats both as false, which is the common
  case and yields set 0 / set 2 respectively.
- `+0x5FC` cases 3 and 4 in `FUN_001544C0` — dead in the shipped build.
- `Sound/Radio.cfg` — the ValueDB source for the §6.1 constants, not shipped
  and not parsed at runtime.
- No line has a name or transcript; index is the only identity. Mapping "that
  was X on Crash FM" to a specific song would require transcribing 807 clips.

---

## 7.1 Gates

`make test-dj` (`tools/validate_dj.py`) builds two probes from the real
`src/burnout3_dj.c` — the second linking `burnout3_music.c` alongside it, so
the hand-over is measured on the two channels separately rather than asserted.
Every number below comes out of the shipping code. 30 checks, all green:

| Check | Measured |
|---|---|
| cooldown floor (5.1) | min gap **60.96 s** over 40 lines |
| cooldown spread (5.1) | **9.18 s** — the `[60,70)` span |
| per-track lines are set 0 (2.1) | 34/34 drawn from `[0,22)` |
| idle selector weighting (2.3) | **0.85** per-track vs the 13/16 = 0.8125 law; the other 6 split `DJMRA` 3, `DJGEN` 2, `DJWWW` 1 — the three 1/16 branches |
| anti-repeat (4.3) | **0** immediate repeats |
| duck (6.3, [S]) | reaches **0.7583**, releases to **1.0000** |
| determinism | same seed → **byte-identical** DJ channel (sha256) |
| audibility | DJ channel **RMS 8466** |
| race-start intro (3.2) | first line at **0.17 s**; next at **+63.55 s** |
| intro is audible (3.2) | **RMS 6934** over the channel's first 6 s |
| intro determinism (3.2) | byte-identical across two runs |
| hand-over (6.4) | song RMS **0.00** across a whole line, DJ RMS **9615** |
| hand-over (6.4) | **0** frames anywhere with both audible |
| hand-over (6.4) | next song up at RMS **1656** after the line |
| crash pause (6.5) | song RMS **0.00** through the cinematic, **2386** after |
| crash retail mode (6.5) | `B3_MUSIC_CRASH=retail` keeps it at RMS **1891** |
| bank inventory (1.3) | all 12 global banks exact; **18 per-track banks × 38** |

Other gates run for this change: `make` and `make wasm` both clean, with the
warning set **byte-identical to the pre-change baseline** (6 pre-existing
`-Wunused-but-set-*`, none in the new code); `validate_aftertouch` **133/133**;
`validate_crashcinema` **115/115**; `web_smoke` **13/13** including
`AUDIO IS AUDIBLE` with 0 underruns and a DJ line confirmed in the worklet
ring; `web_shell_smoke` **6/6**; `web_cold_smoke` PASS.

**Lazy-decode cost.** `b3_dj_set_track()` materialises on the load path,
before the race starts: **0.89 s** native, **2.36 s** wasm, for the
per-track bank's 38 WMA payloads. It now also prefetches the other three
banks the idle selector can reach — `DJWWW`, `DJGEN` and the mode bank —
because each was decoding mid-race the first time its 1/16 branch came up.
Nothing the radio can choose is decoded during a race. (A song change
still is, but off the frame thread — RE_MUSIC.md §7.)
The first version prefetched only when `B3_TRACK` was set — which it usually
is not — and paid that 2.36 s as a mid-race stall instead; the fix is that a
NULL track id still prefetches whatever the port defaulted to.

**Where `b3_dj_tick()` is called, and a wrong turn worth recording.** It was
first placed *below* retail's inner loop, once per rendered frame, and moved
*inside* it, next to `b3_music_crash_tick()`, so the radio is charged once
per sim tick on the same dilated per-tick delta (`DAT_0060EA1C`) retail uses.
That is the right place on principle — the tick is what publishes the delta,
so the delta belongs to the tick — but **it should not be advertised as a bug
fix, because the measurement does not support one**: a 5-minute soak before
the move produced 10 lines in 898 s of audio and one after produced 11 in
898 s. The rate did not move.

The thing that briefly looked like a 2× drift was a misreading of the soak.
`B3_EXIT_AT` is measured on `g_race_time`, which only advances while
`g_state == RACING`; the radio, like the crash bed, also runs while
`g_state == CRASHED` — correctly, since retail's DJ talks straight through a
crash replay and its cooldown runs on the dilated delta that a crash
stretches. So the radio's clock legitimately leads the race clock by whatever
time the car spends wrecked, which for the offscreen autodriver is most of
it: 788 s of radio clock against a 200 s race clock.

`B3_DJ_LOG=1` now stamps each line with the radio's own accumulated clock, so
the gap is read rather than inferred. Ten consecutive in-game cycles:

```
t= 63.6  t=136.4  t=212.6  t=286.9  t=374.7
t=457.0  t=532.4  t=616.1  t=701.7  t=788.1
```

gaps **72.8 – 87.8 s**, against a predicted cycle of cooldown `[60,70)` plus
the ident plus the line itself (2.3–6.9 s). The law holds in the real game,
not only in the probe.

## 8. Address index

| Address | Name here | Role |
|---|---|---|
| `0x004115E8` | `g_radio` | the radio object (static instance) |
| `0x00153230` | `radio_music_start` | start / unpause the song |
| `0x001532D0` | `radio_music_play` | resume |
| `0x00153310` | `radio_music_stop` | stop and free the music voice |
| `0x00153390` | `radio_music_advance` | advance the playlist, rebuild the path |
| `0x00153500` | `radio_music_pause` | pause |
| `0x00153580` | `radio_music_pos` | song position (the 20000 gate) |
| `0x00153890` | `radio_music_ended` | song-ended predicate |
| `0x001538D0` | `radio_music_playing` | song-playing predicate |
| `0x00153B00` | `radio_ctor` | constructor; fills the 0x26 identity table |
| `0x00153BE0` | `radio_boot` | boot state machine; derives the gains |
| `0x00153E50` | `radio_tick` | per-frame driver; dispatches on mode |
| `0x00153F80` | `radio_apply_request` | consumes a pending mode request |
| `0x00154110` | `Radio::SetMode` | the mode setter; swaps the music gain |
| `0x00154270` | `radio_ident_start` | start the jingle voice |
| `0x00154350` | `radio_ident_busy` | jingle-playing predicate |
| `0x00154370` | `radio_ident_stop` | stop the jingle |
| `0x001543B0` | `radio_ident_load` | build `sound/identN.awd`, cycle 0..9 |
| `0x00154460` | `radio_ident_unload` | release the jingle AWD |
| `0x001544C0` | `radio_sm_frontend` | state machine, modes 1/2 |
| `0x0002703F` | — | `Radio::SetMode(4)` — the race-start intro (§3.2) |
| `0x00154800` | `radio_sm_loading` | state machine, mode 4 — **no cooldown gate** |
| `0x00154A70` | `radio_sm_race` | state machine, mode 5 — the main cycle |
| `0x00155060` | `radio_bank_region` | pick `DJAS`/`DJEU`/`DJUS` |
| `0x001550B0` | `radio_bank_djrace` | per-track path + the three ranges |
| `0x001551E0` | `radio_bank_mode` | game mode → `djm*` bank |
| `0x001552E0` | `radio_speech_free` | stop + free the speech voice |
| `0x00155330` | `radio_reset` | reset selection state |
| `0x001553D0` | `radio_shuffle_reseed` | seed the cursor, then Fisher-Yates |
| `0x00155420` | `radio_playlist_pick` | choose the next song |
| `0x001554A0` | `radio_sel_idle` | selector: 13/16 race, 1/16 each other |
| `0x00155590` | `radio_sel_loading` | selector for mode 4 |
| `0x00155630` | `radio_sel_results` | selector for results |
| `0x001556C0` | `radio_shuffle` | Fisher-Yates over the 0x26 table |
| `0x00155720` | `radio_stream_init` | 44100/2ch/16-bit stream setup |
| `0x001CE140` | `voice_alloc` | allocate a stream voice |
| `0x001CE500` | `bank_open` | open an XWB by path |
| `0x001CE6A0` | `bank_start_entry` | begin decoding entry N |
| `0x001CE920` | `voice_stop` | |
| `0x001CED90` | `voice_finished` | |
| `0x001CEDB0` | `voice_speaker_matrix` | 5.1 matrix (**not** a duck) |
| `0x004A1BE0` | `rng_x` | radio PRNG word 0 |
| `0x004A1BE4` | `rng_y` | radio PRNG word 1 |
| `0x004AE1D5` | `radio_volume` | user setting, 0–100 |

### Radio object layout (offsets from `0x004115E8`)

| Off | Type | Field |
|---|---|---|
| `+0x000` | f32 | music volume (user setting / 100) |
| `+0x004` | f32 | music gain (0.455 or 0.60) |
| `+0x4C4` | char[] | song path buffer |
| `+0x538` | u8 | EA TRAX mode flag |
| `+0x53C` | ptr | music stream voice |
| `+0x544` | i32 | music state |
| `+0x554` | i32 | current song index (−1 = none) |
| `+0x564` | u8 | music flags |
| `+0x568` | i32 | playlist length |
| `+0x56C` | char[] | DJ bank path buffer |
| `+0x58C` | char[] | ident path buffer |
| `+0x5AC` | u8[0x26] | **shuffle table** |
| `+0x5E0` | ptr | speech voice |
| `+0x5E4` | ptr | ident AWD |
| `+0x5EC` | ptr | ident voice |
| `+0x5F0` | i32 | boot state |
| `+0x5F4` | i32 | **state machine state** |
| `+0x5F8` | i32 | **mode** |
| `+0x5FC` | i32 | sub-event (always 0 — §3.1) |
| `+0x600` | i32 | **current line index** |
| `+0x604` | i32 | bank entry count |
| `+0x608` | i32 | **range lo** |
| `+0x60C` | i32 | **range hi** (0 = whole bank) |
| `+0x610` | f32 | **cooldown, seconds** |
| `+0x614` | u8 | ident counter 0..9 |
| `+0x615` | u8 | **shuffle cursor** |
| `+0x618` | u32 | flags (bit1 speaking, bit4/5 ident, bit7 advanced) |
| `+0x620` | f32 | DJ volume |
| `+0x628` | f32 | ident volume |
| `+0x62C` | u8 | suppress |
| `+0x62D` | u8 | line-done latch |
| `+0x62E` | u8 | music enabled |
| `+0x630` | u8 | radio enabled |
