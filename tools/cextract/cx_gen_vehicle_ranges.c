/* cx_gen_vehicle_ranges.c -- the C port of tools/gen_vehicle_ranges.py.
 *
 * Emit the byte ranges of B3VehicleFull that are RECOVERED.
 *
 * Shape parity put our fields at retail's offsets, but the port models only
 * the fields we have recovered -- the rest of retail's 0x1A00 object is padding
 * here, and the emulator seeds real values there.  Writing the whole window
 * over the top replaces that seed with zeros and hangs retail's substep loop.
 *
 * So the transfer covers exactly the recovered ranges: same offset on both
 * sides, no field converted, and everything we have NOT recovered is left as
 * the emulator seeded it.  This reads the parity assertions -- the same ones
 * that guarantee the offsets -- so the table cannot drift from the struct.
 *
 * NOTE the optional 0x.  The python original's pattern used to REQUIRE the
 * prefix, and the rigid body is asserted as `offsetof(B3VehicleFull, rb) == 0`
 * -- no prefix -- so the WHOLE BODY silently fell out of the transfer:
 * velocity, heading, omega, angular momentum and all four accumulators.
 * physics=retail was handing the game a vehicle with no motion state and
 * taking back only the frame, which is position without the velocity that
 * produced it.  cxg_scan_assert's `need_0x` is 0 here for exactly that reason.
 */
#include "cx_common_g.h"
#include "cx_extract.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CXG_VR_MAX 1024

/* THE DRIVER BLOCK -- owned by whoever runs the DRIVER, not by the physics.
 *
 * b3_ai_drive writes its inputs AND its carried scratch straight into the
 * vehicle (that is what removing the invented B3AiInputs bought).  Under
 * physics=retail the body is retail's, but the driver is still the port's, so
 * mirroring these back over the port every frame wipes the slew memory at
 * v+0x1408 and the dither/stuck timers at v+0x156C..0x157C.  The AI then stops
 * commanding throttle and the car coasts to a standstill with thr=0.00.
 *
 * Retail receives the live inputs as arguments to the step, so it never needs
 * them mirrored back; and when ai=retail the AI bridge reads its own
 * B3_AIVEH_RANGES.  Excluded from the mirror in both cases. */
static const char *const CXG_DRIVER_OWNED[] = {
    "throttle_1400", "brake_1404", "steer_1408", "throttle_raw_1414",
    "input_bits_13FC", "prev_throttle_156C", "brake_hold_1570",
    "dither_1574", "stuck_arm_1578", "reverse_timer_157C",
};

/* THE HANDOVER SET.
 *
 * B3_VEHICLE_RANGES is what we READ BACK: everything recovered, so the port
 * mirrors whatever retail did.  It is NOT what we may WRITE.
 *
 * A retail-owned step is a co-simulation, not a snapshot compare.  Retail owns
 * the vehicle while it drives it, so the port hands its state over ONCE and
 * then only mirrors.  Writing every frame is what dropped the car through the
 * floor: these fields are retail's to compute, and the port's copies go stale
 * the moment its own step stops running.
 *
 *   rb.inv_inertia_world  rebuilt every step as Rt*I0*R
 *   rb.inv_frame          rebuilt every step from the frame
 *   rb.force_acc/torque_acc/imp_force/imp_torque/deflection
 *                         per-substep accumulators the integrator clears
 *
 * The struct's own comments already say "(rebuilt)" and "(cleared)" on every
 * one of them -- the classification was in the tree, just never acted on. */
static const char *const CXG_DERIVED[] = {
    "rb.inv_inertia_world", "rb.inv_frame", "rb.force_acc",
    "rb.torque_acc", "rb.imp_force", "rb.imp_torque", "rb.deflection",
};

static int cxg_in_set(const char *const *set, size_t n, const char *name)
{
    size_t i;

    for (i = 0; i < n; i++)
        if (!strcmp(set[i], name))
            return 1;
    return 0;
}

static void cxg_sort_stable(cxg_assert_hit *h, int n)
{
    int i, j;

    for (i = 1; i < n; i++) {
        cxg_assert_hit k = h[i];

        for (j = i - 1; j >= 0 && h[j].off > k.off; j--)
            h[j + 1] = h[j];
        h[j + 1] = k;
    }
}

static int cxg_vr_build(cxg_str *L, const char *repo, int *n_names)
{
    char            path[4096];
    char           *src = NULL;
    cxg_assert_hit *names = NULL, *body = NULL;
    int             nn = 0, nb = 0, i, rc = 1;
    int             have_rb = 0;
    uint32_t        rb_off = 0;

    cxg_join(path, sizeof(path), repo, "src/burnout3_vehicle_sim.h");
    src = cxg_read_text(path, NULL);
    if (!src)
        goto done;
    names = (cxg_assert_hit *)malloc(sizeof(*names) * CXG_VR_MAX);
    body  = (cxg_assert_hit *)malloc(sizeof(*body) * CXG_VR_MAX);
    if (!names || !body)
        goto done;

    nn = cxg_scan_assert(src, "B3VehicleFull", 0, names, CXG_VR_MAX);
    nb = cxg_scan_assert(src, "B3RigidBody", 0, body, CXG_VR_MAX);

    /* `frame` is a HOST POINTER (GLUE +0x140, retail keeps its own at v+0x204)
     * and must never be written into the emulator. */
    {
        int w = 0;

        for (i = 0; i < nb; i++)
            if (strcmp(body[i].name, "frame") != 0)
                body[w++] = body[i];
        nb = w;
    }

    /* dict(names).get("rb") -- the last row wins, as a dict does. */
    for (i = 0; i < nn; i++)
        if (!strcmp(names[i].name, "rb")) {
            rb_off = names[i].off;
            have_rb = 1;
        }

    if (have_rb) {
        int w = 0;

        for (i = 0; i < nn; i++)
            if (strcmp(names[i].name, "rb") != 0)
                names[w++] = names[i];
        nn = w;
        /* The body is a nested struct, so it expands into ITS recovered fields
         * rather than moving as one blob. */
        for (i = 0; i < nb && nn < CXG_VR_MAX; i++) {
            snprintf(names[nn].name, sizeof(names[nn].name), "rb.%s",
                     body[i].name);
            names[nn].off = rb_off + body[i].off;
            nn++;
        }
    }
    cxg_sort_stable(names, nn);
    *n_names = nn;

    cxg_sline(L, "// GENERATED by tools/gen_vehicle_ranges.py -- do not edit by hand.");
    cxg_sline(L, "//");
    cxg_sline(L, "// The recovered byte ranges of B3VehicleFull, at retail's own offsets.");
    cxg_sline(L, "// Built from the parity assertions, so it cannot drift from the struct.");
    cxg_sline(L, "//");
    cxg_sline(L, "// Used to hand retail our state without destroying the parts of its");
    cxg_sline(L, "// vehicle object we have not recovered: each range is written at the SAME");
    cxg_sline(L, "// offset on both sides, and everything else is left as the emulator");
    cxg_sline(L, "// seeded it. No field is converted in either direction.");
    cxg_sline(L, "#ifndef BURNOUT3_VEHICLE_RANGES_H");
    cxg_sline(L, "#define BURNOUT3_VEHICLE_RANGES_H");
    cxg_nl(L);
    cxg_sline(L, "#include <stddef.h>");
    cxg_sline(L, "#include \"burnout3_vehicle_sim.h\"");
    cxg_nl(L);
    cxg_sline(L, "typedef struct { unsigned off, len; } B3VehicleRange;");
    cxg_nl(L);
    cxg_sline(L, "#define B3_VEH_MEMBER_LEN(f) ((unsigned)sizeof(((B3VehicleFull*)0)->f))");
    cxg_nl(L);
    cxg_sline(L, "static const B3VehicleRange B3_VEHICLE_RANGES[] = {");
    for (i = 0; i < nn; i++) {
        if (cxg_in_set(CXG_DRIVER_OWNED,
                       sizeof(CXG_DRIVER_OWNED) / sizeof(CXG_DRIVER_OWNED[0]),
                       names[i].name))
            continue;
        cxg_sline(L, "    { (unsigned)offsetof(B3VehicleFull, %s), "
                     "B3_VEH_MEMBER_LEN(%s) },", names[i].name, names[i].name);
    }
    cxg_sline(L, "};");
    /* The count macro's name does not match the table it measures.  That is
     * the python original's text, and this port reproduces it rather than
     * quietly fixing it -- byte identity is the gate. */
    cxg_sline(L, "#define B3_VEHICLE_STATE_RANGE_COUNT "
                 "(sizeof(B3_VEHICLE_STATE_RANGES)/sizeof(B3_VEHICLE_STATE_RANGES[0]))");
    cxg_nl(L);
    cxg_sline(L, "// The state the port HANDS OVER when retail takes the wheel. Excludes");
    cxg_sline(L, "// everything retail rebuilds or clears itself -- see gen_vehicle_ranges.py.");
    cxg_sline(L, "static const B3VehicleRange B3_VEHICLE_STATE_RANGES[] = {");
    for (i = 0; i < nn; i++) {
        if (cxg_in_set(CXG_DERIVED,
                       sizeof(CXG_DERIVED) / sizeof(CXG_DERIVED[0]),
                       names[i].name))
            continue;
        cxg_sline(L, "    { (unsigned)offsetof(B3VehicleFull, %s), "
                     "B3_VEH_MEMBER_LEN(%s) },", names[i].name, names[i].name);
    }
    cxg_sline(L, "};");
    cxg_nl(L);
    cxg_sline(L, "#define B3_VEHICLE_RANGE_COUNT "
                 "(sizeof(B3_VEHICLE_RANGES) / sizeof(B3_VEHICLE_RANGES[0]))");
    cxg_nl(L);
    cxg_sputs(L, "#endif // BURNOUT3_VEHICLE_RANGES_H\n");
    rc = 0;

done:
    free(names);
    free(body);
    free(src);
    return rc;
}

int cx_extract_gen_vehicle_ranges(const char *game_dir, const char *out_root)
{
    const char *repo = cxg_repo_root();
    cxg_str     L = { 0 };
    char        out[4096], rel[4096];
    int         nn = 0, rc;

    (void)game_dir;
    cxg_gen_path(out, sizeof(out), out_root, "burnout3_vehicle_ranges.h");
    rc = cxg_vr_build(&L, repo, &nn);
    if (rc == 0)
        rc = cxg_str_write(&L, out);
    cxg_str_free(&L);
    if (rc == 0) {
        cxg_relpath(rel, sizeof(rel), out, repo);
        printf("wrote %s: %d recovered range(s)\n", rel, nn);
    }
    return rc;
}
