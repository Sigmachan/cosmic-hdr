/*
 * acid-hdr.c  — HDR10 + BT.2020 color pipeline injector via KMS atomic
 *
 * Steals DRM master via VT switch (tty1→tty2→tty1, screen blanks ~0.5s).
 * Sets CTM (BT.709→BT.2020 gamut expansion) + HDR_OUTPUT_METADATA + Colorspace.
 * Properties persist after we release master (Smithay doesn't touch them).
 *
 * Build:
 *   cc -O2 -o ~/.local/bin/acid-hdr $(pkg-config --cflags --libs libdrm) ~/.local/bin/acid-hdr.c -lm
 *
 * Usage (must be root):
 *   sudo acid-hdr          apply
 *   sudo acid-hdr reset    restore linear + SDR
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <stdint.h>
#include <sys/ioctl.h>
#include <linux/vt.h>

#include <xf86drm.h>
#include <xf86drmMode.h>
#include <drm_mode.h>

/* ── config ──────────────────────────────────────────────────────────────── */
#define DRM_DEV       "/dev/dri/card1"

/* A85H OLED peak brightness. Controls tone-mapper aggression:
 * - Too high (10000) → TV boosts everything, too bright on desktop.
 * - Match to display peak → near 1:1 mapping, natural HDR look.
 * Hisense A85H spec: ~800 nits typical, ~1000 nits peak boost. */
#define DISPLAY_PEAK_NITS  1000

/*
 * BT.709 → BT.2020 gamut expansion matrix.
 * Derived from: (BT.2020←XYZ) × (XYZ←BT.709), D65 whitepoint.
 * Correct colourimetric expansion — white stays white, no saturation pumping.
 * Required when Colorspace=BT2020_RGB so the TV decodes colours correctly.
 */
static const double CTM_709_TO_2020[3][3] = {
    { 0.627504,  0.329275,  0.043303 },
    { 0.069108,  0.919519,  0.011360 },
    { 0.016394,  0.088011,  0.895380 },
};
/* ─────────────────────────────────────────────────────────────────────────── */

/* drm_color_ctm: 9 × S31.32 (bit63=sign, bits[62:0]=unsigned magnitude) */
static void build_ctm(const double m[3][3], uint64_t out[9]) {
    for (int r = 0; r < 3; r++) for (int c = 0; c < 3; c++) {
        double v = m[r][c];
        uint64_t mag = (uint64_t)(fabs(v) * (1ULL << 32)) & ~(1ULL << 63);
        if (v < 0) mag |= (1ULL << 63);
        out[r * 3 + c] = mag;
    }
}

static void build_ctm_identity(uint64_t out[9]) {
    double id[3][3] = {{1,0,0},{0,1,0},{0,0,1}};
    build_ctm(id, out);
}

/* ── HDR10 metadata ──────────────────────────────────────────────────────── */
/*
 * Mirrors struct hdr_output_metadata from <drm/drm_mode.h> exactly.
 * sizeof = 32:  u32(4) + u8(1) + u8(1) + [no pad, u16 is at offset 6] +
 *               u16[3][2](12) + u16[2](4) + u16*4(8) + trailing pad(2)
 * Must NOT be packed — the kernel validates blob size == 32.
 */
typedef struct {
    uint32_t metadata_type;         /* 0 = HDMI_STATIC_METADATA_TYPE1    */
    uint8_t  eotf;                  /* 2 = PQ/ST2084                     */
    uint8_t  metadata_descriptor;   /* 0 = Static_Metadata_Descriptor_ID */
    uint16_t display_primaries[3][2]; /* G,B,R  x,y × 50000              */
    uint16_t white_point[2];
    uint16_t max_display_mastering_luminance;
    uint16_t min_display_mastering_luminance;
    uint16_t max_content_light_level;
    uint16_t max_frame_average_light_level;
} hdr_meta_t;

static hdr_meta_t build_hdr_meta(void) {
    hdr_meta_t m = {0};
    m.metadata_type = 0;  /* HDMI_STATIC_METADATA_TYPE1 */
    m.eotf = 2;           /* PQ / ST2084 */
    m.metadata_descriptor = 0;
    /* BT.2020 primaries × 50000 (CTA-861 order: G, B, R) */
    m.display_primaries[0][0] = (uint16_t)(0.170 * 50000);  /* G x */
    m.display_primaries[0][1] = (uint16_t)(0.797 * 50000);  /* G y */
    m.display_primaries[1][0] = (uint16_t)(0.131 * 50000);  /* B x */
    m.display_primaries[1][1] = (uint16_t)(0.046 * 50000);  /* B y */
    m.display_primaries[2][0] = (uint16_t)(0.708 * 50000);  /* R x */
    m.display_primaries[2][1] = (uint16_t)(0.292 * 50000);  /* R y */
    m.white_point[0] = (uint16_t)(0.3127 * 50000);          /* D65 */
    m.white_point[1] = (uint16_t)(0.3290 * 50000);
    /* Declare mastering at the display's own peak so the tone-mapper is near 1:1.
     * MaxCLL == display peak = no aggressive boost; OLED black = max contrast ratio. */
    m.max_display_mastering_luminance = DISPLAY_PEAK_NITS;   /* cd/m² units × 1    */
    m.min_display_mastering_luminance = 1;                   /* 0.0001 cd/m² × 10k */
    m.max_content_light_level = DISPLAY_PEAK_NITS;           /* MaxCLL             */
    m.max_frame_average_light_level = 200;                   /* MaxFALL            */
    return m;
}

/* ── property helpers ────────────────────────────────────────────────────── */
static uint32_t get_prop_id(int fd, uint32_t obj_id, uint32_t obj_type, const char *name) {
    drmModeObjectPropertiesPtr props = drmModeObjectGetProperties(fd, obj_id, obj_type);
    if (!props) return 0;
    uint32_t result = 0;
    for (uint32_t i = 0; i < props->count_props && !result; i++) {
        drmModePropertyPtr p = drmModeGetProperty(fd, props->props[i]);
        if (p) {
            if (strcmp(p->name, name) == 0)
                result = p->prop_id;
            drmModeFreeProperty(p);
        }
    }
    drmModeFreeObjectProperties(props);
    return result;
}

/* Find the integer value of a named enum option on a property. */
static uint64_t get_enum_val(int fd, uint32_t prop_id, const char *enum_name) {
    drmModePropertyPtr p = drmModeGetProperty(fd, prop_id);
    if (!p) return 0;
    uint64_t val = 0;
    int found = 0;
    for (int i = 0; i < p->count_enums; i++) {
        if (strcmp(p->enums[i].name, enum_name) == 0) {
            val = (uint64_t)p->enums[i].value;
            found = 1;
            break;
        }
    }
    if (!found) {
        printf("  enum '%s' not found on prop %u; available:", enum_name, prop_id);
        for (int i = 0; i < p->count_enums; i++)
            printf(" %s=%llu", p->enums[i].name, (unsigned long long)p->enums[i].value);
        printf("\n");
    }
    drmModeFreeProperty(p);
    return found ? val : 0;
}

static uint32_t mk_blob(int fd, const void *data, size_t sz) {
    uint32_t id = 0;
    drmModeCreatePropertyBlob(fd, data, sz, &id);
    return id;
}

/* ── VT switch helper ────────────────────────────────────────────────────── */
static int vt_switch(int tty_fd, int target_vt) {
    if (ioctl(tty_fd, VT_ACTIVATE, target_vt) < 0) { perror("VT_ACTIVATE"); return -1; }
    if (ioctl(tty_fd, VT_WAITACTIVE, target_vt) < 0) { perror("VT_WAITACTIVE"); return -1; }
    return 0;
}

/* ── main ────────────────────────────────────────────────────────────────── */
int main(int argc, char **argv) {
    int reset = (argc >= 2 && strcmp(argv[1], "reset") == 0);

    if (geteuid() != 0) { fprintf(stderr, "run as root: sudo acid-hdr\n"); return 1; }

    /* ── VT switch: move off TTY1 so COSMIC drops DRM master ──────────── */
    int tty_fd = open("/dev/tty1", O_RDWR | O_NOCTTY | O_CLOEXEC);
    if (tty_fd < 0) { perror("open /dev/tty1"); return 1; }

    printf("switching to VT2 (screen will blank ~0.5s)...\n");
    if (vt_switch(tty_fd, 2) < 0) {
        fprintf(stderr, "VT switch failed — try: sudo chvt 2 first\n");
        close(tty_fd);
        return 1;
    }
    usleep(400000); /* wait for cosmic-comp to call DROP_MASTER */

    /* ── now we can take DRM master ───────────────────────────────────── */
    int fd = open(DRM_DEV, O_RDWR | O_CLOEXEC);
    if (fd < 0) { perror("open " DRM_DEV); vt_switch(tty_fd, 1); return 1; }

    if (drmSetClientCap(fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1) ||
        drmSetClientCap(fd, DRM_CLIENT_CAP_ATOMIC, 1)) {
        fprintf(stderr, "drmSetClientCap: %s\n", strerror(errno));
        vt_switch(tty_fd, 1); return 1;
    }

    if (drmSetMaster(fd) != 0) {
        fprintf(stderr, "drmSetMaster: %s — cosmic may not have released master\n", strerror(errno));
        close(fd); vt_switch(tty_fd, 1); return 1;
    }
    printf("DRM master acquired\n");

    /* Find HDMI-A connector and its CRTC */
    drmModeResPtr res = drmModeGetResources(fd);
    if (!res) { perror("drmModeGetResources"); drmDropMaster(fd); vt_switch(tty_fd, 1); return 1; }

    uint32_t conn_id = 0, crtc_id = 0;
    for (int i = 0; i < res->count_connectors && !conn_id; i++) {
        drmModeConnectorPtr c = drmModeGetConnector(fd, res->connectors[i]);
        if (!c) continue;
        if (c->connector_type == DRM_MODE_CONNECTOR_HDMIA && c->connection == DRM_MODE_CONNECTED) {
            conn_id = c->connector_id;
            if (c->encoder_id) {
                drmModeEncoderPtr enc = drmModeGetEncoder(fd, c->encoder_id);
                if (enc) { crtc_id = enc->crtc_id; drmModeFreeEncoder(enc); }
            }
        }
        drmModeFreeConnector(c);
    }
    drmModeFreeResources(res);

    if (!conn_id || !crtc_id) {
        fprintf(stderr, "HDMI-A connector/CRTC not found\n");
        drmDropMaster(fd); vt_switch(tty_fd, 1); return 1;
    }
    printf("connector=%u  CRTC=%u\n", conn_id, crtc_id);

    uint64_t ctm9[9];
    if (reset)
        build_ctm_identity(ctm9);
    else
        build_ctm((const double(*)[3])CTM_709_TO_2020, ctm9);

    uint32_t ctm_blob = mk_blob(fd, ctm9, sizeof(ctm9));
    printf("CTM blob=%u\n", ctm_blob);

    uint32_t p_deg    = get_prop_id(fd, crtc_id, DRM_MODE_OBJECT_CRTC,      "DEGAMMA_LUT");
    uint32_t p_ctm    = get_prop_id(fd, crtc_id, DRM_MODE_OBJECT_CRTC,      "CTM");
    uint32_t p_gam    = get_prop_id(fd, crtc_id, DRM_MODE_OBJECT_CRTC,      "GAMMA_LUT");
    uint32_t p_hdr    = get_prop_id(fd, conn_id, DRM_MODE_OBJECT_CONNECTOR,  "HDR_OUTPUT_METADATA");
    uint32_t p_cspace = get_prop_id(fd, conn_id, DRM_MODE_OBJECT_CONNECTOR,  "Colorspace");
    printf("props: DEGAMMA=%u CTM=%u GAMMA=%u HDR=%u Colorspace=%u\n",
           p_deg, p_ctm, p_gam, p_hdr, p_cspace);

    /* ── CTM only — DEGAMMA/GAMMA disabled (blob=0) to avoid darkening ── */
    drmModeAtomicReqPtr req = drmModeAtomicAlloc();
    if (p_deg) drmModeAtomicAddProperty(req, crtc_id, p_deg, 0);          /* bypass */
    if (p_ctm) drmModeAtomicAddProperty(req, crtc_id, p_ctm, reset ? 0 : ctm_blob);
    if (p_gam) drmModeAtomicAddProperty(req, crtc_id, p_gam, 0);          /* bypass */

    int ret = drmModeAtomicCommit(fd, req, DRM_MODE_ATOMIC_NONBLOCK, NULL);
    printf("CTM commit ret=%d  errno=%d (%s)\n", ret, errno, ret ? strerror(errno) : "ok");
    drmModeAtomicFree(req);

    /* HDR + Colorspace: needs ALLOW_MODESET with full CRTC state */
    int hdr_ret = -1;
    if (p_hdr && p_cspace) {
        uint64_t cspace_val = reset ? 0
            : get_enum_val(fd, p_cspace, "BT2020_RGB");

        hdr_meta_t hdr_m = build_hdr_meta();
        uint32_t hdr_blob = (reset || cspace_val == 0) ? 0
            : mk_blob(fd, &hdr_m, sizeof(hdr_m));

        printf("  Colorspace BT2020_RGB enum value = %llu\n", (unsigned long long)cspace_val);

        /* ALLOW_MODESET requires complete CRTC state in the commit */
        uint32_t p_crtc_id    = get_prop_id(fd, conn_id, DRM_MODE_OBJECT_CONNECTOR, "CRTC_ID");
        uint32_t p_crtc_act   = get_prop_id(fd, crtc_id, DRM_MODE_OBJECT_CRTC,      "ACTIVE");
        uint32_t p_mode_id    = get_prop_id(fd, crtc_id, DRM_MODE_OBJECT_CRTC,      "MODE_ID");

        /* Grab the current mode from the live CRTC */
        drmModeCrtcPtr cur_crtc = drmModeGetCrtc(fd, crtc_id);
        uint32_t mode_blob = 0;
        if (cur_crtc && p_mode_id) {
            drmModeCreatePropertyBlob(fd, &cur_crtc->mode,
                                      sizeof(cur_crtc->mode), &mode_blob);
            drmModeFreeCrtc(cur_crtc);
        }
        printf("  modeset props: CRTC_ID=%u ACTIVE=%u MODE_ID=%u  mode_blob=%u\n",
               p_crtc_id, p_crtc_act, p_mode_id, mode_blob);

        drmModeAtomicReqPtr req2 = drmModeAtomicAlloc();
        /* connector: tie to CRTC + HDR + Colorspace */
        if (p_crtc_id)  drmModeAtomicAddProperty(req2, conn_id, p_crtc_id,  crtc_id);
        drmModeAtomicAddProperty(req2, conn_id, p_hdr,    hdr_blob);
        drmModeAtomicAddProperty(req2, conn_id, p_cspace, cspace_val);
        /* CRTC: must be active with a valid mode */
        if (p_crtc_act) drmModeAtomicAddProperty(req2, crtc_id, p_crtc_act, 1);
        if (p_mode_id && mode_blob)
                        drmModeAtomicAddProperty(req2, crtc_id, p_mode_id,   mode_blob);

        hdr_ret = drmModeAtomicCommit(fd, req2,
            DRM_MODE_ATOMIC_ALLOW_MODESET, NULL);
        printf("HDR+Colorspace commit ret=%d  errno=%d (%s)\n",
               hdr_ret, errno, hdr_ret ? strerror(errno) : "ok");
        drmModeAtomicFree(req2);
        if (hdr_blob)  drmModeDestroyPropertyBlob(fd, hdr_blob);
        if (mode_blob) drmModeDestroyPropertyBlob(fd, mode_blob);
    }

    /* ── release master and switch back ──────────────────────────────── */
    drmDropMaster(fd);
    drmModeDestroyPropertyBlob(fd, ctm_blob);
    close(fd);

    printf("DRM master released — switching back to VT1...\n");
    vt_switch(tty_fd, 1);
    close(tty_fd);

    if (ret == 0 && hdr_ret == 0) {
        if (reset)
            printf("✓ reset: linear gamma + SDR restored\n");
        else
            printf("✓ HDR10 ACTIVE: CTM(BT.709→BT.2020) + BT2020_RGB + 10000nit metadata\n"
                   "  TV should show HDR badge. Properties persist until reset.\n");
    } else {
        printf("color LUT ret=%d  HDR ret=%d\n", ret, hdr_ret);
    }
    return (ret != 0);
}
