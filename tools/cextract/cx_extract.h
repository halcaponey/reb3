/* cx_extract.h -- the shared contract for the C asset-extraction pipeline.
 *
 * This is the C11 port of the per-track python extractors under tools/.
 * The python tools remain THE SPEC: nothing in here may invent format
 * knowledge, and every module is expected to reproduce its python original
 * BYTE FOR BYTE (see tools/cextract/verify_cextract.py, the acceptance gate).
 *
 * ========================================== THE SOURCE (dump OR Xbox ISO) ==
 * `game_dir` is a STRING, and it always will be.  It names EITHER an expanded
 * dump directory OR the Xbox ISO itself; cxtract sniffs which with
 * cx_src_open() and binds the result process-wide with cx_vfs_bind().
 *
 * THE ONE CONVENTION, and it is the whole of it:
 *
 *     STAGES DO NOT CHANGE.  Keep joining paths onto `game_dir` exactly as
 *     before ("%s/pveh/vlist.bin", "%s/Tracks/%s/enviro.dat").  What changes
 *     is the PRIMITIVE underneath: read game bytes with the helpers your
 *     family already uses (cxa_slurp / cxb_read_file / cxd_read_file /
 *     cxf_read_file / cxg_blob_load), test for existence with
 *     cxe_is_dir / cxe_is_file / cxb_file_exists / cxf_file_size, and list
 *     directories with cxe_listdir_sorted / cxd_list_dir / cxf_walk_ext.
 *     Every one of those now goes through cx_vfs_* in cx_src.[ch], which
 *     recognises a path that lies under the bound source root and serves it
 *     from the image.  A path that does NOT -- an output file, a repo header,
 *     build/burnout3.elf -- falls through to plain libc untouched.
 *
 * So: NEVER call fopen()/stat()/opendir() directly on a path built from
 * `game_dir`.  A new low-level reader belongs on cx_vfs_fopen(),
 * cx_vfs_stat(), cx_vfs_is_dir(), cx_vfs_is_file(), cx_vfs_exists() or
 * cx_vfs_listdir().  Output paths keep using libc, as they always did.
 *
 * When the source is a DIRECTORY the shim binds nothing and every cx_vfs_*
 * call is a straight libc passthrough, so directory extraction runs the same
 * code it always ran.  The acceptance gate is that ISO extraction and
 * directory extraction are BYTE-IDENTICAL.
 *
 * ============================================================ THE STAGE ABI
 * Every extractor is one function with this exact signature:
 *
 *     int cx_extract_<thing>(const char *game_dir,   // the mounted game root
 *                            const char *track_dir,  // <game>/Tracks/<REG>/<Cn_Vn>
 *                            const char *track_id,   // "US_C3_V1"
 *                            const char *out_dir);   // per-track output dir
 *
 * Return 0 on success, non-zero on failure.  `out_dir` already exists when the
 * driver calls you; it is the directory that DIRECTLY receives the artefacts
 * (props.bin, track.obj, textures/...), i.e. the C-side equivalent of the
 * python `build/tracks/<ID>` that tools/extract_tlist.py's out_root() builds.
 * A module that emits a subtree (textures/, cars/) creates it under out_dir.
 *
 * ======================================================= LINKING WHILE SPLIT
 * The three agents land their .c files independently, and tools/cextract/
 * build.sh globs *.c, so the driver must link with any subset present.  The
 * rule is one feature macro per module, DEFINED IN THIS HEADER IN THE OWNING
 * AGENT'S BLOCK, right next to that module's prototype:
 *
 *     #define CX_HAVE_PROPS 1
 *     int cx_extract_props(const char *, const char *, const char *,
 *                          const char *);
 *
 * cx_main.c's stage registry is #ifdef'd on those macros, so a stage whose .c
 * file is absent is simply not compiled into the table -- no link error, and
 * the run summary reports it as "not built".  To add your module: drop your
 * .c file in, then add the #define + prototype to your block below.  Never
 * edit another agent's block.
 *
 * ========================================================== GLOBAL STAGES ==
 * The per-track ABI above runs once per track.  The GLOBAL stages -- the car,
 * art, audio and generator families -- run ONCE for the whole dump, and have a
 * shorter signature:
 *
 *     int cx_extract_<thing>(const char *game_dir,   // the mounted game root
 *                            const char *out_root);  // one output root
 *
 * `out_root` already exists when the driver calls you.  It is the pipeline's
 * REPO-ROOT STAND-IN, not a flat dump: extracted assets go to
 * <out_root>/build/..., and GENERATED HEADERS to <out_root>/gen/ .h files.
 * Return 0
 * on success.  The driver runs them with `--all-global`, and `--only <stage>`
 * selects individual ones in either mode.
 *
 * A stage that is registered but must NOT run in a bare `--all-global` -- one
 * that mines the Ghidra bridge rather than game files, say -- is named in
 * cx_main.c's manual list and stays reachable through `--only`.
 *
 * The driver also guarantees, before the first global stage runs: every path
 * you are handed is ABSOLUTE, the working directory is `out_root`, and
 * $B3_REPO_DIR / $B3_ELF / $B3_GLOBALUS point at the port's repo root, the
 * mapped retail image and Globalus.bin when those exist.  Prefer the
 * environment variables over a CWD-relative probe: the CWD is the OUTPUT root,
 * not the repo.
 *
 * REGISTRATION, and why it is a LIST rather than a central table: the driver
 * cannot name a stage it has never heard of, and the four agents land at
 * different times, so each agent's block below OWNS the list of its own global
 * stages.  In your block, next to the prototypes, write
 *
 *     #define CX_GLOBAL_STAGES_D(X)   \
 *         X(cars)                     \
 *         X(car_textures)
 *
 * -- one X(<name>) per stage, where <name> is both the stage's driver name and
 * the suffix of its `cx_extract_<name>` function.  cx_main.c concatenates the
 * four per-agent lists; an agent that has not landed simply has no list, which
 * expands to nothing, so the driver builds and runs with any subset present.
 * The macro is defined empty by the driver when it is absent, so you do NOT
 * need a #ifndef guard around yours.  As with everything else here: define it
 * inside YOUR block only.
 */
#ifndef CX_EXTRACT_H
#define CX_EXTRACT_H

/* Every stage the pipeline knows about, in the order the driver runs them.
 * The driver reports the ones whose CX_HAVE_* macro is undefined as pending,
 * so this list doubles as the port's progress board.  Order matters where a
 * stage consumes another's output (nav_edges reads route.bin from bgd_paths,
 * exactly as tools/extract_nav_edges.py does). */
#define CX_STAGE_LIST(X)      \
    X(TLIST,        tlist)        \
    X(TRACK,        track)        \
    X(TEXTURES,     textures)     \
    X(COLLISION,    collision)    \
    X(ENVMAP,       envmap)       \
    X(BGD_PATHS,    bgd_paths)    \
    X(TRAFFIC,      traffic)      \
    X(TRAFFIC_CARS, traffic_cars) \
    X(NAV_EDGES,    nav_edges)    \
    X(START_GRID,   start_grid)   \
    X(PACE,         pace)         \
    X(PROPS,        props)        \
    X(SCENERY,      scenery)      \
    X(LIGHT_PROBES, light_probes) \
    X(BVH,          bvh)

typedef int (*cx_stage_fn)(const char *game_dir, const char *track_dir,
                           const char *track_id, const char *out_dir);

/* --- agent A --- */
/* .bgd family: tlist / bgd_paths / traffic / nav_edges / start_grid.
 *
 * Also owns track resolution.  The driver calls it as
 *
 *     #define CX_HAVE_RESOLVE_TRACK 1
 *     int cx_resolve_track(const char *game_dir, const char *spec,
 *                          char *out_id, size_t id_sz,
 *                          char *out_dir, size_t dir_sz);
 *
 * -- spec being anything tools/extract_tlist.py's resolve() accepts (a tlist
 * id, a REG/Cn_Vn fragment, a display name, a bare index, a directory), 0 on
 * success.  Until this macro is defined, cx_main.c falls back to
 * cxc_resolve_track(), which implements only the id -> directory half
 * (FUN_001574F0's plain string split, tools/extract_tlist.py "id -> directory").
 * That fallback deliberately does NOT decode tlist.bin -- that is this
 * module's job, not the driver's. */

#include <stddef.h>              /* size_t, for cx_resolve_track */

#define CX_HAVE_RESOLVE_TRACK 1
/* tools/extract_tlist.py's resolve(): decode Tracks/tlist.bin (version 4,
 * count @+0x04, base-40 packed u64 ids @+0x408) and map the id onto its
 * directory the way FUN_001574F0 does -- "tracks/" + id[0:2] + "/" +
 * id[3:5] + "_" + id[6:8].  Accepts a tlist id (US_C3_V1), a REG/Cn_Vn
 * fragment, a directory path, or a bare tlist index; 0 on success.
 * (Display-name specs are not accepted: the python tool resolves those
 * through the name table at .data 0x0039EE00 inside build/burnout3.elf plus
 * Data/Globalus.bin, and this ABI is given no repo root to find the ELF --
 * without it the python tool itself falls back to "TRACK <n>" names.) */
int cx_resolve_track(const char *game_dir, const char *spec,
                     char *out_track_id, size_t id_cap,
                     char *out_track_dir, size_t dir_cap);

#define CX_HAVE_TLIST 1
/* Stage-ABI wrapper: extract_tlist.py emits no per-track artefact, so this
 * only re-resolves track_id against tlist.bin and checks it lands on
 * track_dir. */
int cx_extract_tlist(const char *game_dir, const char *track_dir,
                     const char *track_id, const char *out_dir);

#define CX_HAVE_BGD_PATHS 1
/* tools/extract_bgd_paths.py -> <out_dir>/route.bin ('B3RT' v3) and
 * tools/extract_traffic.py's RIDX half -> <out_dir>/traffic_paths.bin
 * ('B3TP' v4).  Gamedata.bgd is walked with the game's own streaming parser
 * FUN_0018B250 (event count @0x260, ids @0x08, param records @0x198; road
 * network from param+0x3D4/0x3D8); the boundary strip and the drive lines
 * are recovered from the file's own geometry [S].  No src/ header is
 * emitted -- the .h the python tool also writes is not part of this port. */
int cx_extract_paths(const char *game_dir, const char *track_dir,
                     const char *track_id, const char *out_dir);
int cx_extract_bgd_paths(const char *game_dir, const char *track_dir,
                         const char *track_id, const char *out_dir);

#define CX_HAVE_TRAFFIC 1
/* tools/extract_traffic.py's TRACK side -> <out_dir>/traffic.bin ('B3TR'
 * v4): the event mode block's six vehicle lists + gated specials, the
 * spawn/entry table, the oncoming drive line and the lanes recovered from
 * it.  The build/cars OBJ+PNG vehicle-asset half of that tool is NOT part
 * part of this module. */
int cx_extract_traffic(const char *game_dir, const char *track_dir,
                       const char *track_id, const char *out_dir);

#define CX_HAVE_NAV_EDGES 1
/* tools/extract_nav_edges.py -> <out_dir>/nav_edges.bin ('B3NE' v1): the
 * per-node cumulative arc length + width hint from each index row's
 * `edge_rel` table (FUN_00174AF0, stride 8), flat in route.bin nav-link
 * order.  Refuses to write when route.bin disagrees on the link count. */
int cx_extract_nav_edges(const char *game_dir, const char *track_dir,
                         const char *track_id, const char *out_dir);

#define CX_HAVE_START_GRID 1
/* tools/extract_start_grid.py -> <out_dir>/grid.bin ('B3GR' v1) ONLY: the
 * six 0x50-byte slots of the event SPATIAL record (param+0x3BC/0x3C0).
 * The src/burnout3_start_grid.h the python tool also writes is deliberately
 * NOT emitted here. */
int cx_extract_grid(const char *game_dir, const char *track_dir,
                    const char *track_id, const char *out_dir);
int cx_extract_start_grid(const char *game_dir, const char *track_dir,
                          const char *track_id, const char *out_dir);

/* --- pace --- */
/* The last of the compiled-in GAME DATA to leave src/.
 *
 * tools/gen_ai_pace.py baked every shipped track's per-opponent AI pace
 * records into src/burnout3_ai_pace.h; this stage emits the same bytes for
 * ONE track as a runtime asset, and src/burnout3_ai_pace_runtime.h reads it.
 * The event's lap count (param+0x3B8) rides along in the same record, which
 * is what frees src/burnout3_trackselect.h of its per-track `laps` column.
 *
 * tools/gen_ai_pace.py -> <out_dir>/pace.bin ('B3PC' v1): per event, the
 * base-40 id, the lap count (param+0x3B8), the opponent count (param+0x3B4,
 * = retail's DAT_0073A180) and the six 8-byte pace records at
 * `param + slot * 0x98 + 0x90` that FUN_00172870 reads @0x001728DD.  The
 * python tool's param-offset gate (0x3000..0xB800, 0x800-aligned, opponent
 * count 1..6) is reproduced exactly -- cxa_bgd_open's own bounds check is
 * looser and would admit the stale tool memory in the unused event slots.
 * No src/ header is emitted. */

#define CX_HAVE_PACE 1
int cx_extract_pace(const char *game_dir, const char *track_dir,
                    const char *track_id, const char *out_dir);

/* --- agent B --- */
/* geometry / textures / collision / envmap.
 *
 * All four are byte-for-byte ports; the python originals carry the format
 * provenance and the condensed [C] citations are preserved in the .c files.
 * Shared private helpers live in cx_common_b.[ch] (`cxb_` prefix) and the
 * PNG writer in cx_png.[ch].  Nothing here is track-specific: every offset
 * comes out of the files. */

#define CX_HAVE_TRACK 1
/* tools/extract_track.py -> <out_dir>/track.obj + <out_dir>/track.mtl.
 * The Xbox static.dat/streamed.dat world: 0x1C-byte vertices, triangle-strip
 * submeshes, the streamed units' PVS material-slot order (opaque pass, then
 * backdrop/water, then the global alpha pass, then the chevron group), the
 * 0x28-byte material records and the enviro.dat scene light/fog block.
 * cx_extract_track() is the stage-ABI name; cx_extract_track_mesh() is the
 * same function under the name the module was specified with. */
int cx_extract_track_mesh(const char *game_dir, const char *track_dir,
                          const char *track_id, const char *out_dir);
int cx_extract_track(const char *game_dir, const char *track_dir,
                     const char *track_id, const char *out_dir);

#define CX_HAVE_TEXTURES 1
/* tools/extract_textures.py -> <out_dir>/textures/<name>.png.  static.dat's
 * texture table (u16 count +0x16, ptr array +0x18 for version > 0x25); each
 * record is a prebaked Xbox D3DPixelContainer, DXT1 (0x0C) or DXT5 (0x0F) on
 * every shipped track.  PNG row 0 = surface row 0 = v 0; no flip anywhere. */
int cx_extract_textures(const char *game_dir, const char *track_dir,
                        const char *track_id, const char *out_dir);

#define CX_HAVE_COLLISION 1
/* tools/extract_collision.py -> <out_dir>/collision.bin ('B3CL' v1).  The
 * per-unit kd-tree soups in each streamed.dat LOD block (+0xA0 collision
 * header), u16[3] quantised vertices (world = u16/65536*1000 + cell*500) and
 * 0x0E-byte prims; quads split (i0,i1,i2)+(i2,i1,i3) per FUN_001B2940. */
int cx_extract_collision(const char *game_dir, const char *track_dir,
                         const char *track_id, const char *out_dir);

#define CX_HAVE_ENVMAP 1
/* tools/extract_envmap.py -> <out_dir>/envmap.png.  The car-body stage-1
 * reflection sheet: enviro.dat's +0xA0 texture-record slot, the one
 * FUN_00188880 relocates at 0x001888BF.  Absent on four tracks, in which case
 * nothing is written and the runtime keeps its probe fallback. */
int cx_extract_envmap(const char *game_dir, const char *track_dir,
                      const char *track_id, const char *out_dir);

/* --- agent C --- */
/* props / light probes / driver / diff harness. */

#define CX_HAVE_PROPS 1
/* tools/extract_props.py -> <out_dir>/props.bin ('B3PP').  The DESTRUCTIBLE
 * track props out of static.dat's second 0x70-record table (hdr +0x36/+0x3C
 * models, +0x40/+0x48 instance transforms, +0x44 prop class, +0x4C/+0x50 the
 * per-unit lists that bind instance to model). */
int cx_extract_props(const char *game_dir, const char *track_dir,
                     const char *track_id, const char *out_dir);

#define CX_HAVE_SCENERY 1
/* NO PYTHON ORACLE -- a NEW RECOVERY, see cx_scenery.c's header.  The
 * INSTANCED SCENERY out of static.dat's FIRST 0x70-record table (hdr +0x34
 * count / +0x38 table, FUN_001ADA40's own source) placed by the per-unit
 * counts/lists pair the streamed LOD block carries at +0xA8 (relocated by
 * FUN_0019D7A0 @0x0019D7D9 -> FUN_0019D760, base block+0xA8).  Palms, hero
 * trees, lamp posts, signage, benches, moored boats and parked vehicles: the
 * table extract_track.py:23 marks `(not extracted, [S])`.  Because it has no
 * oracle, verify_cextract.py cannot cover it -- tools/validate_scenery.py is
 * the gate, and it re-derives every offset from the shipped .dat files. */
int cx_extract_scenery(const char *game_dir, const char *track_dir,
                       const char *track_id, const char *out_dir);

#define CX_HAVE_BVH 1
/* NO PYTHON ORACLE and NO RETAIL COUNTERPART -- a DERIVED artefact, see
 * cx_bvh.c's header.  -> <out_dir>/bvh.bin ('B3BV' v1): a binned-SAH BVH2
 * over the STATIC world (the opaque track submeshes out of track.obj, every
 * prop placement out of props.bin and every scenery placement out of
 * scenery.bin, baked to world space), flattened depth-first with ESCAPE
 * INDICES so an ESSL 1.00 fragment shader can walk it with no stack.  It
 * serves the INSPIRED ray-traced sun shadow (docs/PHOTOREALISM.md tier 4r)
 * and is NOT a claim about Burnout 3 -- the Xbox had no such structure.
 *
 * It runs LAST in CX_STAGE_LIST because it reads three earlier stages'
 * output rather than the game files, the same ordering rule nav_edges has
 * against bgd_paths.  Because it has no oracle, verify_cextract.py cannot
 * cover it -- tools/validate_bvh.py is the gate, and it re-derives the
 * geometry from the same three artefacts and re-traces rays against a
 * brute-force reference. */
int cx_extract_bvh(const char *game_dir, const char *track_dir,
                   const char *track_id, const char *out_dir);

#define CX_HAVE_LIGHT_PROBES 1
/* tools/extract_light_probes.py -> <out_dir>/light_probes.bin ('B3LP').  The
 * 9-byte quantised SH probe per collision vertex that FUN_0019D400 samples by
 * casting the car's position down into the streamed unit's collision BSP. */
int cx_extract_light_probes(const char *game_dir, const char *track_dir,
                            const char *track_id, const char *out_dir);

/* --- agent G: gen --- */
/* The GENERATOR family: the C port of tools/gen_*.py.
 *
 * These are GLOBAL stages (see "GLOBAL STAGES" above), not per-track ones.
 * They are also the only family in this pipeline whose inputs are not all
 * under the game directory: they read the correctly-mapped XBE image
 * (build/burnout3.elf), Data/Globalus.bin, every track's Gamedata.bgd, AND the
 * port's own annotated headers under src/.  The repo root comes from
 * $B3_REPO_DIR (the driver's --repo sets it); nothing here ever WRITES inside
 * it -- every artefact goes to the caller-given out_root, so a generator run
 * can never clobber the src/ header it was derived from.
 *
 * Byte-for-byte agreement with the python original is the gate, and for these
 * that includes the generated-header COMMENT TEXT, not just the tables.
 *
 * Output goes to <out_root>/gen/<name>.h, the project-wide convention for a
 * generated header.  $B3_GEN_OUT (the driver's --gen-out) overrides that
 * directory outright, which is how one generator is aimed at a scratch path
 * for an oracle diff without moving the rest of the run. */

#define CX_GLOBAL_STAGES_G(X)   \
    X(gen_trackselect)          \
    X(gen_ai_pace)              \
    X(gen_ai_ranges)            \
    X(gen_crash_ranges)         \
    X(gen_retail_struct)        \
    X(gen_sfx_emitters)         \
    X(gen_vehicle_ranges)

#define CX_HAVE_GEN_TRACKSELECT 1
/* tools/gen_trackselect.py -> <out_root>/burnout3_trackselect.h.  Everything
 * the TRACK SELECT screen needs: Tracks/tlist.bin's 36 packed ids + flags, the
 * four parallel XBE tables at 0x0039EBC0/ECE0/ED70/EE00, the per-track
 * Gamedata.bgd lap counts and start-grid occupancy, the 28 region-map
 * locations at 0x0039F9B0/F990/F978/FA20, and the ten special events at
 * 0x0039E880/E8D0/E8F8.  Asserts the XBE id table decodes to the same 36 ids
 * as tlist.bin, so a misaligned table cannot slip through. */
int cx_extract_gen_trackselect(const char *game_dir, const char *out_root);
/* The python tool's --check: rebuild in memory and compare against the header
 * at `path` without writing.  0 = up to date, 1 = STALE. */
int cx_gen_trackselect_check(const char *game_dir, const char *path);

#define CX_HAVE_GEN_AI_PACE 1
/* tools/gen_ai_pace.py -> <out_root>/burnout3_ai_pace.h.  Every shipped
 * track's per-opponent AI pace records: `param + grid_slot * 0x98 + 0x90`, 8
 * bytes each, for every event whose param record offset is a sane multiple of
 * 0x800 and whose opponent count (param+0x3B4) is 1..6.  Note this generator
 * ships its OWN base-40 decode, without the reversal the tlist decoder does --
 * which is why its event ids read "FCRGSFFO". */
int cx_extract_gen_ai_pace(const char *game_dir, const char *out_root);

#define CX_HAVE_GEN_AI_RANGES 1
/* tools/gen_ai_ranges.py -> <out_root>/burnout3_ai_ranges.h.  The recovered
 * byte ranges of B3AiCar and B3AiState, read out of src/burnout3_ai.h's parity
 * assertions, plus the hand-listed vehicle interface FUN_00105340 actually
 * touches (checked against src/burnout3_vehicle_sim.h's assertions). */
int cx_extract_gen_ai_ranges(const char *game_dir, const char *out_root);

#define CX_HAVE_GEN_CRASH_RANGES 1
/* tools/gen_crash_ranges.py -> <out_root>/burnout3_crash_ranges.h.  The 26
 * vehicle fields FUN_0011AEF0 (chassis-vs-world resolve) touches. */
int cx_extract_gen_crash_ranges(const char *game_dir, const char *out_root);

#define CX_HAVE_GEN_RETAIL_STRUCT 1
/* tools/gen_retail_struct.py -> <out_root>/burnout3_vehicle_retail.h, plus the
 * DROPPED report on stdout.  B3VehicleFull's offset annotations, sorted and
 * laid out with explicit padding over retail's 0x1A00 span, one
 * _Static_assert per field. */
int cx_extract_gen_retail_struct(const char *game_dir, const char *out_root);

#define CX_HAVE_GEN_SFX_EMITTERS 1
/* tools/gen_sfx_emitters.py -> <out_root>/burnout3_sfx_emitters.h.  The retail
 * emitter behind each B3SfxEvent, matched between src/burnout3_sfx.h's enum
 * comments and tools/emulate_sfx.py's EMITTERS (which carries the calling
 * convention the enum comment does not). */
int cx_extract_gen_sfx_emitters(const char *game_dir, const char *out_root);

#define CX_HAVE_GEN_VEHICLE_RANGES 1
/* tools/gen_vehicle_ranges.py -> <out_root>/burnout3_vehicle_ranges.h.  The
 * recovered byte ranges of B3VehicleFull at retail's own offsets, with the
 * rigid body expanded into its own fields and the driver-owned / retail-
 * rebuilt sets split out. */
int cx_extract_gen_vehicle_ranges(const char *game_dir, const char *out_root);

/* --- agent F: audio --- */
/* The AUDIO family: RenderWare wave dictionaries and streams, the XACT wave
 * banks and the EA TRAX soundtrack.  These are NOT stage-ABI modules -- none
 * of them is per-track, so they take the DUMP-GLOBAL signature
 *
 *     int cx_extract_<name>(const char *game_dir, const char *out_root);
 *
 * and are absent from CX_STAGE_LIST.  Each one scans `game_dir` recursively
 * for its own extension, exactly as its python original does when handed the
 * game root, and lays its output out under `out_root` with the same directory
 * names.  Private helpers live in cx_audio_common.[ch] (`cxf_` prefix), which
 * carries literal ports of the CPython behaviours the outputs depend on --
 * above all the `wave` module's 44-byte canonical header.
 *
 * ffmpeg: the XWB and EA TRAX modules shell out to `ffmpeg` for the WMA
 * entries, as the python does.  Without it on PATH the XWB module still dumps
 * every .wma (and says so), and the EA TRAX module fails the track. */

#define CX_HAVE_AUDIO_AWD 1
/* tools/extract_awd.py -> <out_root>/awd_<dict>/<wave>.wav.  RenderWare Audio
 * WaveDictionaries (chunk 0x809): the sound/ banks, the per-track SOUND.AWD and
 * the 134 per-vehicle pveh/<CLASS>/Car*.hwd + .lwd engine banks.  All waves
 * are mono 16-bit PCM.  Where a dictionary name collides -- every .hwd names
 * itself "high" and every .lwd "low" -- the output directory is qualified
 * with the source path, awd_<path>_<dict>. */
int cx_extract_awd(const char *game_dir, const char *out_root);

#define CX_HAVE_AUDIO_RWS 1
/* tools/extract_rws.py -> <out_root>/rws_<relpath>/<stream>.wav.  RenderWare
 * audio streams (chunk 0x80D): the per-track CRASH1/2/3.RWS beds, 1 or 2
 * sub-streams each, de-interleaved from their 0x10000-byte cluster slices.
 * The 36 MUSIC.RWS files are 124-byte dummy wave dictionaries and are
 * skipped before the output directory is created, so they leave nothing. */
int cx_extract_rws(const char *game_dir, const char *out_root);

#define CX_HAVE_AUDIO_XWB 1
/* tools/extract_xwb.py -> <out_root>/<bankName>/NNN.wma + NNN.wav.  XACT v3
 * wave banks: the DJ/commentary banks, the twelve per-track E_DJRACE.xwb, the
 * two _EATraxN.xwb and ovid/movie.xwb.  Every entry in this game is tag 2
 * (WMA) and its play region is a standalone ASF file; the PCM and Xbox-ADPCM
 * paths are ported for completeness and are unexercised.  Set CX_NO_FFMPEG to
 * dump the .wma only (the python's --no-ffmpeg). */
int cx_extract_xwb(const char *game_dir, const char *out_root);

/* ONE bank, and nothing else.  What src/burnout3_isodata.c calls when the
 * game opens build/audio/<BANK>/NNN.wav in iso mode -- the stage above is the
 * whole-disc dump, 33 banks and 885 entries with the two 361 MB EA TRAX banks
 * in the middle of it, which is not a thing to do when Crash FM wants one
 * line.  `bank_name` is the OUTPUT directory name, i.e. the bank's own
 * BANKDATA name ("DJGEN", "US_C1"), not its filename -- on disc every one of
 * these carries a language prefix and the per-track banks are all called
 * E_DJRACE.xwb.  See the block comment above the definition. */
int cx_extract_xwb_one(const char *game_dir, const char *out_root,
                       const char *bank_name);

#define CX_HAVE_AUDIO_EATRAX 1
/* tools/extract_eatrax.py -> <out_root>/track_NN.wav + eatrax.txt.  The 44
 * licensed songs out of Tracks/_EATrax0.xwb and _EATrax1.xwb, decoded to
 * 44100 Hz MONO s16 because that is what the harness's SDL device wants.  The
 * manifest's artist/title/album come from the game's own song table (VA
 * 0x003EC458) resolved against Globalus.bin -- env B3_GLOBALUS, else
 * "build/Globalus.bin" relative to the working directory.  Env B3_EATRAX_ONLY
 * and B3_EATRAX_RATE mirror the python's --only / --rate. */
int cx_extract_eatrax(const char *game_dir, const char *out_root);

/* ONE song, no manifest, no Globalus.  What src/burnout3_isodata.c calls when
 * the game opens build/music/track_NN.wav in iso mode -- the whole family is
 * ~720 MB of PCM and about a minute of decoding, which is not a thing to do at
 * the first note of a race.  `index` is 0..43.  See the block comment above
 * the definition for what it deliberately does not do. */
int cx_extract_eatrax_one(const char *game_dir, const char *out_root,
                          int index);

/* Driver registration for the four global stages above.  `game_dir` may be
 * NULL or empty: each entry then falls back to $B3_GAME_DIR and finally to
 * the path the python tools hard-code. */
#define CX_GLOBAL_STAGES_F(X)   \
    X(awd)                      \
    X(rws)                      \
    X(xwb)                      \
    X(eatrax)

/* --- agent E: art --- */
/* The ART / FRONTEND family: the two Data .txd banks, the compiled-in fonts,
 * and the FX art the car/boost/particle/post-FX modules consume.
 *
 * These are DUMP-GLOBAL stages, not per-track ones, so they do NOT use the
 * per-track stage ABI above.  Their signature is
 *
 *     int cx_extract_<name>(const char *game_dir, const char *out_root);
 *
 * `game_dir` may be NULL/empty, in which case $B3_GAME_DIR and then the
 * shipped default the python tools hard-code are used.  `out_root` stands in
 * for the REPO ROOT: every tool writes exactly the repo-relative path its
 * python original writes, so an oracle check is a plain directory diff --
 *
 *     <out_root>/build/frontend/<name>.png  txd + font atlases
 *     <out_root>/src/burnout3_font.h      font metrics
 *     <out_root>/build/carfx/           carfx art + env_light.txt
 *     <out_root>/build/cars/<car>.lights   per-car corona/shadow
 *     <out_root>/build/postfx/          sky art, env.txt, manifest
 *     <out_root>/build/boostfx/<n>.png     exhaust-flame art
 *     <out_root>/build/particlefx/<n>.png  crash dust/debris art
 *
 * Private helpers live in cx_art_common.[ch] (`cxe_` prefix); every format
 * decoder is REUSED from cx_common_b.[ch].  Return 0 on success.
 *
 * NOTE ON CWD: extract_postfx_art.py records os.path.relpath() of each output
 * in its manifest, i.e. paths relative to the PROCESS CWD.  The C reproduces
 * posixpath.relpath() exactly, so run it with cwd == out_root to get the same
 * manifest the python produces with cwd == the repo root. */

/* Driver registration for the six global stages below.  `game_dir` may be
 * NULL or empty: each entry then falls back to $B3_GAME_DIR and finally to
 * the path the python tools hard-code.  Order matters once: boostfx_art's
 * type-8 emitter report reads the .lights files carfx_art writes. */
#define CX_GLOBAL_STAGES_E(X)   \
    X(txd)                      \
    X(font)                     \
    X(carfx_art)                \
    X(boostfx_art)              \
    X(particlefx_art)           \
    X(postfx_art)

#define CX_HAVE_ART_TXD 1
/* tools/extract_txd.py -> <out_root>/build/frontend/<name>.png.  Both retail
 * banks (Frontend.txd then Global.txd), the flat Criterion container, DXT1 /
 * DXT5 / Morton-swizzled paletted.  ALWAYS writes the `--all-palettes`
 * superset (_p1.._pN for every multi-palette texture).  The one cross-bank
 * name collision ("Takedown") takes a _<bank> suffix on the later bank.
 * Returns non-zero if any texture fails to decode or fails the
 * V-ORIGIN/ALPHA self-check on hud_element01. */
int cx_extract_txd(const char *game_dir, const char *out_root);
/* the same, with the python's explicit input list / palette switch */
int cx_extract_txd_banks(const char *const *inputs, int ninputs,
                         const char *outdir, int all_palettes, int verbose);

#define CX_HAVE_ART_FONT 1
/* tools/extract_font.py -> <out_root>/build/frontend/{GlobalFont,HeadFont,
 * SmallFont}.png + <out_root>/src/burnout3_font.h.  Reads the XBE image, not
 * the game dir: $B3_ELF, else "build/burnout3.elf" relative to the CWD. */
int cx_extract_font(const char *game_dir, const char *out_root);

#define CX_HAVE_ART_CARFX 1
/* tools/extract_carfx_art.py -> <out_root>/build/carfx/{blobbyshadow,
 * coronaglow}.png + env_light.txt, and <out_root>/build/cars/<car>.lights (the
 * per-car corona light table and shadow-quad sources out of the .bgv). */
int cx_extract_carfx_art(const char *game_dir, const char *out_root);

#define CX_HAVE_ART_BOOSTFX 1
/* tools/extract_boostfx_art.py -> <out_root>/build/boostfx/{coronaboost,
 * coronaboostred}.png (sprite pools 1 and 2 of FUN_0017EE00's table). */
int cx_extract_boostfx_art(const char *game_dir, const char *out_root);

#define CX_HAVE_ART_PARTICLEFX 1
/* tools/extract_particlefx_art.py -> <out_root>/build/particlefx/fx<n>.png, the
 * crash dust / smoke / debris family out of Data/Global.txd. */
int cx_extract_particlefx_art(const char *game_dir, const char *out_root);

#define CX_HAVE_ART_POSTFX 1
/* tools/extract_postfx_art.py -> <out_root>/build/postfx/<REG>_<TRK>_<role>.png
 * plus <REG>_<TRK>_env.txt and enviro_manifest.txt.  The dump-global entry
 * runs the python's `--all` (every track that ships an enviro.dat); the _ex
 * form takes a single "REG/Cn_Vn" track and the `--scan` cross-check. */
int cx_extract_postfx_art(const char *game_dir, const char *out_root);
int cx_extract_postfx_art_ex(const char *game_dir, const char *out_root,
                             const char *only_track, int do_scan);

/* --- agent D: cars --- */
/* The CAR / VEHICLE family: the .bgv player fleet, the .btv traffic fleet,
 * their paint pages, their corona tables, the roster header and the per-car
 * ValueDB tuning.  Modules: cx_cars_common.c (the shared .bgv/.btv reader and
 * OBJ writer, private prefix `cxd_`, contract in cx_cars.h), cx_cars_bgv.c,
 * cx_cars_paint.c, cx_cars_lights.c, cx_cars_roster.c, cx_cars_vdb.c,
 * cx_cars_traffic.c.
 *
 * These are DUMP-GLOBAL stages, not per-track: the fleet lives in pveh/ and
 * does not vary by track, so they take
 *
 *     int cx_extract_<name>(const char *game_dir, const char *out_root);
 *
 * with `out_root` a REPO-ROOT STAND-IN (the project-wide convention, shared
 * with agent E): the vehicle assets land in <out_root>/build/cars/, exactly
 * the tree the python tools build, and the generated headers in
 * <out_root>/gen/.  Both subtrees are created by the stages themselves.  The
 * one exception is cx_extract_traffic_cars(), which IS per-track and
 * therefore carries the ordinary four-argument stage ABI.
 *
 * <out_root>/build/cars/ is deliberately the SAME directory agent E's
 * carfx_art stage writes the 67 .bgv .lights into: this family supplies the
 * 40 .btv .lights, and src/burnout3_carfx.c parses both fleets with one
 * parser, so the halves have to land together.
 *
 * Because they are dump-global they are NOT members of CX_STAGE_LIST (which
 * the driver runs once per track); they are registered in CX_GLOBAL_STAGES_D
 * below.  Their macros are still CX_HAVE_*, so a driver can test for them the
 * same way, and each is compiled only when its .c file is present.
 *
 * cx_extract_traffic_cars() is the odd one out: it is PER-TRACK and so keeps
 * the four-argument ABI.  It is deliberately NOT added to CX_STAGE_LIST here
 * -- that list is the driver's, and this block may not reach outside itself;
 * the driver owner can append `X(TRAFFIC_CARS, traffic_cars)` after TRAFFIC
 * whenever the per-track sweep should carry the .btv mirror.
 *
 * The python originals stay THE SPEC and every emitted byte matches them,
 * quirks included; the reproduced quirks are enumerated Q1..Q26 in the module
 * headers (cx_cars_bgv.c Q1-Q9, cx_cars_paint.c Q10, cx_cars_roster.c
 * Q11-Q12, cx_cars_traffic.c Q13-Q17, cx_cars_vdb.c Q18-Q21,
 * cx_cars_physparams.c Q22-Q26). */

#define CX_GLOBAL_STAGES_D(X)   \
    X(car_meshes)               \
    X(car_paint)                \
    X(traffic_lights)           \
    X(vehicle_roster)           \
    X(car_tuning)
/* cx_extract_physics_params() is deliberately NOT in that list: it is the one
 * module here that mines Ghidra rather than the game files, so it must not be
 * pulled into a cold `--all-global` dump run.  cx_main.c registers it instead
 * through CX_GLOBAL_MANUAL_D, keyed on CX_HAVE_PHYSICS_PARAMS below, so
 * `--only physics_params` still reaches it. */

#define CX_HAVE_CAR_MESHES 1
/* tools/extract_bgv.py -> <out_root>/build/cars/<CLS>_<CarN>{,_intact,
 * _shell,_glass,_wheel}.obj + .wheels + .panels + parts/<CLS>_<CarN>/{panel<K>_
 * kind<D>,wheel_slot<S>}.obj, over every pveh/<CLASS> .bgv file.  Layout from
 * the game's relinker FUN_000310F0 / FUN_00031010: 0x18-byte vertices with a
 * NORMPACKED3 normal, u16 triangle strips, the 18-slot LOD part table and the
 * embedded one-piece intact car at S+0x60.  Highest-detail LOD wins. */
int cx_extract_car_meshes(const char *game_dir, const char *out_root);

#define CX_HAVE_CAR_PAINT 1
/* tools/extract_bgv_textures.py -> <out_root>/build/cars/<CLS>_<CarN>_p<K>.png, one
 * per colour variant.  The .bgv's own 8bpp paletted texture record (header
 * +0x60), Morton/Z-order unswizzled, palettes at record+0x14 x record+0x69.
 * PIXEL identity with PIL is the gate, not byte identity. */
int cx_extract_car_paint(const char *game_dir, const char *out_root);

#define CX_HAVE_TRAFFIC_LIGHTS 1
/* tools/extract_traffic_lights.py -> <out_root>/build/cars/<CLS>_<CarN>.lights for
 * the 40 shipped .btv.  The corona table the emitter FUN_001879E0 /
 * FUN_00187AC0 reads: u32[12] offsets at model+0x1664, u8[12] counts at
 * model+0x16AC, 0x30-byte records {float4 pos, float4 normal, float4 aux}
 * [C].  Every file is re-verified (range, finiteness, plausibility, and
 * headlights forward of tail lights) before anything is written; a failure
 * refuses that file AND fails the stage, as the python's SystemExit does. */
int cx_extract_traffic_lights(const char *game_dir, const char *out_root);

#define CX_HAVE_VEHICLE_ROSTER 1
/* tools/extract_vehicles.py -> <out_root>/gen/burnout3_vehicle_data.h: the
 * 107-vehicle roster out of pveh/vlist.bin plus every .bgv/.btv header
 * (magic 0x17 at +0x00, self-describing size at +0x08 cross-checked against
 * the file, variant word +0x0C, section count +0x10, dims +0x14/+0x18).
 * The python writes src/; this pipeline writes <out_root>/gen/ with identical
 * CONTENT, the convention adopted project-wide for generated headers. */
int cx_extract_vehicle_roster(const char *game_dir, const char *out_root);

#define CX_HAVE_CAR_TUNING 1
/* tools/extract_car_vdb.py `generate` -> <out_root>/gen/burnout3_car_physics.h:
 * every car's physics overrides out of the retail Data/vdb.xml, keyed by the
 * game's own registration hash -- key "<param><group>/../Export/ValueDB/
 * VehiclePhysics/<VLIST-ID>.cfg", table-CRC at 0x001AF250 with SAR semantics
 * over the 256 dwords at VA 0x003F7700.  Reads two files outside game_dir,
 * exactly as the python does: <repo>/src/burnout3_physics_params.h for the 64
 * (offset, group, name) rows and <repo>/build/burnout3.elf for the CRC table
 * (<repo> is the driver's --repo / $B3_REPO_DIR).  The python's Unicorn re-emulation
 * gate is replaced by a standing hash self-check -- see the module header. */
int cx_extract_car_tuning(const char *game_dir, const char *out_root);

#define CX_HAVE_PHYSICS_PARAMS 1
/* tools/extract_physics_params.py -> <out_root>/gen/burnout3_physics_params.h:
 * name, group and struct offset for all 64 tunables the registrar
 * FUN_00132D10 hands the ValueDB, plus the compiled-in defaults from
 * FUN_00132950.  UNLIKE EVERY OTHER MODULE HERE this one is not self-
 * contained: the python mines Ghidra, and a faithful port mines the same
 * Ghidra -- it speaks HTTP to the same MCP bridge ($B3_GHIDRA_MCP, default
 * 127.0.0.1:8089) and calls the same two endpoints.  With the bridge down it
 * fails cleanly and writes nothing.  It is a REGENERATION tool, not part of a
 * cold asset dump: the header it produces is the checked-in
 * src/burnout3_physics_params.h that cx_extract_car_tuning() reads back. */
int cx_extract_physics_params(const char *game_dir, const char *out_root);

#define CX_HAVE_TRAFFIC_CARS 1
/* The vehicle-asset half of tools/extract_traffic.py (export_assets /
 * extract_btv) -> <out_dir>/cars/ AND the dump-global build/cars mirror:
 * <CLS>_<CarN>.obj + _wheel.obj + .wheels + _p<K>.png for the track's traffic
 * fleet.  PER-TRACK, so it keeps the four-argument stage ABI.  It reads the
 * roster back out of <out_dir>/traffic.bin, so it must run AFTER the TRAFFIC
 * stage (the same ordering rule nav_edges has against bgd_paths).  out_dir is
 * <repo>/build/tracks/<ID> under the repo-root convention, so the mirror --
 * <out_dir>/../../cars, unless $B3_CARS_OUT overrides it -- lands exactly on
 * the <out_root>/build/cars the dump-global stages write.  Porting this
 * closes the last cars/ gap, so tools/extract_traffic.py can retire. */
int cx_extract_traffic_cars(const char *game_dir, const char *track_dir,
                            const char *track_id, const char *out_dir);

/* --- hull --- */
/* The per-vehicle COLLISION HULL: <out_root>/build/cars/<CLS>_<CarN>.hull,
 * one 0x600-byte convex-polyhedron record per .bgv player car and per .btv
 * traffic car.  Module: cx_cars_hull.c, private prefix `cxh_`.
 *
 * DUMP-GLOBAL, so it takes the two-argument global ABI, and it writes into the
 * same <out_root>/build/cars/ the agent-D car family and agent E's carfx use
 * -- deliberately, because src/burnout3_carcol.c's b3_carcol_hull_load() reads
 * these files from exactly that directory alongside the meshes and .lights.
 * It shares no code with agent D and touches no file it owns.
 *
 * It retires the last non-Unicorn job left in tools/emulate_carcol.py
 * (`--extract-hulls`).  There is nothing to emulate: retail does not BUILD the
 * hull, it copies it.  FUN_00122830 @[C] 0x001229EE forms
 * `*(model + 0x40) + 0x1060` and hands it to FUN_00122C20, a pure
 * section-by-section move into veh+0x220 whose six trip counts define the
 * record (40 planes / 22 verts / 60 edges) and pin its size at exactly 0x600;
 * FUN_0012E4D0 and FUN_00125BF0 form the same pair.  So this stage is a
 * verbatim byte window out of the container plus the python's two gates
 * (short file, implausible counts) -- no arithmetic at all, and in particular
 * no floating point, so the output is bit-exact by construction rather than by
 * matching an operation order.  Full transcription in the module header.
 *
 * Registered through CX_GLOBAL_STAGES_EXTRA, cx_main.c's documented escape
 * hatch for a stage outside the seven lettered blocks: this is not agent D's
 * family and its block may not be edited from here. */
#define CX_GLOBAL_STAGES_EXTRA(X)   \
    X(hulls)                        \
    X(car_bvh)

#define CX_HAVE_HULLS 1
int cx_extract_hulls(const char *game_dir, const char *out_root);

/* --- the CARS' ray-tracing tree --- */
/* <out_root>/build/cars/carbvh.bin ('B3CV' v1): one MODEL-SPACE BVH per
 * vehicle -- both fleets, 67 .bgv + 40 .btv -- in one file, for the optional
 * ray-traced sun shadow's two-level trace.  Module: tools/cextract/cx_car_bvh.c.
 *
 * The static world's tree (cx_bvh.c) cannot hold a car, because a BVH is built
 * once and a car moves every frame.  A car is RIGID, though, so its tree is
 * built ONCE in model space and the per-frame cost is one 3x4 matrix per
 * instance; src/burnout3_aftereffects.c uploads those as uniforms and the
 * shadow ray transforms itself into each car's space rather than anything
 * being rebuilt.  Both artefacts are flattened by the SAME builder
 * (tools/cextract/cx_bvh_build.c) so the two cannot drift apart from the one
 * ESSL 1.00 traversal that walks them.
 *
 * DUMP-GLOBAL, and it reads the .bgv/.btv CONTAINERS through cx_cars_common.c
 * rather than the OBJs that cx_cars_bgv.c writes -- deliberately, because the
 * traffic fleet's OBJs are written PER TRACK and a global artefact may not
 * depend on which tracks somebody happened to visit first.  That makes "is
 * this the mesh the renderer draws" a claim rather than a construction, so it
 * is gated: tools/validate_car_bvh.py re-derives the triangle set from
 * build/cars/<NAME>_intact.obj and compares it as a multiset.
 *
 * NO PYTHON ORACLE and no retail counterpart -- the Xbox drew a blobbyshadow
 * quad and nothing else.  INSPIRED; see src/burnout3_rt.h.
 *
 * Registered through CX_GLOBAL_STAGES_EXTRA for the same reason `hulls` is:
 * it is not agent D's family and that block may not be edited from here. */
#define CX_HAVE_CAR_BVH 1
int cx_extract_car_bvh(const char *game_dir, const char *out_root);

/* --- purge 2: the RUNTIME ASSETS that retire the last compiled-in tables ---
 *
 * Phase 1 of the compiled-in-game-data purge took five generated headers out of
 * src/ (track_paths, start_grid, ai_pace, trackselect, traffic_data).  Three
 * were left, because they are not per-track and so had no per-track asset to
 * move into:
 *
 *     src/burnout3_car_physics.h    every car's Data/vdb.xml tuning, 332 KB
 *     src/burnout3_vehicle_data.h   the 107-entry pveh/ roster
 *     src/burnout3_font.h           the three XBE font atlases' glyph metrics
 *
 * All three are GAME DATA -- vdb.xml values, .bgv/.btv header fields and the
 * publisher's font -- so a user-supplies-assets distribution cannot compile
 * them in either.  This block defines the RUNTIME ASSETS that replace them.
 *
 * THERE IS NO NEW STAGE.  Each artefact is emitted by the stage that ALREADY
 * walks that data and generates the header, one extra writer at the point
 * where every value is already in memory:
 *
 *     cx_cars_vdb.c      car_tuning      -> <out_root>/build/cars/car_physics.bin
 *     cx_cars_roster.c   vehicle_roster  -> <out_root>/build/cars/roster.bin
 *     cx_art_font.c      font            -> <out_root>/build/frontend/font.bin
 *
 * so `--all-global` produces them on a cold dump with no extra step, and each
 * stays byte-for-byte in step with the header it replaces by construction.
 * The generated headers are STILL written, unchanged: they are the python
 * originals' output and the verify harnesses' oracle, and nothing in src/
 * includes them any more.
 *
 * ============================================================ THE PRECISION
 * The three .bin files carry EXACTLY THE NUMBERS THE HEADER CARRIED, not the
 * raw ones -- the header prints dim_a/dim_b with "%.5f", the font's UVs with
 * "%.6f" and its pixel metrics with "%.1f", so a full-precision asset would
 * NOT reproduce the compiled table and the port's [S-ref] HUD offsets were
 * tuned against the rounded values.  Each emitter therefore formats the value
 * with the header's own format and parses it back before storing the f32,
 * which makes the loaded table bit-identical to the compiled one (proved by a
 * dump diff, and asserted by tools/validate_no_baked_data.py section 3).
 * car_physics.bin needs no such step: "%.9g" already round-trips a float.
 * The full-precision source is one re-run away if a later revision wants it.
 *
 * ============================================================== NO ORACLE
 * The python tools never wrote these files, so -- exactly like cx_scenery.c --
 * verify_cextract.py has nothing to diff them against.  It reports the three
 * paths as SKIP (no python oracle) rather than as extras; verify_cx_art.py
 * does the same for font.bin.  The gate for them is
 * tools/validate_no_baked_data.py, which compares the LOADED table against the
 * bytes the extractor wrote and against the game's own files.
 *
 *   car_physics.bin  'B3CP' v1
 *     +0x00 char[4] 'B3CP'  +0x04 u32 version=1
 *     +0x08 u32 car_count   +0x0C u32 param_total (sum of the n_params below)
 *     +0x10 car_count x 40-byte car record, in vlist order:
 *             char[16] id          NUL-padded, "COMPCAR1"
 *             char[8]  class_code  "COMP"
 *             char[12] file        "Car10.bgv" / "Car3.btv"
 *             u32      n_params    64 drivable / 9 traffic
 *     then param_total x 8-byte record, cars in the same order, params sorted
 *     by struct offset:
 *             u16 offset  (into the game's 0x1D0 physics struct)
 *             u16 pad = 0
 *             f32 value
 *
 *   roster.bin  'B3VR' v1
 *     +0x00 char[4] 'B3VR'  +0x04 u32 version=1
 *     +0x08 u32 count       +0x0C u32 stride = 64
 *     +0x10 u32 vlist_version  +0x14 u32 vlist_declared_count  (0 = no vlist)
 *     +0x18 count x 64-byte record, in the header's own class/extension order:
 *             char[16] file        "Car10.bgv"
 *             char[8]  class_code  "COMP"
 *             char[16] class_name  "Compact"
 *             u32 kind             0 = player (.bgv), 1 = traffic (.btv)
 *             u32 data_size        the actual file size (== header +0x08)
 *             u32 variant          header +0x0C
 *             u32 sections         header +0x10
 *             f32 dim_a            header +0x14, quantised to "%.5f"
 *             f32 dim_b            header +0x18, quantised to "%.5f"
 *
 *   font.bin  'B3FN' v1
 *     +0x00 char[4] 'B3FN'  +0x04 u32 version=1
 *     +0x08 u32 font_count  +0x0C u32 glyphs_per_font = 95   (0x20..0x7E)
 *     +0x10 font_count x 3844-byte record, GlobalFont / HeadFont / SmallFont:
 *             char[32] name        the atlas basename in build/frontend
 *             u32 tex_w  u32 tex_h
 *             f32 line_h           "%.3f"
 *             95 x 40-byte glyph:
 *               f32 u0,v0,u1,v1    "%.6f"
 *               f32 w,h            "%.1f", atlas px
 *               f32 xoff,yoff      "%.1f", atlas px
 *               f32 advance        "%.1f", atlas px
 *               u8  present        0 = the font has no glyph for this char
 *               u8  pad[3] = 0
 *
 * Little-endian throughout; no coordinates, so no GL reflection applies.
 * The loaders are src/burnout3_car_physics_runtime.h,
 * src/burnout3_vehicle_data_runtime.h and src/burnout3_font_runtime.h. */
#define CX_HAVE_RUNTIME_ASSETS 1
#define CX_RT_CAR_PHYSICS_BIN "build/cars/car_physics.bin"
#define CX_RT_ROSTER_BIN      "build/cars/roster.bin"
#define CX_RT_FONT_BIN        "build/frontend/font.bin"

#endif /* CX_EXTRACT_H */
