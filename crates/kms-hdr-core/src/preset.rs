//! Factory-tuned panel presets.
//!
//! A [`Preset`] is a starting-point [`Conf`] hand-tuned for a class of display,
//! so plugging in a known panel gets correct HDR output in one click instead of
//! a full manual calibration pass. [`recommend`] picks the best match from the
//! detected [`DisplayInfo`]; the UI still lets the user override.
//!
//! Tuning assumes the panel's own dynamic tone-mapping is **off** (LG: HGiG /
//! "PC" icon, or dynamic tone-mapping disabled) so this injector owns the PQ
//! roll-off — otherwise the TV double-maps highlights.
//!
//! Luminance/gamut figures are real-world measured ballparks for these panels:
//! LG WOLED covers ~98% DCI-P3 / ~74% BT.2020, so DCI-P3 is the honest gamut
//! target (BT.2020 expansion overshoots what the panel can actually render).
//! 2026 models (G6/C6) are provisional until measured and err on the safe side.

use crate::conf::{Conf, GamutMode, Tonemap};
use crate::edid::DisplayInfo;

/// A named, hand-tuned configuration for a class of display.
pub struct Preset {
    /// Stable identifier (used by UIs / `--preset`).
    pub id: &'static str,
    /// Human label.
    pub label: &'static str,
    /// One-line rationale shown under the label.
    pub desc: &'static str,
    /// Builder for the tuned configuration.
    build: fn() -> Conf,
}

impl Preset {
    /// Materialize this preset's configuration.
    pub fn conf(&self) -> Conf {
        (self.build)()
    }
}

// --- tuned configurations -------------------------------------------------
//
// Shared OLED baseline: D65 white, BT.2408 reference SDR white (203 nits),
// 10-bit PQ, soft BT.2390 highlight roll-off, idle auto-dim for panel care.

fn lg_g5() -> Conf {
    Conf {
        sdr_nits: 203,
        peak_nits: 2300, // 4-stack Primary RGB Tandem WOLED, ~2100-2700 nits @10%
        gamut_mode: GamutMode::DciP3,
        gamut_pct: 100,
        saturation: 100,
        oled_dim_min: 10,
        force_oled: true,
        white_temp: 6504,
        black_lift: 0,
        tonemap: Tonemap::Rolloff,
        tonemap_knee: 68, // bright panel -> late, gentle roll-off
        ..Conf::default()
    }
}

fn lg_g6() -> Conf {
    Conf {
        peak_nits: 2700, // 2026 flagship, provisional (>= G5)
        tonemap_knee: 70,
        ..lg_g5()
    }
}

fn lg_c6() -> Conf {
    Conf {
        peak_nits: 1300, // C-series evo panel, ~1200-1500 nits @10%
        tonemap_knee: 55,
        ..lg_g5()
    }
}

fn generic_woled() -> Conf {
    Conf {
        peak_nits: 800,
        tonemap_knee: 50,
        ..lg_g5()
    }
}

fn generic_qd_oled() -> Conf {
    Conf {
        peak_nits: 1000,
        gamut_mode: GamutMode::Bt2020, // QD-OLED reaches ~90% BT.2020
        tonemap_knee: 55,
        ..lg_g5()
    }
}

fn generic_oled() -> Conf {
    Conf {
        peak_nits: 700,
        tonemap_knee: 48,
        ..lg_g5()
    }
}

fn hisense_oled() -> Conf {
    Conf {
        peak_nits: 600,
        tonemap_knee: 45,
        ..lg_g5()
    }
}

fn oled_safe_desktop() -> Conf {
    Conf {
        sdr_nits: 120, // dimmer SDR white = less static-load burn-in risk
        peak_nits: 600,
        oled_dim_min: 5,
        tonemap_knee: 45,
        ..lg_g5()
    }
}

/// Every selectable preset, ordered for UI presentation.
pub static PRESETS: &[Preset] = &[
    Preset {
        id: "lg-g6",
        label: "LG G6 OLED (2026)",
        desc: "4-stack tandem WOLED · ~2700 nits · DCI-P3 · provisional",
        build: lg_g6,
    },
    Preset {
        id: "lg-g5",
        label: "LG G5 OLED (2025)",
        desc: "Primary RGB Tandem WOLED · ~2300 nits · DCI-P3",
        build: lg_g5,
    },
    Preset {
        id: "lg-c6",
        label: "LG C6 OLED (2026)",
        desc: "evo WOLED · ~1300 nits · DCI-P3",
        build: lg_c6,
    },
    Preset {
        id: "woled",
        label: "Generic WOLED",
        desc: "LG-display WOLED, unknown model · ~800 nits · DCI-P3",
        build: generic_woled,
    },
    Preset {
        id: "qd-oled",
        label: "Generic QD-OLED",
        desc: "Samsung/Sony QD-OLED · ~1000 nits · BT.2020",
        build: generic_qd_oled,
    },
    Preset {
        id: "oled",
        label: "Generic OLED",
        desc: "Unknown OLED panel · ~700 nits · DCI-P3",
        build: generic_oled,
    },
    Preset {
        id: "hisense-oled",
        label: "Hisense OLED",
        desc: "Hisense OLED TV · ~600 nits · DCI-P3",
        build: hisense_oled,
    },
    Preset {
        id: "oled-desktop",
        label: "OLED-safe desktop",
        desc: "Longevity-first: dim SDR white, fast auto-dim",
        build: oled_safe_desktop,
    },
];

/// Look up a preset by its stable `id`.
pub fn by_id(id: &str) -> Option<&'static Preset> {
    PRESETS.iter().find(|p| p.id == id)
}

/// Pick the best preset for a detected display, or `None` if nothing matches
/// confidently (caller should keep the existing/default config).
pub fn recommend(info: &DisplayInfo) -> Option<&'static Preset> {
    let name = info.name.as_deref().unwrap_or("").to_uppercase();
    let has = |tok: &str| name.contains(tok);

    if info.is_lg() {
        let id = if has("G6") {
            "lg-g6"
        } else if has("G5") {
            "lg-g5"
        } else if has("C6") || has("C5") {
            "lg-c6"
        } else {
            "woled" // LG panel, model not in EDID -> generic WOLED tuning
        };
        return by_id(id);
    }
    if has("HISENSE") {
        return by_id("hisense-oled");
    }
    if has("QD-OLED") || has("S95") || has("S90") {
        return by_id("qd-oled");
    }
    if info.is_oled {
        return by_id("oled");
    }
    None
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn lg_models_inherit_oled_baseline() {
        for f in [lg_g5, lg_g6, lg_c6] {
            let c = f();
            assert!(c.force_oled);
            assert_eq!(c.tonemap, Tonemap::Rolloff);
            assert_eq!(c.gamut_mode, GamutMode::DciP3);
            assert_eq!(c.white_temp, 6504);
        }
        // brighter panels roll off later
        assert!(lg_g6().tonemap_knee >= lg_g5().tonemap_knee);
        assert!(lg_g5().tonemap_knee > lg_c6().tonemap_knee);
        assert!(lg_g6().peak_nits > lg_c6().peak_nits);
    }

    #[test]
    fn ids_are_unique_and_resolvable() {
        for p in PRESETS {
            assert!(by_id(p.id).is_some(), "{} not resolvable", p.id);
        }
        let mut ids: Vec<_> = PRESETS.iter().map(|p| p.id).collect();
        ids.sort();
        let n = ids.len();
        ids.dedup();
        assert_eq!(ids.len(), n, "duplicate preset id");
    }

    #[test]
    fn recommend_lg_by_manufacturer() {
        let lg = DisplayInfo {
            manufacturer: Some("GSM".into()),
            is_oled: true,
            ..Default::default()
        };
        assert_eq!(recommend(&lg).unwrap().id, "woled");

        let g5 = DisplayInfo {
            manufacturer: Some("GSM".into()),
            name: Some("LG TV OLED G5".into()),
            ..Default::default()
        };
        assert_eq!(recommend(&g5).unwrap().id, "lg-g5");
    }

    #[test]
    fn recommend_hisense_and_generic_and_none() {
        let his = DisplayInfo {
            name: Some("HISENSE".into()),
            is_oled: true,
            ..Default::default()
        };
        assert_eq!(recommend(&his).unwrap().id, "hisense-oled");

        let oled = DisplayInfo {
            name: Some("Some Panel".into()),
            is_oled: true,
            ..Default::default()
        };
        assert_eq!(recommend(&oled).unwrap().id, "oled");

        let lcd = DisplayInfo {
            name: Some("Dell U2720Q".into()),
            ..Default::default()
        };
        assert!(recommend(&lcd).is_none());
    }
}
