# ARCHIVED Python extractors — reference implementations

These tools are **retired from active use**. The per-track extraction
pipeline is now the C extractor in `tools/cextract/` (`cxtract`), which was
validated **byte-identical** (pixel-identical for PNGs) against these exact
scripts over every shipped track before the switch — see
`tools/cextract/verify_cextract.py`, the permanent differential gate.

They are kept, unmodified, for three reasons:

1. **They are the oracle.** Any change to the C extractor must re-pass the
   byte-identity gate against a fresh run of these scripts. Never edit an
   archived tool to make the gate pass — the archive is the spec.
2. **They carry the recovered format knowledge** with its full provenance
   commentary ([C] Ghidra addresses, [S] source-verified markers), in the
   form it was originally recovered.
3. **Library consumers.** Several validators import these as parsing
   libraries. Forwarding stubs remain at the old `tools/<name>.py` paths, so
   `import extract_tlist` keeps working; the stubs execute the archived
   module unchanged.

Archived here (superseded by the named `cxtract` stage):

| archived tool | cxtract stage |
|---|---|
| extract_tlist.py | tlist / cx_resolve_track |
| extract_bgd_paths.py | bgd_paths (route.bin, traffic_paths.bin) |
| extract_track.py | track_mesh (track.obj/.mtl) |
| extract_collision.py | collision |
| extract_textures.py | textures |
| extract_envmap.py | envmap |
| extract_props.py | props |
| extract_light_probes.py | light_probes |
| extract_nav_edges.py | nav_edges |
| extract_start_grid.py | grid (grid.bin only; the src header emitter
|                       | exists only here, for pinned-header regeneration) |

Still LIVE in `tools/` (no C port yet): `extract_traffic.py` (only its
**vehicle-asset** half — `build/cars/*`; its track-side `traffic.bin` output
is superseded), `extract_txd.py` (shared RenderWare texture-dictionary
library for the car/art family), the car family (`extract_bgv*`,
`extract_car_vdb`, `extract_vehicles`, `extract_traffic_lights`,
`extract_physics_params`), the art family (`extract_font`, `extract_*_art`),
and the audio family (`extract_awd`, `extract_rws`, `extract_xwb`,
`extract_eatrax`). Each moves here when its C port passes the gate.
