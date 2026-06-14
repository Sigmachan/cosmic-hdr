# kms-hdr

HDR10 color pipeline injector for Linux — COSMIC Desktop, KDE Plasma 6, or any compositor.

Brings HDR, wide-gamut (BT.2020 / DCI-P3 D65), and 10/12-bit output to Linux desktops via the DRM kernel API, with a GUI settings panel built on libcosmic. Works out of the box with **smart defaults** derived from your display's EDID — no manual configuration needed to get a great picture.

---

## Automatic defaults

On first run, kms-hdr reads your display's EDID and picks the best settings automatically:

- **Peak nits** — taken from the EDID HDR Static Metadata luminance field. Falls back to 800 nits (safe for most HDR OLED/QLED panels) if the EDID field is unset.
- **SDR white** — 203 nits (ITU-R BT.2408 reference, the broadcast standard for SDR-in-HDR). This is the value the panel's interactive calibration helps you fine-tune if needed.
- **Gamut** — BT.2020 at 100% if the display reports BT.2020 or DCI-P3 support in its colorimetry block; otherwise sRGB passthrough.
- **Bit depth** — 10 bpc if HDR10 support is detected in EDID; 8 bpc otherwise.
- **VRR** — automatically preserved (no black-screen glitch on FreeSync / G-Sync displays).

The GUI panel shows everything derived from EDID — interface version (HDMI 2.1, DP 1.4), HDR formats supported (HDR10, HLG, HDR10+, Dolby Vision), DCI-P3, DSC, HDMI-CEC availability, and peak luminance — so you can see exactly what your display reported and tune from there.

---

## What it does

| Feature | AMD / Intel | NVIDIA (desktop) | NVIDIA (gaming, hdr-game) |
|---------|:-----------:|:----------------:|:-------------------------:|
| HDR10 metadata (PQ / ST 2084) | ✓ | ✓ | ✓ (Gamescope) |
| Wide gamut — BT.2020 / DCI-P3 | ✓ | — ¹ | ✓ (Gamescope CTM) |
| Colour saturation matrix | ✓ | — ¹ | — |
| 10 / 12 bpc output | ✓ | ✓ | ✓ |
| VRR / FreeSync / G-Sync safe | ✓ | ✓ | ✓ |
| Auto-detect display from EDID | ✓ | ✓ | ✓ |
| RTX Smooth Motion | — | — | ✓ (Vulkan layer) |
| NVIDIA Reflex | — | — | ✓ (NvAPI) |
| Digital Vibrance | — | — | ✓ (nvibrant) |
| FSR / NIS / DLSS upscaling | — | — | ✓ (Gamescope) |
| DLDSR | — | — | ✓ |

¹ NVIDIA's KMS driver does not expose the DRM CTM / DEGAMMA_LUT / GAMMA_LUT CRTC properties needed for full colour-pipeline control. On NVIDIA, kms-hdr falls back to a legacy 1D PQ gamma ramp via `drmModeCrtcSetGamma()` — the same technique KWin Plasma 6 uses on NVIDIA. BT.2020 / DCI-P3 gamut expansion and saturation are silently skipped. Use `hdr-game` for full feature support in games.

---

## Components

| Component | Location | Description |
|-----------|----------|-------------|
| `kms-hdr` | `/usr/local/bin/kms-hdr` | KMS binary — runs as root via polkit |
| `kms-hdr-panel` | `/usr/local/bin/kms-hdr-panel` | libcosmic settings panel |
| `hdr-cal.py` | `/usr/local/lib/kms-hdr/hdr-cal.py` | GTK4 calibration / test-pattern overlay |
| `kms-hdr.service` | systemd | Applies HDR at login, resets at logout |
| `kms-hdr.policy` | polkit | Allows the active session to invoke the binary without a password prompt |

---

## Display detection

Both the C binary and the Rust panel auto-detect your active display by enumerating `/sys/class/drm/card*-*/edid`. No hardcoded paths — survives GPU changes, cable swaps, and multi-GPU setups. The first connector with a valid EDID (≥ 128 bytes) wins.

The panel reads the EDID and reports:

| Field | Source |
|-------|--------|
| HDR10, HLG, HDR10+, Dolby Vision | CTA-861 HDR Static Metadata block + VSVDB |
| BT.2020, DCI-P3 | CTA-861 Colorimetry block (ext_tag=5) |
| HDMI 1.4 / 2.0 / 2.1 | HDMI Licensing VSDB (OUI 0x000C03) + HDMI Forum VSDB (OUI 0xC45D00) |
| DSC | EDID HF-VSDB DSC bit + `/sys/class/drm/*/dsc_enable` sysfs |
| HDMI-CEC | `/dev/cec0` present |
| Peak nits | CTA-861 HDR Static Metadata `Maximum Luminance` |

---

## HDMI version detection

The HDMI Forum VSDB (OUI `0xC45D00`) carries the `Max TMDS Character Rate` field. Rate × 5 MHz ≥ 600 MHz → HDMI 2.1. Lower rate → HDMI 2.0. If only the HDMI Licensing VSDB (OUI `0x000C03`) is present, the display is HDMI 1.4.

---

## DCI-P3

**Does it work?** Yes, on AMD and Intel.

When you select `DCI-P3 D65` as the Target Gamut, kms-hdr computes and loads a 3×3 CTM that maps sRGB primaries to DCI-P3 D65 primaries into the GPU's display engine. All desktop content — not just video — is rendered with the wider colour gamut. This is the same P3 colour space used by Apple displays and modern HDR TVs.

Note: `DCI-P3 D65` uses the standard D65 white point (6504 K), matching sRGB and BT.2020. The cinema native `P3-ST 431-2` (D63, 6300 K) is intentionally not exposed — it's too warm for desktop use and only correct for cinema projection booths.

On **NVIDIA**, the gamut mode is ignored silently — the NVIDIA KMS driver does not expose CTM properties (a driver limitation). PQ tone mapping still works.

---

## HDMI-CEC

**Detection: yes. Active control: shell only (for now).**

The panel detects whether `/dev/cec0` exists (Linux kernel CEC framework, exposed by GPU drivers that support it). The badge in the Display panel shows `CEC ✓` or `CEC —`.

CEC is a separate sideband channel on the HDMI cable. It cannot toggle HDR mode — the display switches to HDR by reading the HDR10 InfoFrame embedded in the video signal, which is exactly what kms-hdr sets via `HDR_OUTPUT_METADATA`. CEC and HDR metadata are completely independent.

What CEC _can_ do via `cec-ctl` (from `v4l-utils`):

```bash
# Announce this PC as the active HDMI source (wakes TV, switches input)
cec-ctl --device=0 --active-source phys-addr=0.0.0.0

# Power the TV on
cec-ctl --device=0 --image-view-on

# Put the TV in standby
cec-ctl --device=0 --standby
```

CEC source announcement at HDR-enable time is on the roadmap.

---

## HDR Calibration

### Interactive calibration (`Calibrate…` button in the panel)

Opens a fullscreen GTK4 overlay with a nested-box reference pattern:

- **Dark background** — represents deep shadow / HDR dark content
- **Outer gray box** — 18% reflectance reference (standard film / TV neutral gray)
- **Inner white box** — SDR reference white at the selected brightness

Drag the slider. The pipeline applies live (via `pkexec`, no password prompt — polkit `allow_active = yes`). The target: the inner white box should look naturally bright — clearly whiter than the 18% gray, but not blinding. A good starting point for HDR OLED is 203 nits (ITU-R BT.2408).

Press **Save & Apply** to commit the value. Press **Close** or **Esc** to discard.

### Test patterns

| Pattern | Use |
|---------|-----|
| Black | Verify black crush / near-black clipping |
| 5% Gray | EOTF accuracy near black (should be barely visible, not crushed to black) |
| 50% Gray | Mid-tone reference |
| White | Check HDR white clip / overexposure |
| Red / Green / Blue | Gamut primaries — check after CTM / gamut changes |
| SDR\|HDR Split | Left = 20% SDR gray, right = full HDR white; compare tone-mapping |

Click anywhere or press **Esc** to close a test pattern.

---

## Installation

### From source

```bash
# C binary + service + polkit + calibration tool
git clone https://github.com/Sigmachan/kms-hdr
cd kms-hdr
make
sudo make install
sudo make enable          # enables and starts kms-hdr.service

# Rust panel (optional)
git clone https://github.com/Sigmachan/kms-hdr-panel
cd kms-hdr-panel
cargo build --release
sudo install -Dm755 target/release/kms-hdr-panel /usr/local/bin/kms-hdr-panel
```

**Dependencies:** `libdrm`, `python-gobject` (GTK4 Python bindings, for `hdr-cal.py`), `polkit`, `systemd`.

### Arch Linux (PKGBUILD)

```bash
# Inside the kms-hdr directory
makepkg -si
```

---

## NVIDIA Gaming

On NVIDIA, the KMS driver doesn't expose the CTM/DEGAMMA/GAMMA properties needed for full colour-pipeline control, so the desktop-level HDR features (gamut expansion, saturation) are limited. Instead, **gaming features work through [Gamescope](https://github.com/ValveSoftware/gamescope)** via the `hdr-game` launcher script included in this package.

### hdr-game

Wraps any Vulkan game or Steam launch command in Gamescope with:

| Feature | Implementation |
|---------|---------------|
| HDR10 output | `--hdr-enabled` + ITM tone-mapping |
| RTX Smooth Motion | `NVPRESENT_ENABLE_SMOOTH_MOTION=1` (VK_LAYER_NV_present implicit layer) |
| NVIDIA Reflex | `PROTON_ENABLE_NVAPI=1` + `DXVK_ENABLE_NVAPI=1` (low latency via NvAPI) |
| Digital Vibrance | `nvibrant <value>` (ioctl to `/dev/nvidia-modeset`, Wayland-native) |
| FSR / NIS / DLSS | Passed through to Gamescope upscaling |
| DLDSR | `__GL_DLDSR_MULTIPLIER=2.25` + render resolution override |
| MangoApp overlay | `--mangoapp` flag |
| VRR / Adaptive Sync | `--adaptive-sync` |

**Steam launch option:**

```
ENABLE_GAMESCOPE_WSI=1 DXVK_HDR=1 hdr-game %command%
```

**Manual launch:**

```bash
hdr-game -- /path/to/game --game-args
hdr-game --vibrance 400 --mango --upscale fsr -- %command%
hdr-game --no-smooth-motion --no-itm -- native-hdr-game
```

**All options:**

```
--vibrance N        Digital vibrance -1024..1023 (0=neutral, 512=vivid; NVIDIA only)
--no-smooth-motion  Disable RTX Smooth Motion
--no-reflex         Disable NVIDIA Reflex / NvAPI
--no-itm            Disable inverse tone-mapping (for native HDR10 titles)
--mango             Attach MangoApp overlay
--width W           Gamescope output width  (default: 3840)
--height H          Gamescope output height (default: 2160)
--fps N             Framerate cap           (default: 120)
--peak-nits N       HDR peak luminance override
--sdr-nits N        SDR white point override
--upscale fsr|nis|dlss|integer  Upscaling algorithm
```

Defaults for `--vibrance`, `--width`, `--height`, `--fps` are read from `/etc/hdr-game.conf` (managed by `kms-hdr-panel`). If the file doesn't exist, built-in defaults apply.

**`/etc/hdr-game.conf` keys:**

| Key | Default | Description |
|-----|---------|-------------|
| `SMOOTH_MOTION` | 1 | RTX Smooth Motion (0 or 1) |
| `REFLEX` | 1 | NVIDIA Reflex via NvAPI (0 or 1) |
| `VIBRANCE` | 0 | Digital vibrance, nvibrant scale -1024..1023 |
| `UPSCALE` | fsr | `fsr` / `nis` / `integer` / `nearest` |
| `DLDSR` | 0 | NVIDIA DLDSR (renders at 2.25× pixels, display native) |
| `GS_WIDTH` | 3840 | Gamescope output width |
| `GS_HEIGHT` | 2160 | Gamescope output height |
| `GS_FPS` | 120 | Gamescope target FPS |

---

## Configuration

Written to `/etc/kms-hdr.conf` by `kms-hdr --save ...` (runs as root via polkit):

```ini
SDR_NITS=203
PEAK_NITS=800
GAMUT=100
MAX_BPC=10
GAMUT_MODE=bt2020
SATURATION=100
OLED_DIM_MIN=0
```

| Key | Default | Description |
|-----|---------|-------------|
| `SDR_NITS` | 203 | SDR reference white in HDR mode (nits). ITU-R BT.2408 broadcast reference. |
| `PEAK_NITS` | 800 | Display peak luminance for HDR10 metadata. Match to your display's spec. |
| `GAMUT` | 100 | Gamut expansion blend 0–100%. 0 = sRGB passthrough, 100 = full target. |
| `MAX_BPC` | 10 | Bit depth requested via `max_requested_bpc` DRM property. |
| `GAMUT_MODE` | bt2020 | Target colour space: `bt2020`, `dci-p3`, or `srgb`. |
| `SATURATION` | 100 | Colour saturation via BT.709 matrix. 100 = neutral, 150 = vivid, 50 = muted. |
| `OLED_DIM_MIN` | 0 | Auto-dim to 50 nit after N minutes idle (0 = disabled; requires swayidle). |

---

## CLI reference

```
kms-hdr [OPTIONS] [COMMAND]

Commands:
  reset                     Reset to SDR (remove HDR metadata, restore gamma)

Options:
  --card <path>             DRM device (default: auto-detected from sysfs EDID)
  --connector <name>        Connector name, e.g. HDMI-A-2 (default: auto-detected)
  --output / --display      Aliases for --connector
  --sdr-nits <n>            SDR reference white, nits (default: config → 203)
  --peak-nits <n>           Display peak, nits (default: config → 800)
  --gamut <0-100>           Gamut expansion blend % (default: 100)
  --bpc <8|10|12>           Max requested bit depth (default: 10)
  --gamut-mode <mode>       bt2020 | dci-p3 | srgb (default: bt2020)
  --saturation <50-200>     Colour saturation % via BT.709 matrix (default: 100)
  --dim-to <nits>           Set SDR=N, peak=N×4 — used by OLED auto-dim service
  --no-vt-switch            Skip VT switch to steal DRM master (for headless/boot use)
  --save                    Write /etc/kms-hdr.conf before applying
  --help                    Show this help
```

---

## VRR / FreeSync / G-Sync compatibility

Applying HDR via a full modeset (`DRM_MODE_ATOMIC_ALLOW_MODESET`) while VRR is active causes a brief black flash as the display resyncs timing. kms-hdr avoids this:

1. Reads the current `vrr_enabled` property value before committing.
2. Disables VRR, applies the HDR pipeline.
3. Restores `vrr_enabled` to its original value.

Additionally, a non-blocking (`DRM_MODE_ATOMIC_NONBLOCK`) commit is attempted first for the HDR metadata properties, falling back to `ALLOW_MODESET` only if that fails — avoiding the resync entirely when the compositor is idle.

---

## License

GPL-3.0-only.

Maintainer: Kira Keller \<senedato@gmail.com\>
