# kms-hdr

**HDR, wide-gamut colour, and NVIDIA feature parity for Wayland compositors that
have no colour management** — COSMIC, wlroots, and anything else that hasn't
implemented `wp-color-management-v1` yet.

Most Linux desktops can't drive an HDR or wide-gamut display because the
compositor never programs the display's colour pipeline. `kms-hdr` does it
directly through the kernel's atomic KMS API — the same DRM colour properties a
compositor *would* use — so you get vivid, calibrated HDR output today, without
waiting for the compositor to catch up.

```
┌─ kms-hdr ──────────────────────────────────────────────────────────────────┐
│  DEGAMMA (sRGB→linear) → CTM (BT.709 → BT.2020/DCI-P3 × saturation × white   │
│  balance) → GAMMA (linear → midtone γ → highlight roll-off → black lift → PQ)│
│  → HDR_OUTPUT_METADATA (ST 2084) + Colorspace=BT2020_RGB + max_bpc           │
└─────────────────────────────────────────────────────────────────────────────┘
```

## Why this is hard

- **Compositors gate colour management.** On Wayland the compositor owns the
  display pipeline. If it doesn't implement `wp-color-management-v1` (COSMIC
  doesn't yet), nothing can put the panel into HDR or a wide gamut. `kms-hdr`
  takes DRM master and programs the CRTC's `DEGAMMA_LUT` / `CTM` / `GAMMA_LUT` /
  `HDR_OUTPUT_METADATA` / `Colorspace` properties itself.
- **NVIDIA exposes no atomic colour properties.** The proprietary driver has no
  `CTM` / `DEGAMMA_LUT` / `GAMMA_LUT` atomic props — so on NVIDIA the only path
  is legacy per-channel gamma ramps (`drmModeCrtcSetGamma`). `kms-hdr` detects
  the GPU and folds the *entire* transform (white balance, gamma, tone-map) into
  those ramps so NVIDIA users still get HDR.
- **It's real colour science.** Correct sRGB→linear de-gamma, a BT.709→BT.2020
  matrix, linear-light saturation, a CCT-derived white point, ST 2084 (PQ)
  encoding, and a BT.2390-style highlight roll-off — all quantised into 16-bit
  LUTs and committed atomically (with a `TEST_ONLY` dry-run so a rejected state
  never flashes the screen).

## Repository layout

This is a Cargo workspace monorepo with one shared source of truth.

| Path | What |
|------|------|
| `core/` | The C injector (`kms-hdr.c`, libdrm) — the kernel-facing colour pipeline, daemon, polkit policy, systemd unit. |
| `crates/kms-hdr-core` | Shared Rust library: config I/O, EDID parsing, GPU detection, calibration profiles, injector invocation. Pure `std`, no heavy deps. Unit-tested. |
| `crates/kms-hdr-cal` | Native GTK4 calibration wizard (below). |
| `docs/ROADMAP.md` | The plan to grow this into a full NVIDIA control panel for COSMIC. |

The libcosmic settings panel and the cosmic-settings integration consume
`kms-hdr-core` (migration in progress — see the roadmap).

## The calibration wizard

`kms-hdr-cal` is a nine-step, eyeball-driven wizard that previews **live**
through the injector — going beyond KDE's single-slider HDR calibration:

1. **Display info** — EDID peak, advertised gamut/EOTF, OLED + GPU pipeline.
2. **Black level** — PLUGE-style near-black patches to set the shadow floor
   (OLED black-crush recovery).
3. **Peak luminance** — the clipping-cross test: lower peak nits until the bright
   detail just disappears, finding the panel's true peak.
4. **SDR white reference** — how bright SDR/desktop white appears in HDR.
5. **Gamut & vividness** — output gamut + saturation, judged against a skin-tone
   patch.
6. **White balance** — temperature (CCT) + tint against a neutral grey field.
   *KDE has no white-balance control.*
7. **Midtone gamma** — a luminance ramp for contrast vs shadow detail.
8. **Highlight roll-off** — soft BT.2390 knee instead of hard clipping.
   *KDE has none.*
9. **Summary & save** — apply and optionally store a named profile.

Profiles are saved under `~/.config/kms-hdr/profiles/` in the same format as the
system config, so any profile can be applied verbatim by the injector.

## Build & install

Requires: `libdrm`, a Rust toolchain, GTK 4, `pkexec` (polkit).

```sh
make            # builds the C injector + the Rust workspace
sudo make install
```

This installs `kms-hdr`, `kms-hdr-cal`, `hdr-game`, the systemd unit, the polkit
policy (active-session, no password), and a desktop entry.

## Usage

```sh
# enable HDR with sane defaults (auto-detects card/connector/EDID peak)
sudo kms-hdr

# tune individual knobs and persist them
sudo kms-hdr --peak-nits 1000 --gamut-mode bt2020 --white-temp 6504 \
             --tonemap rolloff --tonemap-knee 60 --save

# run the calibration wizard (applies live, saves on finish)
kms-hdr-cal

# restore SDR
sudo kms-hdr reset

# run as a daemon that re-applies on config change / SIGUSR1
sudo kms-hdr --daemon
```

Run `kms-hdr --help` for the full flag list.

## Status

Used daily on an RTX 5090 (Blackwell) + 4K HDR display under COSMIC. AMD/Intel
get the full atomic `DEGAMMA+CTM+GAMMA` pipeline; NVIDIA gets the legacy-gamma
fallback. HDR signalling (PQ/BT.2020/DCI-P3) works on both. The
[roadmap](docs/ROADMAP.md) tracks the path to full NVIDIA feature parity
(RTX-HDR-alike inverse tone-mapping, VRR, sharpening, per-app profiles).

## License

GPL-3.0-or-later.
