//! Persisted kms-hdr configuration — mirrors the C injector's `KmsConf`.
//!
//! The on-disk format is plain `KEY=VALUE` lines at [`CONF_PATH`]. The same
//! serialization is reused for named calibration profiles (see [`crate::profile`]).

use std::fs;
use std::path::Path;

/// System config written/read by the root-owned injector.
pub const CONF_PATH: &str = "/etc/kms-hdr.conf";

/// Output gamut target for the CTM stage.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum GamutMode {
    /// BT.2020 primaries (widest).
    Bt2020,
    /// DCI-P3 primaries.
    DciP3,
    /// Identity — no gamut expansion (sRGB/BT.709).
    Srgb,
}

impl GamutMode {
    pub fn as_str(self) -> &'static str {
        match self {
            GamutMode::Bt2020 => "bt2020",
            GamutMode::DciP3 => "dci-p3",
            GamutMode::Srgb => "srgb",
        }
    }

    /// Parse a persisted `GAMUT_MODE` value, defaulting to BT.2020.
    pub fn from_conf_str(s: &str) -> GamutMode {
        match s.trim() {
            "dci-p3" => GamutMode::DciP3,
            "srgb" => GamutMode::Srgb,
            _ => GamutMode::Bt2020,
        }
    }

    /// Human label for UI.
    pub fn label(self) -> &'static str {
        match self {
            GamutMode::Bt2020 => "BT.2020 (widest)",
            GamutMode::DciP3 => "DCI-P3 (cinema)",
            GamutMode::Srgb => "sRGB (no expansion)",
        }
    }
}

/// Highlight handling for content above the knee point.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Tonemap {
    /// Hard clip at peak.
    Clip,
    /// BT.2390-style soft roll-off.
    Rolloff,
}

impl Tonemap {
    pub fn as_arg(self) -> &'static str {
        match self {
            Tonemap::Clip => "clip",
            Tonemap::Rolloff => "rolloff",
        }
    }
    pub fn as_int(self) -> i32 {
        match self {
            Tonemap::Clip => 0,
            Tonemap::Rolloff => 1,
        }
    }
}

/// Full persisted state. Field defaults mirror the C `DEFAULT_*` macros.
#[derive(Clone, Debug, PartialEq)]
pub struct Conf {
    pub sdr_nits: i32,
    pub peak_nits: i32,
    pub gamut_pct: i32,
    pub gamut_mode: GamutMode,
    pub max_bpc: i32,
    pub saturation: i32,
    pub oled_dim_min: i32,
    pub midtone_gamma: i32,
    pub force_oled: bool,
    pub white_temp: i32,
    pub white_tint: i32,
    pub black_lift: i32,
    pub tonemap: Tonemap,
    pub tonemap_knee: i32,
}

impl Default for Conf {
    fn default() -> Self {
        Conf {
            sdr_nits: 203,
            peak_nits: 800,
            gamut_pct: 100,
            gamut_mode: GamutMode::Bt2020,
            max_bpc: 10,
            saturation: 100,
            oled_dim_min: 0,
            midtone_gamma: 100,
            force_oled: false,
            white_temp: 6504,
            white_tint: 0,
            black_lift: 0,
            tonemap: Tonemap::Clip,
            tonemap_knee: 50,
        }
    }
}

impl Conf {
    /// Load the system config, falling back to defaults for anything missing.
    pub fn load() -> Conf {
        Conf::load_from(CONF_PATH)
    }

    /// Load from an arbitrary path (used for named profiles).
    pub fn load_from<P: AsRef<Path>>(path: P) -> Conf {
        let mut c = Conf::default();
        let Ok(text) = fs::read_to_string(path) else {
            return c;
        };
        c.merge_str(&text);
        c
    }

    /// Apply `KEY=VALUE` lines from `text` over the current values.
    pub fn merge_str(&mut self, text: &str) {
        for line in text.lines() {
            let line = line.trim();
            if line.is_empty() || line.starts_with('#') {
                continue;
            }
            let Some((k, v)) = line.split_once('=') else {
                continue;
            };
            let (k, v) = (k.trim(), v.trim());
            let int = |d: i32| v.parse::<i32>().unwrap_or(d);
            match k {
                "SDR_NITS" => self.sdr_nits = int(self.sdr_nits),
                "PEAK_NITS" => self.peak_nits = int(self.peak_nits),
                "GAMUT" => self.gamut_pct = int(self.gamut_pct),
                "MAX_BPC" => self.max_bpc = int(self.max_bpc),
                "SATURATION" => self.saturation = int(self.saturation),
                "OLED_DIM_MIN" => self.oled_dim_min = int(self.oled_dim_min),
                "MIDTONE_GAMMA" => self.midtone_gamma = int(self.midtone_gamma),
                "FORCE_OLED" => self.force_oled = int(self.force_oled as i32) != 0,
                "WHITE_TEMP" => self.white_temp = int(self.white_temp),
                "WHITE_TINT" => self.white_tint = int(self.white_tint),
                "BLACK_LIFT" => self.black_lift = int(self.black_lift),
                "TONEMAP" => {
                    self.tonemap = if int(self.tonemap.as_int()) != 0 {
                        Tonemap::Rolloff
                    } else {
                        Tonemap::Clip
                    }
                }
                "TONEMAP_KNEE" => self.tonemap_knee = int(self.tonemap_knee),
                "GAMUT_MODE" => self.gamut_mode = GamutMode::from_conf_str(v),
                _ => {}
            }
        }
    }

    /// Serialize to the canonical `KEY=VALUE` format.
    pub fn to_conf_string(&self) -> String {
        format!(
            "SDR_NITS={}\nPEAK_NITS={}\nGAMUT={}\nMAX_BPC={}\nGAMUT_MODE={}\n\
             SATURATION={}\nOLED_DIM_MIN={}\nMIDTONE_GAMMA={}\nFORCE_OLED={}\n\
             WHITE_TEMP={}\nWHITE_TINT={}\nBLACK_LIFT={}\nTONEMAP={}\nTONEMAP_KNEE={}\n",
            self.sdr_nits,
            self.peak_nits,
            self.gamut_pct,
            self.max_bpc,
            self.gamut_mode.as_str(),
            self.saturation,
            self.oled_dim_min,
            self.midtone_gamma,
            self.force_oled as i32,
            self.white_temp,
            self.white_tint,
            self.black_lift,
            self.tonemap.as_int(),
            self.tonemap_knee,
        )
    }

    /// Build the injector CLI args that reproduce this configuration.
    pub fn to_args(&self) -> Vec<String> {
        vec![
            "--sdr-nits".into(),
            self.sdr_nits.to_string(),
            "--peak-nits".into(),
            self.peak_nits.to_string(),
            "--gamut".into(),
            self.gamut_pct.to_string(),
            "--gamut-mode".into(),
            self.gamut_mode.as_str().into(),
            "--bpc".into(),
            self.max_bpc.to_string(),
            "--saturation".into(),
            self.saturation.to_string(),
            "--midtone-gamma".into(),
            self.midtone_gamma.to_string(),
            "--white-temp".into(),
            self.white_temp.to_string(),
            "--white-tint".into(),
            self.white_tint.to_string(),
            "--black-lift".into(),
            self.black_lift.to_string(),
            "--tonemap".into(),
            self.tonemap.as_arg().into(),
            "--tonemap-knee".into(),
            self.tonemap_knee.to_string(),
            "--force-oled".into(),
            (self.force_oled as i32).to_string(),
            "--oled-dim-min".into(),
            self.oled_dim_min.to_string(),
        ]
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn roundtrip_preserves_values() {
        let mut c = Conf::default();
        c.white_temp = 5500;
        c.black_lift = 12;
        c.tonemap = Tonemap::Rolloff;
        c.gamut_mode = GamutMode::DciP3;
        let s = c.to_conf_string();
        let back = {
            let mut d = Conf::default();
            d.merge_str(&s);
            d
        };
        assert_eq!(c, back);
    }

    #[test]
    fn defaults_match_injector() {
        let c = Conf::default();
        assert_eq!(c.white_temp, 6504);
        assert_eq!(c.tonemap, Tonemap::Clip);
        assert_eq!(c.tonemap_knee, 50);
    }
}
