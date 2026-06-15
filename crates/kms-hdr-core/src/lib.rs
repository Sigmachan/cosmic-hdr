//! Shared logic for the kms-hdr stack.
//!
//! Single source of truth consumed by the calibration tool, the standalone
//! settings panel, and the cosmic-settings integration. Owns the persisted
//! configuration model ([`conf`]), display introspection ([`edid`]), GPU
//! detection ([`gpu`]), injector invocation ([`apply`]), and named calibration
//! profiles ([`profile`]).

pub mod apply;
pub mod conf;
pub mod edid;
pub mod gpu;
pub mod preset;
pub mod profile;

pub use conf::{Conf, GamutMode, Tonemap, CONF_PATH};
pub use edid::DisplayInfo;
pub use gpu::GpuVendor;
pub use preset::{Preset, PRESETS};

/// Color-temperature presets surfaced by calibration UIs (Kelvin).
pub const TEMP_PRESETS: &[(&str, i32)] = &[
    ("Warm (5000K)", 5000),
    ("D65 (6504K)", 6504),
    ("Cool (7500K)", 7500),
    ("Daylight (9300K)", 9300),
];
