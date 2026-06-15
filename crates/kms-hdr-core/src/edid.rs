//! Minimal EDID + CTA-861 extension parser.
//!
//! Pulls the bits the calibration tool actually needs: monitor name, HDR peak
//! luminance (from the HDR Static Metadata Data Block), wide-gamut support
//! (Colorimetry Data Block), and a best-effort OLED guess. Pure byte parsing —
//! no external EDID crate.

use std::fs;
use std::path::{Path, PathBuf};

#[derive(Clone, Debug, Default)]
pub struct DisplayInfo {
    /// DRM connector name, e.g. `HDMI-A-1`.
    pub connector: String,
    /// 3-letter PnP manufacturer ID from EDID bytes 8-9 (e.g. `GSM`, `SAM`).
    pub manufacturer: Option<String>,
    /// Monitor product name from descriptor block 0xFC, if present.
    pub name: Option<String>,
    /// Desired/peak content luminance in nits, decoded from HDR metadata.
    pub peak_nits: Option<u32>,
    /// Minimum luminance in nits (×1000 precision lost; rounded).
    pub min_nits: Option<f32>,
    /// BT.2020 RGB colorimetry advertised.
    pub bt2020: bool,
    /// DCI-P3 colorimetry advertised (CTA extended).
    pub dci_p3: bool,
    /// PQ (SMPTE ST 2084) EOTF supported per HDR metadata block.
    pub pq: bool,
    /// Best-effort OLED detection (name heuristic).
    pub is_oled: bool,
}

impl DisplayInfo {
    /// True if the panel is an LG display (PnP `GSM`/`LGD`/`LGE`, or an `LG`
    /// prefix in the product name). LG TVs report several of these.
    pub fn is_lg(&self) -> bool {
        let mfg_lg = matches!(
            self.manufacturer.as_deref(),
            Some("GSM") | Some("LGD") | Some("LGE")
        );
        let name_lg = self
            .name
            .as_deref()
            .map(|n| n.to_uppercase().starts_with("LG"))
            .unwrap_or(false);
        mfg_lg || name_lg
    }
}

/// Scan `/sys/class/drm` for a connected display exposing a usable EDID.
///
/// On multi-output systems (e.g. iGPU + dGPU) the first connected connector can
/// be a stub with an empty/garbage EDID, so prefer one that actually parsed a
/// product name or manufacturer; fall back to the first connected EDID otherwise.
pub fn detect() -> Option<DisplayInfo> {
    let entries = fs::read_dir("/sys/class/drm").ok()?;
    let mut found: Vec<PathBuf> = entries
        .flatten()
        .map(|e| e.path())
        .filter(|p| {
            p.file_name()
                .and_then(|n| n.to_str())
                .map(|n| n.contains('-')) // connector dirs look like cardN-HDMI-A-1
                .unwrap_or(false)
        })
        .collect();
    found.sort();

    let mut fallback: Option<DisplayInfo> = None;
    for conn in found {
        let status = fs::read_to_string(conn.join("status")).unwrap_or_default();
        if status.trim() != "connected" {
            continue;
        }
        // A read error on one connector must not abort the whole scan.
        let Ok(edid) = fs::read(conn.join("edid")) else {
            continue;
        };
        if edid.len() < 128 {
            continue;
        }
        let mut info = parse(&edid);
        info.connector = connector_name(&conn);
        // A connector that identified itself wins immediately.
        if info.name.is_some() || info.manufacturer.is_some() {
            return Some(info);
        }
        // Otherwise keep the first usable EDID as a last resort.
        fallback.get_or_insert(info);
    }
    fallback
}

/// Strip the leading `cardN-` from a connector sysfs dir name.
fn connector_name(path: &Path) -> String {
    let raw = path
        .file_name()
        .and_then(|n| n.to_str())
        .unwrap_or("")
        .to_string();
    match raw.split_once('-') {
        Some((_, rest)) => rest.to_string(),
        None => raw,
    }
}

/// Parse a full EDID blob (base block + optional CTA-861 extensions).
pub fn parse(edid: &[u8]) -> DisplayInfo {
    let mut info = DisplayInfo::default();

    // Manufacturer PnP ID: EDID bytes 8-9, three packed 5-bit letters (1=A).
    if edid.len() >= 10 {
        info.manufacturer = decode_pnp_id(edid[8], edid[9]);
    }

    // Descriptor blocks live at 0x36..0x6D, four 18-byte descriptors.
    for d in 0..4 {
        let base = 0x36 + d * 18;
        if base + 18 > edid.len() {
            break;
        }
        let desc = &edid[base..base + 18];
        // Monitor name descriptor: 00 00 00 FC 00 <13 bytes ASCII>
        if desc[0] == 0 && desc[1] == 0 && desc[2] == 0 && desc[3] == 0xFC {
            let name: String = desc[5..]
                .iter()
                .take_while(|&&b| b != 0x0A)
                .map(|&b| b as char)
                .collect();
            let name = name.trim().to_string();
            if !name.is_empty() {
                info.is_oled = name.to_lowercase().contains("oled");
                info.name = Some(name);
            }
        }
    }

    // Walk CTA-861 extension blocks.
    let ext_count = *edid.get(0x7E).unwrap_or(&0) as usize;
    for e in 0..ext_count {
        let start = 128 * (e + 1);
        if start + 128 > edid.len() {
            break;
        }
        let block = &edid[start..start + 128];
        if block[0] == 0x02 {
            parse_cta(block, &mut info);
        }
    }
    info
}

/// Decode the EDID manufacturer ID (bytes 8-9) into its 3-letter PnP code.
/// Returns `None` if the high bit is set or any letter is out of range.
fn decode_pnp_id(b0: u8, b1: u8) -> Option<String> {
    let raw = ((b0 as u16) << 8) | b1 as u16;
    if raw & 0x8000 != 0 {
        return None; // reserved high bit must be 0
    }
    let c1 = ((raw >> 10) & 0x1F) as u8;
    let c2 = ((raw >> 5) & 0x1F) as u8;
    let c3 = (raw & 0x1F) as u8;
    let letter = |v: u8| -> Option<char> {
        if (1..=26).contains(&v) {
            Some((b'A' + v - 1) as char)
        } else {
            None
        }
    };
    Some([letter(c1)?, letter(c2)?, letter(c3)?].iter().collect())
}

/// Parse a CTA-861 extension block's data-block collection.
fn parse_cta(block: &[u8], info: &mut DisplayInfo) {
    let dtd_start = block[2] as usize; // offset of detailed timing descriptors
    if dtd_start < 4 {
        return;
    }
    let mut i = 4usize;
    while i < dtd_start && i < block.len() {
        let tag = (block[i] >> 5) & 0x07;
        let len = (block[i] & 0x1F) as usize;
        let payload_start = i + 1;
        let payload_end = (payload_start + len).min(block.len());
        if payload_start > payload_end {
            break;
        }
        let payload = &block[payload_start..payload_end];

        // Tag 7 = "use extended tag" — the extended tag is payload[0].
        if tag == 0x07 && !payload.is_empty() {
            match payload[0] {
                0x05 => parse_colorimetry(payload, info), // Colorimetry Data Block
                0x06 => parse_hdr_metadata(payload, info), // HDR Static Metadata
                _ => {}
            }
        }
        i = payload_end;
    }
}

/// Colorimetry Data Block — advertises BT.2020 / DCI-P3 support.
fn parse_colorimetry(payload: &[u8], info: &mut DisplayInfo) {
    // payload[0] = ext tag 0x05; payload[1] = byte3 (colorimetry bits)
    if payload.len() < 2 {
        return;
    }
    let b = payload[1];
    // bit5 BT2020cYCC, bit6 BT2020YCC, bit7 BT2020RGB
    info.bt2020 = (b & 0b1110_0000) != 0;
    // DCI-P3 is signalled in some sinks via the additional colorimetry bits
    if payload.len() >= 3 {
        // byte4 bit7 = DCI-P3 (per CTA-861-G additional support flags)
        info.dci_p3 = (payload[2] & 0b1000_0000) != 0;
    }
}

/// HDR Static Metadata Data Block — EOTF support + luminance.
fn parse_hdr_metadata(payload: &[u8], info: &mut DisplayInfo) {
    // payload[0]=ext tag 0x06; payload[1]=EOTF support flags; payload[2]=metadata desc
    if payload.len() < 2 {
        return;
    }
    // EOTF bit2 = SMPTE ST 2084 (PQ)
    info.pq = (payload[1] & 0b0000_0100) != 0;

    // Optional luminance bytes follow (max, max-frame-avg, min).
    // Coding: L = 50 * 2^(CV/32). Min uses Lmax * (CVmin/255)^2 / 100.
    let decode_max = |cv: u8| -> u32 { (50.0 * 2f64.powf(cv as f64 / 32.0)).round() as u32 };
    if payload.len() >= 4 {
        let max_cv = payload[3];
        if max_cv > 0 {
            info.peak_nits = Some(decode_max(max_cv));
        }
    }
    if payload.len() >= 6 {
        let min_cv = payload[5];
        if let Some(peak) = info.peak_nits {
            let min = peak as f32 * (min_cv as f32 / 255.0).powi(2) / 100.0;
            info.min_nits = Some(min);
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn empty_is_graceful() {
        let info = parse(&[0u8; 128]);
        assert!(info.name.is_none());
        assert!(!info.bt2020);
    }

    #[test]
    fn pnp_id_decodes_lg() {
        // "GSM" = G(7) S(19) M(13) -> 0x1E6D
        assert_eq!(decode_pnp_id(0x1E, 0x6D).as_deref(), Some("GSM"));
        // reserved high bit set -> rejected
        assert_eq!(decode_pnp_id(0x80, 0x00), None);
    }

    #[test]
    fn is_lg_detects_manufacturer_and_name() {
        let mut info = DisplayInfo {
            manufacturer: Some("GSM".into()),
            ..Default::default()
        };
        assert!(info.is_lg());
        info.manufacturer = Some("SAM".into());
        info.name = Some("LG TV SSCR2".into());
        assert!(info.is_lg());
        info.name = Some("HISENSE".into());
        assert!(!info.is_lg());
    }

    #[test]
    fn name_descriptor_parsed() {
        let mut edid = [0u8; 128];
        // descriptor 0 at 0x36: monitor name "OLED55"
        let d = 0x36;
        edid[d + 3] = 0xFC;
        for (k, b) in b"OLED55\n".iter().enumerate() {
            edid[d + 5 + k] = *b;
        }
        let info = parse(&edid);
        assert_eq!(info.name.as_deref(), Some("OLED55"));
        assert!(info.is_oled);
    }
}
