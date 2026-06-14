/*
 * kms-hdr (cosmic-hdr.c) — Universal KMS HDR injector for Linux
 * HDR10 + BT.2020/DCI-P3 color pipeline via DRM atomic, compositor-agnostic.
 *
 * Modes:
 *   one-shot (default):  apply and exit
 *   daemon (--daemon):   apply, stay alive, re-apply on SIGUSR1
 *   reload (--reload):   send SIGUSR1 to running daemon via /run/kms-hdr.pid
 *
 * Two pipeline modes (auto-detected by GPU driver):
 *   AMD/Intel: DEGAMMA (sRGB→linear) → CTM (BT.709→gamut×sat) → GAMMA (linear→midtone→PQ)
 *              + HDR_OUTPUT_METADATA + Colorspace=BT2020_RGB
 *   NVIDIA:    legacy gamma ramp (sRGB→PQ, no gamut expansion)
 *              + HDR_OUTPUT_METADATA + Colorspace=BT2020_RGB
 *
 * Steals DRM master via VT switch (tty1→tty2→tty1, screen blanks ~0.5s).
 * Properties persist after master release (any compositor).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <stdint.h>
#include <dirent.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/inotify.h>
#include <sys/wait.h>
#include <linux/vt.h>

#include <xf86drm.h>
#include <xf86drmMode.h>
#include <drm_mode.h>

/* ── config ──────────────────────────────────────────────────────────────── */
#define CONF_PATH        "/etc/kms-hdr.conf"
#define PID_FILE         "/run/kms-hdr.pid"
#define LUT_SIZE         4096
#define MAX_CARDS        8
#define MASTER_RETRY_MS  500   /* ms between drmSetMaster retries */
#define MASTER_RETRIES   12   /* total retries (~6 seconds) */

#define DEFAULT_SDR_NITS      203
#define DEFAULT_PEAK_NITS     800
#define DEFAULT_GAMUT         100
#define DEFAULT_GAMUT_MODE    0    /* 0=BT.2020, 1=DCI-P3 */
#define DEFAULT_MAX_BPC       10
#define DEFAULT_SATURATION    100  /* 100 = neutral */
#define DEFAULT_MIDTONE_GAMMA 100  /* 100 = neutral; >100 = HDR punch; <100 = lift */

/* ── daemon signal state ──────────────────────────────────────────────────── */
static volatile sig_atomic_t g_reload = 0;
static void sigusr1_handler(int s) { (void)s; g_reload = 1; }

/* ── apply context ────────────────────────────────────────────────────────── */
typedef struct {
    char card_path[64];   /* empty = auto-detect */
    char conn_name[64];   /* empty = auto-detect */
    int  reset;
    int  no_vt_switch;
    int  sdr_nits;
    int  peak_nits;
    int  gamut_pct;
    int  gamut_mode;
    int  max_bpc;
    int  saturation_pct;
    int  midtone_gamma;
    int  explicit_peak;
    int  explicit_gamut_mode;
} ApplyCtx;

/* ── conf I/O ────────────────────────────────────────────────────────────── */
static void load_conf(int *sdr_nits, int *peak_nits, int *gamut_pct,
                      int *gamut_mode, int *max_bpc, int *saturation,
                      int *oled_dim_min, int *midtone_gamma, int *force_oled) {
    *sdr_nits      = DEFAULT_SDR_NITS;
    *peak_nits     = DEFAULT_PEAK_NITS;
    *gamut_pct     = DEFAULT_GAMUT;
    *gamut_mode    = DEFAULT_GAMUT_MODE;
    *max_bpc       = DEFAULT_MAX_BPC;
    *saturation    = DEFAULT_SATURATION;
    *oled_dim_min  = 0;
    *midtone_gamma = DEFAULT_MIDTONE_GAMMA;
    *force_oled    = 0;
    FILE *f = fopen(CONF_PATH, "r");
    if (!f) return;
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        int v; char s[64];
        if (sscanf(line, "SDR_NITS=%d",      &v) == 1) *sdr_nits      = v;
        if (sscanf(line, "PEAK_NITS=%d",     &v) == 1) *peak_nits     = v;
        if (sscanf(line, "GAMUT=%d",         &v) == 1) *gamut_pct     = v;
        if (sscanf(line, "MAX_BPC=%d",       &v) == 1) *max_bpc       = v;
        if (sscanf(line, "SATURATION=%d",    &v) == 1) *saturation    = v;
        if (sscanf(line, "OLED_DIM_MIN=%d",  &v) == 1) *oled_dim_min  = v;
        if (sscanf(line, "MIDTONE_GAMMA=%d", &v) == 1) *midtone_gamma = v;
        if (sscanf(line, "FORCE_OLED=%d",    &v) == 1) *force_oled    = v;
        if (sscanf(line, "GAMUT_MODE=%63s",   s) == 1)
            *gamut_mode = (strncmp(s, "dci-p3", 6) == 0) ? 1 : 0;
    }
    fclose(f);
}

static void save_conf(int sdr_nits, int peak_nits, int gamut_pct,
                      int gamut_mode, int max_bpc, int saturation,
                      int oled_dim_min, int midtone_gamma, int force_oled) {
    const char *tmp = CONF_PATH ".tmp";
    FILE *f = fopen(tmp, "w");
    if (!f) { perror("save_conf: fopen " CONF_PATH ".tmp"); return; }
    int n = fprintf(f,
        "SDR_NITS=%d\nPEAK_NITS=%d\nGAMUT=%d\nMAX_BPC=%d\n"
        "GAMUT_MODE=%s\nSATURATION=%d\nOLED_DIM_MIN=%d\nMIDTONE_GAMMA=%d\nFORCE_OLED=%d\n",
        sdr_nits, peak_nits, gamut_pct, max_bpc,
        gamut_mode == 1 ? "dci-p3" : (gamut_mode == 2 ? "srgb" : "bt2020"),
        saturation, oled_dim_min, midtone_gamma, force_oled);
    if (n < 0) { perror("save_conf: fprintf"); fclose(f); unlink(tmp); return; }
    fflush(f);
    fsync(fileno(f));
    if (fclose(f) != 0) { perror("save_conf: fclose"); unlink(tmp); return; }
    if (rename(tmp, CONF_PATH) != 0) { perror("save_conf: rename"); unlink(tmp); return; }
    printf("saved: %s\n", CONF_PATH);
}

/* Write /etc/hdr-game.conf from KEY=VAL pairs passed on the command line. */
static int save_game_conf(int argc, char **argv) {
    const char *game_conf = "/etc/hdr-game.conf";
    FILE *f = fopen(game_conf, "w");
    if (!f) { perror("save_game_conf: fopen /etc/hdr-game.conf"); return 1; }
    for (int i = 1; i < argc; i++) {
        char *eq = strchr(argv[i], '=');
        if (!eq) { fprintf(stderr, "save-game: expected KEY=VAL, got: %s\n", argv[i]); continue; }
        fprintf(f, "%s\n", argv[i]);
    }
    fclose(f);
    printf("saved: %s\n", game_conf);
    return 0;
}

/* Auto-detect the DRM card device and connector name.
 * card_out: "/dev/dri/cardN" · conn_out: "HDMI-A-2" */
static int find_drm_device(char *card_out, size_t card_sz,
                            char *conn_out, size_t conn_sz) {
    DIR *d = opendir("/sys/class/drm");
    if (!d) return -1;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (strncmp(e->d_name, "card", 4) != 0) continue;
        char *dash = strchr(e->d_name + 4, '-');
        if (!dash) continue;

        /* Skip virtual/headless connectors (X11 backend, winit-in-X, Wayland backend) */
        const char *conn_part = dash + 1;
        if (strncmp(conn_part, "X11-",      4) == 0 ||
            strncmp(conn_part, "Virtual-",  8) == 0 ||
            strncmp(conn_part, "HEADLESS-", 9) == 0 ||
            strncmp(conn_part, "WL-",       3) == 0) continue;

        char edid_path[512];
        snprintf(edid_path, sizeof(edid_path), "/sys/class/drm/%s/edid", e->d_name);
        int efd = open(edid_path, O_RDONLY);
        if (efd < 0) continue;
        char buf[1]; int n = read(efd, buf, 1); close(efd);
        if (n <= 0) continue;

        int card_num = atoi(e->d_name + 4);
        snprintf(card_out, card_sz, "/dev/dri/card%d", card_num);
        snprintf(conn_out, conn_sz, "%s", conn_part);
        closedir(d);
        return 0;
    }
    closedir(d);
    return -1;
}

/* ── EDID auto-configuration ─────────────────────────────────────────────── */
static int parse_edid_caps(const char *path,
                            int *peak_nits, int *bt2020, int *dcip3) {
    *peak_nits = 0; *bt2020 = 0; *dcip3 = 0;
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    uint8_t edid[2048]; memset(edid, 0, sizeof(edid));
    size_t n = fread(edid, 1, sizeof(edid), f);
    fclose(f);
    if (n < 128) return -1;
    for (size_t bs = 128; bs + 128 <= n; bs += 128) {
        if (edid[bs] != 0x02) continue;
        uint8_t dtd = edid[bs + 2];
        for (size_t i = 4; i < (size_t)dtd && bs + i < n; ) {
            uint8_t tag = (edid[bs + i] >> 5) & 0x7;
            uint8_t len = edid[bs + i] & 0x1f;
            if (bs + i + 1 + (size_t)len > n) break;
            const uint8_t *d = &edid[bs + i + 1];
            if (tag == 7 && len > 1) {
                const uint8_t *p = d + 1;
                size_t plen = len - 1;
                if (d[0] == 6 && plen > 2 && p[2] != 0)
                    *peak_nits = (int)(50.0 * pow(2.0, p[2] / 32.0));
                else if (d[0] == 5 && plen > 0) {
                    *bt2020 = (p[0] & 0x80) ? 1 : 0;
                    *dcip3  = (p[0] & 0x02) ? 1 : 0;
                }
            }
            i += 1 + (size_t)len;
        }
    }
    return 0;
}

/* ── LUT helpers ─────────────────────────────────────────────────────────── */
typedef struct { uint16_t r, g, b, pad; } drm_lut_entry;

static double srgb_to_linear(double x) {
    return x <= 0.04045 ? x / 12.92 : pow((x + 0.055) / 1.055, 2.4);
}

static double linear_to_pq(double L) {
    if (L <= 0.0) return 0.0;
    const double m1 = 0.1593017578125;
    const double m2 = 78.84375;
    const double c1 = 0.8359375;
    const double c2 = 18.8515625;
    const double c3 = 18.6875;
    double Lm = pow(L, m1);
    return pow((c1 + c2 * Lm) / (1.0 + c3 * Lm), m2);
}

static double clamp01(double v) { return v < 0.0 ? 0.0 : (v > 1.0 ? 1.0 : v); }

/* DEGAMMA: sRGB gamma → linear [0,1] */
static drm_lut_entry *build_degamma_srgb(int n) {
    drm_lut_entry *lut = calloc(n, sizeof(*lut));
    if (!lut) return NULL;
    for (int i = 0; i < n; i++) {
        double x = (double)i / (n - 1);
        uint16_t v = (uint16_t)(clamp01(srgb_to_linear(x)) * 65535.0 + 0.5);
        lut[i].r = lut[i].g = lut[i].b = v;
    }
    return lut;
}

/*
 * GAMMA: linear [0,1] → midtone curve → PQ code
 *   midtone_gamma: 100 = neutral, >100 = darken midtones (HDR punch),
 *                  <100 = lift midtones (looks washed out). Range 30–250.
 *   Applies Y^(gamma/100) in luminance before PQ encode.
 */
static drm_lut_entry *build_gamma_pq(int n, double sdr_nits, double midtone_gamma) {
    drm_lut_entry *lut = calloc(n, sizeof(*lut));
    if (!lut) return NULL;
    double scale = sdr_nits / 10000.0;
    double g = midtone_gamma / 100.0;
    for (int i = 0; i < n; i++) {
        double x = (double)i / (n - 1);
        if (g != 1.0 && x > 0.0)
            x = pow(x, g);
        double pq = linear_to_pq(x * scale);
        uint16_t v = (uint16_t)(clamp01(pq) * 65535.0 + 0.5);
        lut[i].r = lut[i].g = lut[i].b = v;
    }
    return lut;
}

/* Identity LUT for reset */
static drm_lut_entry *build_linear_lut(int n) {
    drm_lut_entry *lut = calloc(n, sizeof(*lut));
    if (!lut) return NULL;
    for (int i = 0; i < n; i++) {
        uint16_t v = (uint16_t)((double)i / (n - 1) * 65535.0 + 0.5);
        lut[i].r = lut[i].g = lut[i].b = v;
    }
    return lut;
}

/* ── CTM ─────────────────────────────────────────────────────────────────── */
static const double CTM_709_TO_2020[3][3] = {
    { 0.627504,  0.329275,  0.043303 },
    { 0.069108,  0.919519,  0.011360 },
    { 0.016394,  0.088011,  0.895380 },
};

static const double CTM_709_TO_DCIP3[3][3] = {
    { 0.822461,  0.177538,  0.000000 },
    { 0.033195,  0.966805,  0.000000 },
    { 0.017083,  0.072397,  0.910520 },
};

/* sRGB/BT.709 → BT.709 = identity (no gamut expansion) */
static const double CTM_IDENTITY[3][3] = {
    { 1.000000,  0.000000,  0.000000 },
    { 0.000000,  1.000000,  0.000000 },
    { 0.000000,  0.000000,  1.000000 },
};

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

static void build_sat_mat(double s, double out[3][3]) {
    double Ry = 0.2126, Gy = 0.7152, By = 0.0722;
    out[0][0] = (1-s)*Ry + s; out[0][1] = (1-s)*Gy;      out[0][2] = (1-s)*By;
    out[1][0] = (1-s)*Ry;     out[1][1] = (1-s)*Gy + s;  out[1][2] = (1-s)*By;
    out[2][0] = (1-s)*Ry;     out[2][1] = (1-s)*Gy;      out[2][2] = (1-s)*By + s;
}

static void mat_mul_3x3(const double a[3][3], const double b[3][3], double out[3][3]) {
    for (int r = 0; r < 3; r++)
        for (int c = 0; c < 3; c++) {
            out[r][c] = 0;
            for (int k = 0; k < 3; k++) out[r][c] += a[r][k] * b[k][c];
        }
}

/* ── HDR10 metadata ──────────────────────────────────────────────────────── */
typedef struct {
    uint32_t metadata_type;
    uint8_t  eotf;
    uint8_t  metadata_descriptor;
    uint16_t display_primaries[3][2];
    uint16_t white_point[2];
    uint16_t max_display_mastering_luminance;
    uint16_t min_display_mastering_luminance;
    uint16_t max_content_light_level;
    uint16_t max_frame_average_light_level;
} hdr_meta_t;

static hdr_meta_t build_hdr_meta(int peak_nits, int sdr_nits) {
    hdr_meta_t m = {0};
    m.metadata_type      = 0;
    m.eotf               = 2;   /* PQ / ST2084 */
    m.metadata_descriptor = 0;
    m.display_primaries[0][0] = (uint16_t)(0.170 * 50000);
    m.display_primaries[0][1] = (uint16_t)(0.797 * 50000);
    m.display_primaries[1][0] = (uint16_t)(0.131 * 50000);
    m.display_primaries[1][1] = (uint16_t)(0.046 * 50000);
    m.display_primaries[2][0] = (uint16_t)(0.708 * 50000);
    m.display_primaries[2][1] = (uint16_t)(0.292 * 50000);
    m.white_point[0] = (uint16_t)(0.3127 * 50000);
    m.white_point[1] = (uint16_t)(0.3290 * 50000);
    m.max_display_mastering_luminance = (uint16_t)peak_nits;
    m.min_display_mastering_luminance = 1;
    m.max_content_light_level        = (uint16_t)peak_nits;
    m.max_frame_average_light_level  = (uint16_t)sdr_nits;
    return m;
}

/* ── NVIDIA legacy gamma fallback ────────────────────────────────────────── */
static int set_nvidia_gamma_pq(int fd, uint32_t crtc_id, double sdr_nits,
                                double midtone_gamma, int reset) {
    drmModeCrtcPtr crtc = drmModeGetCrtc(fd, crtc_id);
    if (!crtc) {
        fprintf(stderr, "NVIDIA gamma: crtc has no gamma table\n");
        return -1;
    }
    if (crtc->gamma_size <= 1) {
        drmModeFreeCrtc(crtc);
        fprintf(stderr, "[kms-hdr] NVIDIA gamma_size too small\n");
        return -1;
    }
    uint32_t gs = crtc->gamma_size;
    drmModeFreeCrtc(crtc);

    uint16_t *r = malloc(gs * sizeof(uint16_t));
    uint16_t *g = malloc(gs * sizeof(uint16_t));
    uint16_t *b = malloc(gs * sizeof(uint16_t));
    if (!r || !g || !b) { free(r); free(g); free(b); return -1; }
    double gv = midtone_gamma / 100.0;

    for (uint32_t i = 0; i < gs; i++) {
        uint16_t v;
        if (reset) {
            v = (uint16_t)((double)i / (gs - 1) * 65535.0 + 0.5);
        } else {
            double x      = (double)i / (gs - 1);
            double linear = srgb_to_linear(x);
            if (gv != 1.0 && linear > 0.0)
                linear = pow(linear, gv);
            double pq = linear_to_pq(linear * sdr_nits / 10000.0);
            v = (uint16_t)(clamp01(pq) * 65535.0 + 0.5);
        }
        r[i] = g[i] = b[i] = v;
    }

    int ret = drmModeCrtcSetGamma(fd, crtc_id, gs, r, g, b);
    free(r); free(g); free(b);
    return ret;
}

/* ── property helpers ────────────────────────────────────────────────────── */
static uint32_t get_prop_id(int fd, uint32_t obj_id, uint32_t obj_type, const char *name) {
    drmModeObjectPropertiesPtr props = drmModeObjectGetProperties(fd, obj_id, obj_type);
    if (!props) return 0;
    uint32_t result = 0;
    for (uint32_t i = 0; i < props->count_props && !result; i++) {
        drmModePropertyPtr p = drmModeGetProperty(fd, props->props[i]);
        if (p) {
            if (strcmp(p->name, name) == 0) result = p->prop_id;
            drmModeFreeProperty(p);
        }
    }
    drmModeFreeObjectProperties(props);
    return result;
}

static uint64_t get_enum_val(int fd, uint32_t prop_id, const char *enum_name) {
    drmModePropertyPtr p = drmModeGetProperty(fd, prop_id);
    if (!p) return 0;
    uint64_t val = 0; int found = 0;
    for (int i = 0; i < p->count_enums; i++) {
        if (strcmp(p->enums[i].name, enum_name) == 0) {
            val = (uint64_t)p->enums[i].value; found = 1; break;
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
    if (drmModeCreatePropertyBlob(fd, data, sz, &id) != 0 || id == 0) {
        fprintf(stderr, "[kms-hdr] drmModeCreatePropertyBlob failed: %s\n", strerror(errno));
        return 0;
    }
    return id;
}

/* ── VT switch ───────────────────────────────────────────────────────────── */
static int vt_switch(int tty_fd, int target_vt) {
    if (ioctl(tty_fd, VT_ACTIVATE,   target_vt) < 0) { perror("VT_ACTIVATE");  return -1; }
    if (ioctl(tty_fd, VT_WAITACTIVE, target_vt) < 0) { perror("VT_WAITACTIVE"); return -1; }
    return 0;
}

/* ── usage ───────────────────────────────────────────────────────────────── */
static void usage(const char *argv0) {
    fprintf(stderr,
        "Usage: %s [OPTIONS] [reset]\n"
        "       %s --save-game KEY=VAL ...\n"
        "       %s --reload\n"
        "\n"
        "Options:\n"
        "  --card /dev/dri/cardN    DRM device (alias: --output; auto-detected)\n"
        "  --connector NAME         Connector, e.g. HDMI-A-2 (alias: --display; auto-detected)\n"
        "  --save                   Write settings to " CONF_PATH " before applying\n"
        "  --save-only              Write " CONF_PATH " and exit (no DRM operations)\n"
        "  --sdr-nits N             SDR white brightness in nits (default: 203 or from EDID)\n"
        "  --peak-nits N            Display peak luminance in nits (default: EDID or 800)\n"
        "  --gamut N                Gamut expansion blend 0-100%% (default 100)\n"
        "  --gamut-mode MODE        bt2020 | dci-p3 | srgb (default: from EDID colorimetry)\n"
        "  --saturation N           Color intensity 50-200%% (100=neutral; default 100)\n"
        "  --midtone-gamma N        Midtone curve 30-250%% (100=neutral, >100=HDR punch; default 100)\n"
        "  --bpc N                  Output bit depth: 8, 10, or 12 (default 10)\n"
        "  --oled-dim-min N         OLED auto-dim timeout in minutes (0=disabled)\n"
        "  --dim-to N               Set SDR=N peak=N*4 and apply (OLED auto-dim use)\n"
        "  --no-vt-switch           Skip VT2 switch — try direct DRM master\n"
        "  --daemon                 Apply and stay alive; re-apply on SIGUSR1\n"
        "  --reload                 Send SIGUSR1 to running daemon via " PID_FILE "\n"
        "  reset                    Restore SDR (identity pipeline)\n"
        "  --save-game KEY=VAL ...  Write /etc/hdr-game.conf from KEY=VAL pairs and exit\n"
        "  --help                   Show this message\n"
        "\n"
        "Midtone gamma: 100=neutral, 130-160=HDR punch (darkens midtones),\n"
        "               <100=lift midtones (looks washed out). Range 30-250.\n"
        "GPU driver is auto-detected: AMD/Intel = full pipeline; NVIDIA = PQ gamma only.\n",
        argv0, argv0, argv0);
}

/* ── do_apply ────────────────────────────────────────────────────────────── */
/* All DRM work extracted here so both one-shot and daemon can call it. */
static int do_apply(ApplyCtx *ctx) {
    char card_path[64];
    char conn_buf[64];
    const char *want_conn = NULL;

    /* Auto-detect card/connector if not provided */
    if (ctx->card_path[0] != '\0') {
        snprintf(card_path, sizeof(card_path), "%s", ctx->card_path);
        want_conn = ctx->conn_name[0] ? ctx->conn_name : NULL;
    } else {
        if (find_drm_device(card_path, sizeof(card_path),
                            conn_buf, sizeof(conn_buf)) != 0) {
            fprintf(stderr, "no connected display found in /sys/class/drm — "
                            "use --card and --connector\n");
            return 1;
        }
        printf("auto-detected: %s  connector: %s\n", card_path, conn_buf);
        want_conn = ctx->conn_name[0] ? ctx->conn_name : conn_buf;
    }

    int sdr_nits      = ctx->sdr_nits;
    int peak_nits     = ctx->peak_nits;
    int gamut_pct     = ctx->gamut_pct;
    int gamut_mode    = ctx->gamut_mode;
    int max_bpc       = ctx->max_bpc;
    int saturation_pct = ctx->saturation_pct;
    int midtone_gamma  = ctx->midtone_gamma;
    int reset          = ctx->reset;
    int no_vt_switch   = ctx->no_vt_switch;

    /* EDID auto-configuration */
    if (!reset && want_conn) {
        char sysfs_edid[512];
        const char *cname = strrchr(card_path, '/');
        cname = cname ? cname + 1 : card_path;
        snprintf(sysfs_edid, sizeof(sysfs_edid),
                 "/sys/class/drm/%s-%s/edid", cname, want_conn);

        int edid_peak = 0, edid_bt2020 = 0, edid_dcip3 = 0;
        if (parse_edid_caps(sysfs_edid, &edid_peak, &edid_bt2020, &edid_dcip3) == 0) {
            printf("EDID auto-detect: peak=%d nits  BT.2020=%s  DCI-P3=%s\n",
                   edid_peak, edid_bt2020 ? "✓" : "—", edid_dcip3 ? "✓" : "—");
            if (!ctx->explicit_peak && edid_peak > 0) {
                peak_nits = edid_peak;
                printf("  → peak-nits set to %d (from EDID)\n", peak_nits);
            }
            if (!ctx->explicit_gamut_mode) {
                if (edid_bt2020)     { gamut_mode = 0; printf("  → gamut-mode: BT.2020 (from EDID)\n"); }
                else if (edid_dcip3) { gamut_mode = 1; printf("  → gamut-mode: DCI-P3 (from EDID)\n"); }
                else                 { gamut_mode = 2; printf("  → gamut-mode: sRGB (display lacks wide gamut)\n"); }
            }
        }
    }

    const char *gmode_str = gamut_mode == 1 ? "DCI-P3" :
                            gamut_mode == 2 ? "sRGB"   : "BT.2020";
    printf("card: %s  connector: %s\n"
           "config: sdr_nits=%d  peak_nits=%d  gamut=%d%%  mode=%s  "
           "bpc=%d  saturation=%d%%  midtone_gamma=%d%%\n",
           card_path, want_conn ? want_conn : "any",
           sdr_nits, peak_nits, gamut_pct, gmode_str,
           max_bpc, saturation_pct, midtone_gamma);

    int tty_fd = -1;
    if (!no_vt_switch) {
        tty_fd = open("/dev/tty1", O_RDWR | O_NOCTTY | O_CLOEXEC);
        if (tty_fd < 0) {
            fprintf(stderr, "open /dev/tty1: %s — retrying without VT switch\n", strerror(errno));
            no_vt_switch = 1;
        }
    }
    if (!no_vt_switch) {
        printf("switching to VT2 (screen will blank ~0.5s)...\n");
        if (vt_switch(tty_fd, 2) < 0) {
            fprintf(stderr, "VT switch failed — proceeding without it\n");
            close(tty_fd); tty_fd = -1; no_vt_switch = 1;
        } else {
            usleep(300000);
        }
    } else {
        printf("no-vt-switch mode: attempting direct DRM master acquisition\n");
    }

#define VT_CLEANUP()  do { if (tty_fd >= 0) { vt_switch(tty_fd, 1); close(tty_fd); } } while(0)

    int fd = open(card_path, O_RDWR | O_CLOEXEC);
    if (fd < 0) { perror(card_path); VT_CLEANUP(); return 1; }

    /* Detect GPU driver */
    drmVersionPtr drv = drmGetVersion(fd);
    int is_nvidia = drv && strstr(drv->name, "nvidia") != NULL;
    printf("GPU driver: %s  (pipeline mode: %s)\n",
           drv ? drv->name : "unknown",
           is_nvidia ? "metadata-only (HDR_OUTPUT_METADATA + Colorspace + BPC)"
                     : "full (DEGAMMA+CTM+GAMMA+HDR_OUTPUT_METADATA)");
    if (drv) drmFreeVersion(drv);

    if (is_nvidia && !reset) {
        printf("NVIDIA: DEGAMMA_LUT/CTM/GAMMA_LUT not exposed by nvidia-drm.\n"
               "  HDR10 signal will be sent; no software tonemapping/gamut expansion.\n");
    }

    if (drmSetClientCap(fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1) ||
        drmSetClientCap(fd, DRM_CLIENT_CAP_ATOMIC, 1)) {
        fprintf(stderr, "drmSetClientCap: %s\n", strerror(errno));
        close(fd); VT_CLEANUP(); return 1;
    }

    int master_ok = 0;
    for (int r = 0; r < MASTER_RETRIES; r++) {
        if (drmSetMaster(fd) == 0) { master_ok = 1; break; }
        if (r == 0) fprintf(stderr, "waiting for DRM master...\n");
        usleep(MASTER_RETRY_MS * 1000);
    }
    if (!master_ok) {
        fprintf(stderr, "drmSetMaster failed after %d retries: %s\n",
                MASTER_RETRIES, strerror(errno));
        close(fd); VT_CLEANUP(); return 1;
    }
    printf("DRM master acquired\n");

    drmModeResPtr res = drmModeGetResources(fd);
    if (!res) { perror("drmModeGetResources"); drmDropMaster(fd); VT_CLEANUP(); return 1; }

    static const char *conn_type_names[] = {
        "Unknown","VGA","DVII","DVID","DVIA","Composite","SVIDEO",
        "LVDS","Component","9PinDIN","DisplayPort","HDMI-A","HDMI-B",
        "TV","eDP","Virtual","DSI","DPI","Writeback","SPI","USB"
    };
    uint32_t conn_id = 0, crtc_id = 0;
    for (int i = 0; i < res->count_connectors && !conn_id; i++) {
        drmModeConnectorPtr c = drmModeGetConnector(fd, res->connectors[i]);
        if (!c) continue;
        if (c->connection != DRM_MODE_CONNECTED) { drmModeFreeConnector(c); continue; }

        const char *type_str = (c->connector_type < 21)
                               ? conn_type_names[c->connector_type] : "Unknown";
        char cname[64];
        snprintf(cname, sizeof(cname), "%s-%u", type_str, c->connector_type_id);

        int match = (want_conn == NULL) ||
                    (strcasecmp(cname, want_conn) == 0) ||
                    (strstr(cname, want_conn) != NULL);

        if (match) {
            conn_id = c->connector_id;
            if (c->encoder_id) {
                drmModeEncoderPtr enc = drmModeGetEncoder(fd, c->encoder_id);
                if (enc) { crtc_id = enc->crtc_id; drmModeFreeEncoder(enc); }
            }
            printf("using connector: %s (id=%u)  CRTC=%u\n", cname, conn_id, crtc_id);
        }
        drmModeFreeConnector(c);
    }
    drmModeFreeResources(res);

    if (!conn_id || !crtc_id) {
        fprintf(stderr, "no connected %s connector/CRTC found\n",
                want_conn ? want_conn : "display");
        drmDropMaster(fd); VT_CLEANUP(); return 1;
    }

    /* Build LUTs and CTM */
    drm_lut_entry *deg_lut, *gam_lut;
    uint64_t ctm9[9];

    if (reset) {
        deg_lut = build_linear_lut(LUT_SIZE);
        gam_lut = build_linear_lut(LUT_SIZE);
        build_ctm_identity(ctm9);
    } else {
        deg_lut = build_degamma_srgb(LUT_SIZE);
        gam_lut = build_gamma_pq(LUT_SIZE, (double)sdr_nits, (double)midtone_gamma);
        double t = gamut_pct / 100.0;
        double gamut_mat[3][3];
        const double (*target)[3] = (gamut_mode == 1) ? CTM_709_TO_DCIP3 :
                                    (gamut_mode == 2) ? CTM_IDENTITY     : CTM_709_TO_2020;
        for (int r = 0; r < 3; r++)
            for (int c = 0; c < 3; c++)
                gamut_mat[r][c] = (r==c ? 1.0 : 0.0) * (1.0-t) + target[r][c] * t;
        double sat_mat[3][3];
        build_sat_mat(saturation_pct / 100.0, sat_mat);
        double combined[3][3];
        mat_mul_3x3(gamut_mat, sat_mat, combined);
        build_ctm((const double(*)[3])combined, ctm9);
    }

    if (!deg_lut || !gam_lut) {
        fprintf(stderr, "[kms-hdr] LUT allocation failed\n");
        free(deg_lut); free(gam_lut);
        drmDropMaster(fd); close(fd); VT_CLEANUP(); return 1;
    }

    uint32_t deg_blob = mk_blob(fd, deg_lut, LUT_SIZE * sizeof(drm_lut_entry));
    uint32_t gam_blob = mk_blob(fd, gam_lut, LUT_SIZE * sizeof(drm_lut_entry));
    uint32_t ctm_blob = mk_blob(fd, ctm9, sizeof(ctm9));
    free(deg_lut); free(gam_lut);
    printf("blobs: DEGAMMA=%u CTM=%u GAMMA=%u\n", deg_blob, ctm_blob, gam_blob);

    /* NVIDIA path uses legacy gamma, not these blobs — only require them on AMD/Intel */
    if (!is_nvidia && (!deg_blob || !ctm_blob || !gam_blob)) {
        fprintf(stderr, "[kms-hdr] property blob creation failed\n");
        if (deg_blob) drmModeDestroyPropertyBlob(fd, deg_blob);
        if (ctm_blob) drmModeDestroyPropertyBlob(fd, ctm_blob);
        if (gam_blob) drmModeDestroyPropertyBlob(fd, gam_blob);
        drmDropMaster(fd); close(fd); VT_CLEANUP(); return 1;
    }

    uint32_t p_deg    = get_prop_id(fd, crtc_id, DRM_MODE_OBJECT_CRTC,     "DEGAMMA_LUT");
    uint32_t p_ctm    = get_prop_id(fd, crtc_id, DRM_MODE_OBJECT_CRTC,     "CTM");
    uint32_t p_gam    = get_prop_id(fd, crtc_id, DRM_MODE_OBJECT_CRTC,     "GAMMA_LUT");
    uint32_t p_hdr    = get_prop_id(fd, conn_id, DRM_MODE_OBJECT_CONNECTOR, "HDR_OUTPUT_METADATA");
    uint32_t p_cspace = get_prop_id(fd, conn_id, DRM_MODE_OBJECT_CONNECTOR, "Colorspace");
    printf("props: DEGAMMA=%u CTM=%u GAMMA=%u HDR=%u Colorspace=%u\n",
           p_deg, p_ctm, p_gam, p_hdr, p_cspace);

    int ret;
    if (is_nvidia) {
        ret = set_nvidia_gamma_pq(fd, crtc_id, (double)sdr_nits,
                                  (double)midtone_gamma, reset);
        printf("NVIDIA legacy gamma (sRGB→midtone→PQ): ret=%d  errno=%d (%s)\n",
               ret, errno, ret ? strerror(errno) : "ok");
    } else {
        drmModeAtomicReqPtr req = drmModeAtomicAlloc();
        if (!req) {
            fprintf(stderr, "[kms-hdr] drmModeAtomicAlloc failed\n");
            if (deg_blob) drmModeDestroyPropertyBlob(fd, deg_blob);
            if (ctm_blob) drmModeDestroyPropertyBlob(fd, ctm_blob);
            if (gam_blob) drmModeDestroyPropertyBlob(fd, gam_blob);
            drmDropMaster(fd); close(fd); VT_CLEANUP(); return 1;
        }
        if (p_deg) drmModeAtomicAddProperty(req, crtc_id, p_deg, reset ? 0 : deg_blob);
        if (p_ctm) drmModeAtomicAddProperty(req, crtc_id, p_ctm, reset ? 0 : ctm_blob);
        if (p_gam) drmModeAtomicAddProperty(req, crtc_id, p_gam, reset ? 0 : gam_blob);
        ret = drmModeAtomicCommit(fd, req, DRM_MODE_ATOMIC_NONBLOCK, NULL);
        printf("AMD/Intel color pipeline commit ret=%d  errno=%d (%s)\n",
               ret, errno, ret ? strerror(errno) : "ok");
        drmModeAtomicFree(req);
    }

    int hdr_ret = -1;
    if (p_hdr && p_cspace) {
        uint64_t cspace_val = reset ? 0 : get_enum_val(fd, p_cspace, "BT2020_RGB");

        hdr_meta_t hdr_m  = build_hdr_meta(peak_nits, sdr_nits);
        uint32_t hdr_blob = (reset || cspace_val == 0) ? 0
                            : mk_blob(fd, &hdr_m, sizeof(hdr_m));

        uint32_t p_crtc_id  = get_prop_id(fd, conn_id, DRM_MODE_OBJECT_CONNECTOR, "CRTC_ID");
        uint32_t p_crtc_act = get_prop_id(fd, crtc_id, DRM_MODE_OBJECT_CRTC,      "ACTIVE");
        uint32_t p_mode_id  = get_prop_id(fd, crtc_id, DRM_MODE_OBJECT_CRTC,      "MODE_ID");
        uint32_t p_bpc      = get_prop_id(fd, conn_id, DRM_MODE_OBJECT_CONNECTOR,  "max_requested_bpc");
        uint32_t p_vrr      = get_prop_id(fd, conn_id, DRM_MODE_OBJECT_CONNECTOR,  "vrr_enabled");

        uint64_t vrr_saved = 0;
        if (p_vrr) {
            drmModeObjectPropertiesPtr oprops =
                drmModeObjectGetProperties(fd, conn_id, DRM_MODE_OBJECT_CONNECTOR);
            if (oprops) {
                for (uint32_t i = 0; i < oprops->count_props; i++) {
                    if (oprops->props[i] == p_vrr) { vrr_saved = oprops->prop_values[i]; break; }
                }
                drmModeFreeObjectProperties(oprops);
            }
            if (vrr_saved) {
                drmModeAtomicReqPtr vrr_req = drmModeAtomicAlloc();
                if (!vrr_req) {
                    fprintf(stderr, "[kms-hdr] drmModeAtomicAlloc (vrr disable) failed\n");
                    if (deg_blob) drmModeDestroyPropertyBlob(fd, deg_blob);
                    if (ctm_blob) drmModeDestroyPropertyBlob(fd, ctm_blob);
                    if (gam_blob) drmModeDestroyPropertyBlob(fd, gam_blob);
                    drmDropMaster(fd); close(fd); VT_CLEANUP(); return 1;
                }
                drmModeAtomicAddProperty(vrr_req, conn_id, p_vrr, 0);
                drmModeAtomicCommit(fd, vrr_req, DRM_MODE_ATOMIC_NONBLOCK, NULL);
                drmModeAtomicFree(vrr_req);
                usleep(50000);
                printf("VRR disabled (was %llu) before modeset\n", (unsigned long long)vrr_saved);
            }
        }

        drmModeCrtcPtr cur = drmModeGetCrtc(fd, crtc_id);
        uint32_t mode_blob = 0;
        if (cur && p_mode_id) {
            drmModeCreatePropertyBlob(fd, &cur->mode, sizeof(cur->mode), &mode_blob);
            drmModeFreeCrtc(cur);
        }

        drmModeAtomicReqPtr req2 = drmModeAtomicAlloc();
        if (!req2) {
            fprintf(stderr, "[kms-hdr] drmModeAtomicAlloc (hdr commit) failed\n");
            if (hdr_blob)  drmModeDestroyPropertyBlob(fd, hdr_blob);
            if (mode_blob) drmModeDestroyPropertyBlob(fd, mode_blob);
            if (deg_blob)  drmModeDestroyPropertyBlob(fd, deg_blob);
            if (ctm_blob)  drmModeDestroyPropertyBlob(fd, ctm_blob);
            if (gam_blob)  drmModeDestroyPropertyBlob(fd, gam_blob);
            drmDropMaster(fd); close(fd); VT_CLEANUP(); return 1;
        }
        if (p_crtc_id)       drmModeAtomicAddProperty(req2, conn_id, p_crtc_id,  crtc_id);
                              drmModeAtomicAddProperty(req2, conn_id, p_hdr,      hdr_blob);
                              drmModeAtomicAddProperty(req2, conn_id, p_cspace,   cspace_val);
        if (p_bpc && !reset) drmModeAtomicAddProperty(req2, conn_id, p_bpc,      (uint64_t)max_bpc);
        if (p_crtc_act)      drmModeAtomicAddProperty(req2, crtc_id, p_crtc_act, 1);
        if (p_mode_id && mode_blob)
                             drmModeAtomicAddProperty(req2, crtc_id, p_mode_id,  mode_blob);

        hdr_ret = drmModeAtomicCommit(fd, req2, DRM_MODE_ATOMIC_NONBLOCK, NULL);
        if (hdr_ret != 0) {
            hdr_ret = drmModeAtomicCommit(fd, req2, DRM_MODE_ATOMIC_ALLOW_MODESET, NULL);
            printf("HDR+Colorspace modeset ret=%d  errno=%d (%s)\n",
                   hdr_ret, errno, hdr_ret ? strerror(errno) : "ok");
        } else {
            printf("HDR+Colorspace non-blocking commit: ok\n");
        }
        drmModeAtomicFree(req2);
        if (hdr_blob)  drmModeDestroyPropertyBlob(fd, hdr_blob);
        if (mode_blob) drmModeDestroyPropertyBlob(fd, mode_blob);

        if (p_vrr && vrr_saved && hdr_ret == 0) {
            usleep(100000);
            drmModeAtomicReqPtr vrr_req = drmModeAtomicAlloc();
            if (!vrr_req) {
                fprintf(stderr, "[kms-hdr] drmModeAtomicAlloc (vrr restore) failed\n");
                if (deg_blob) drmModeDestroyPropertyBlob(fd, deg_blob);
                if (ctm_blob) drmModeDestroyPropertyBlob(fd, ctm_blob);
                if (gam_blob) drmModeDestroyPropertyBlob(fd, gam_blob);
                drmDropMaster(fd); close(fd); VT_CLEANUP(); return 1;
            }
            drmModeAtomicAddProperty(vrr_req, conn_id, p_vrr, vrr_saved);
            int vrr_ret = drmModeAtomicCommit(fd, vrr_req, DRM_MODE_ATOMIC_NONBLOCK, NULL);
            drmModeAtomicFree(vrr_req);
            printf("VRR restored ret=%d\n", vrr_ret);
        }
    }

    drmDropMaster(fd);
    drmModeDestroyPropertyBlob(fd, deg_blob);
    drmModeDestroyPropertyBlob(fd, ctm_blob);
    drmModeDestroyPropertyBlob(fd, gam_blob);
    close(fd);

    if (tty_fd >= 0) {
        printf("DRM master released — switching back to VT1...\n");
        vt_switch(tty_fd, 1);
        close(tty_fd);
    }

    if (ret == 0 && hdr_ret == 0) {
        if (reset) {
            printf("✓ reset: SDR restored\n");
        } else if (is_nvidia) {
            printf("✓ NVIDIA HDR ACTIVE (PQ tonemapping only — no gamut expansion)\n"
                   "  sRGB→midtone→PQ via legacy gamma  SDR white=%d nits  peak=%d nits\n"
                   "  midtone_gamma=%d%%  bpc=%d\n",
                   sdr_nits, peak_nits, midtone_gamma, max_bpc);
        } else {
            printf("✓ HDR10 ACTIVE: sRGB→linear→%s→midtone→PQ pipeline live\n"
                   "  SDR white=%d nits  peak=%d nits  gamut=%d%%  sat=%d%%  "
                   "midtone_gamma=%d%%  bpc=%d\n",
                   gmode_str, sdr_nits, peak_nits, gamut_pct, saturation_pct,
                   midtone_gamma, max_bpc);
        }

        if (!reset && access("/dev/cec0", F_OK) == 0) {
            const char *cec_bin = NULL;
            if      (access("/usr/bin/cec-ctl",       X_OK) == 0) cec_bin = "/usr/bin/cec-ctl";
            else if (access("/usr/local/bin/cec-ctl", X_OK) == 0) cec_bin = "/usr/local/bin/cec-ctl";

            if (!cec_bin) {
                printf("HDMI-CEC: cec-ctl not found — install v4l-utils for CEC control\n");
            } else {
                pid_t pid = fork();
                if (pid == 0) {
                    /* child: silence output, exec absolute-path cec-ctl */
                    int devnull = open("/dev/null", O_WRONLY);
                    if (devnull >= 0) { dup2(devnull, 1); dup2(devnull, 2); close(devnull); }
                    execl(cec_bin, "cec-ctl", "--device=0", "--active-source",
                          "phys-addr=0.0.0.0", (char *)NULL);
                    _exit(127);
                } else if (pid > 0) {
                    int status = 0;
                    waitpid(pid, &status, 0);
                    int ok = WIFEXITED(status) && WEXITSTATUS(status) == 0;
                    printf("HDMI-CEC: %s\n", ok
                           ? "active-source announced ✓"
                           : "cec-ctl failed");
                } else {
                    perror("HDMI-CEC: fork");
                }
            }
        }
    } else {
        printf("pipeline ret=%d  HDR ret=%d\n", ret, hdr_ret);
    }

#undef VT_CLEANUP
    return (ret != 0);
}

/* ── daemon loop ─────────────────────────────────────────────────────────── */
/*
 * Stays alive after initial apply. Writes PID file. Installs SIGUSR1 handler.
 * On SIGUSR1: re-reads conf and calls do_apply() again.
 * Panel sends reload via: pkexec kms-hdr --reload
 */
static void run_daemon(ApplyCtx *base_ctx) {
    int pfd;
    for (;;) {
        pfd = open(PID_FILE, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
        if (pfd >= 0) break;
        if (errno != EEXIST) { perror("run_daemon: open " PID_FILE); return; }

        /* PID file exists — check if the recorded daemon is still alive */
        FILE *ex = fopen(PID_FILE, "r");
        int old_pid = 0;
        if (ex) { if (fscanf(ex, "%d", &old_pid) != 1) old_pid = 0; fclose(ex); }

        char procpath[64];
        snprintf(procpath, sizeof(procpath), "/proc/%d", old_pid);
        if (old_pid > 0 && access(procpath, F_OK) == 0) {
            fprintf(stderr, "daemon already running (PID %d)\n", old_pid);
            return;
        }
        /* stale PID file — remove and retry */
        if (unlink(PID_FILE) != 0 && errno != ENOENT) {
            perror("run_daemon: unlink stale " PID_FILE);
            return;
        }
    }
    {
        char pidbuf[32];
        int len = snprintf(pidbuf, sizeof(pidbuf), "%d\n", getpid());
        if (len > 0) { ssize_t w = write(pfd, pidbuf, (size_t)len); (void)w; }
        close(pfd);
    }

    signal(SIGUSR1, sigusr1_handler);
    signal(SIGTERM, SIG_DFL); /* clean exit on SIGTERM */

    printf("kms-hdr daemon running (PID %d). Send SIGUSR1 or run 'kms-hdr --reload' to re-apply.\n",
           getpid());

    /* Use inotify on /etc/kms-hdr.conf as secondary trigger (complements SIGUSR1) */
    int ifd = inotify_init1(IN_CLOEXEC | IN_NONBLOCK);
    if (ifd >= 0)
        inotify_add_watch(ifd, CONF_PATH, IN_CLOSE_WRITE | IN_MOVED_TO);

    for (;;) {
        fd_set rfds;
        FD_ZERO(&rfds);
        if (ifd >= 0) FD_SET(ifd, &rfds);
        /* select blocks until inotify event or signal (EINTR) */
        int sr = select(ifd >= 0 ? ifd + 1 : 1, &rfds, NULL, NULL, NULL);

        int triggered = g_reload;
        if (sr > 0 && ifd >= 0 && FD_ISSET(ifd, &rfds)) {
            /* drain inotify events */
            char ibuf[sizeof(struct inotify_event) + NAME_MAX + 1];
            while (read(ifd, ibuf, sizeof(ibuf)) > 0) {}
            triggered = 1;
        }

        if (triggered) {
            g_reload = 0;
            int sn, pn, gp, gm, bpc, sat, odm, mtg, fo;
            load_conf(&sn, &pn, &gp, &gm, &bpc, &sat, &odm, &mtg, &fo);
            ApplyCtx ctx = *base_ctx;
            ctx.sdr_nits      = sn;
            ctx.peak_nits     = pn;
            ctx.gamut_pct     = gp;
            ctx.gamut_mode    = gm;
            ctx.max_bpc       = bpc;
            ctx.saturation_pct = sat;
            ctx.midtone_gamma  = mtg;
            /* In daemon re-apply, trust saved conf values over EDID re-detection */
            ctx.explicit_peak       = 1;
            ctx.explicit_gamut_mode = 1;
            printf("\n[daemon] conf changed — re-applying...\n");
            do_apply(&ctx);
        }
    }

    if (ifd >= 0) close(ifd);
    unlink(PID_FILE);
}

/* ── main ────────────────────────────────────────────────────────────────── */
int main(int argc, char **argv) {
    /* --save-game: dispatched immediately */
    if (argc >= 2 && strcmp(argv[1], "--save-game") == 0) {
        if (geteuid() != 0) { fprintf(stderr, "must run as root (pkexec or sudo)\n"); return 1; }
        return save_game_conf(argc - 1, argv + 1);
    }

    /* --reload: send SIGUSR1 to running daemon */
    if (argc >= 2 && strcmp(argv[1], "--reload") == 0) {
        if (geteuid() != 0) { fprintf(stderr, "must run as root (pkexec or sudo)\n"); return 1; }
        FILE *pf = fopen(PID_FILE, "r");
        if (!pf) { fprintf(stderr, "daemon not running (no %s)\n", PID_FILE); return 1; }
        int pid = 0; fscanf(pf, "%d", &pid); fclose(pf);
        if (pid <= 0) { fprintf(stderr, "invalid PID in %s\n", PID_FILE); return 1; }
        if (kill(pid, SIGUSR1) != 0) { perror("kill"); return 1; }
        printf("reload signal sent to daemon PID %d\n", pid);
        return 0;
    }

    ApplyCtx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.sdr_nits      = DEFAULT_SDR_NITS;
    ctx.peak_nits     = DEFAULT_PEAK_NITS;
    ctx.gamut_pct     = DEFAULT_GAMUT;
    ctx.gamut_mode    = DEFAULT_GAMUT_MODE;
    ctx.max_bpc       = DEFAULT_MAX_BPC;
    ctx.saturation_pct = DEFAULT_SATURATION;
    ctx.midtone_gamma  = DEFAULT_MIDTONE_GAMMA;

    int save = 0, save_only = 0, daemon_mode = 0;
    int oled_dim_min = 0, force_oled = 0;

    /* Load defaults from conf */
    {
        int sn, pn, gp, gm, bpc, sat, odm, mtg, fo;
        load_conf(&sn, &pn, &gp, &gm, &bpc, &sat, &odm, &mtg, &fo);
        ctx.sdr_nits       = sn;
        ctx.peak_nits      = pn;
        ctx.gamut_pct      = gp;
        ctx.gamut_mode     = gm;
        ctx.max_bpc        = bpc;
        ctx.saturation_pct = sat;
        ctx.midtone_gamma  = mtg;
        oled_dim_min = odm;
        force_oled   = fo;
    }

    for (int i = 1; i < argc; i++) {
        if      (strcmp(argv[i], "reset")            == 0) ctx.reset         = 1;
        else if (strcmp(argv[i], "--help")            == 0 ||
                 strcmp(argv[i], "-h")                == 0) { usage(argv[0]); return 0; }
        else if (strcmp(argv[i], "--save")            == 0) save             = 1;
        else if (strcmp(argv[i], "--save-only")       == 0) save_only        = 1;
        else if (strcmp(argv[i], "--no-vt-switch")    == 0) ctx.no_vt_switch = 1;
        else if (strcmp(argv[i], "--daemon")          == 0) daemon_mode      = 1;
        else if ((strcmp(argv[i], "--card")   == 0 ||
                  strcmp(argv[i], "--output") == 0) && i+1 < argc)
            snprintf(ctx.card_path, sizeof(ctx.card_path), "%s", argv[++i]);
        else if ((strcmp(argv[i], "--connector") == 0 ||
                  strcmp(argv[i], "--display")   == 0) && i+1 < argc)
            snprintf(ctx.conn_name, sizeof(ctx.conn_name), "%s", argv[++i]);
        else if (strcmp(argv[i], "--sdr-nits")      == 0 && i+1 < argc) ctx.sdr_nits      = atoi(argv[++i]);
        else if (strcmp(argv[i], "--peak-nits")     == 0 && i+1 < argc) { ctx.peak_nits = atoi(argv[++i]); ctx.explicit_peak = 1; }
        else if (strcmp(argv[i], "--gamut")         == 0 && i+1 < argc) ctx.gamut_pct     = atoi(argv[++i]);
        else if (strcmp(argv[i], "--bpc")           == 0 && i+1 < argc) ctx.max_bpc       = atoi(argv[++i]);
        else if (strcmp(argv[i], "--saturation")    == 0 && i+1 < argc) ctx.saturation_pct = atoi(argv[++i]);
        else if (strcmp(argv[i], "--midtone-gamma") == 0 && i+1 < argc) ctx.midtone_gamma  = atoi(argv[++i]);
        else if (strcmp(argv[i], "--oled-dim-min")  == 0 && i+1 < argc) oled_dim_min       = atoi(argv[++i]);
        else if (strcmp(argv[i], "--force-oled")    == 0 && i+1 < argc) force_oled         = atoi(argv[++i]);
        else if (strcmp(argv[i], "--gamut-mode")    == 0 && i+1 < argc) {
            const char *m = argv[++i];
            ctx.gamut_mode = strcmp(m, "dci-p3") == 0 ? 1 :
                             strcmp(m, "srgb")   == 0 ? 2 : 0;
            ctx.explicit_gamut_mode = 1;
        }
        else if (strcmp(argv[i], "--dim-to") == 0 && i+1 < argc) {
            int n = atoi(argv[++i]);
            if (n < 1) { fprintf(stderr, "[kms-hdr] --dim-to: value must be >= 1\n"); return 1; }
            ctx.sdr_nits  = n;
            ctx.peak_nits = n * 4;
            ctx.explicit_peak = 1;
        }
        /* forward-compat passthrough */
        else if (strcmp(argv[i], "--oled-preset") == 0 && i+1 < argc) { ++i; }
        else { fprintf(stderr, "unknown arg: %s  (try --help)\n", argv[i]); return 1; }
    }

    if (geteuid() != 0) { fprintf(stderr, "must run as root (pkexec or sudo)\n"); return 1; }

    if ((save || save_only) && !ctx.reset)
        save_conf(ctx.sdr_nits, ctx.peak_nits, ctx.gamut_pct, ctx.gamut_mode,
                  ctx.max_bpc, ctx.saturation_pct, oled_dim_min, ctx.midtone_gamma, force_oled);

    if (save_only) return 0;

    int ret = do_apply(&ctx);

    if (ret == 0 && daemon_mode)
        run_daemon(&ctx);  /* enters infinite loop */

    return ret;
}
