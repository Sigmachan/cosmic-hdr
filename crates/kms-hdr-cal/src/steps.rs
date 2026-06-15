//! Wizard step definitions, their test patterns, and per-step controls.

use crate::patterns::Pattern;
use crate::state::Shared;
use gtk4::prelude::*;
use gtk4::{Align, Box as GtkBox, DropDown, Label, Orientation, Scale, StringList, Switch};
use kms_hdr_core::conf::{GamutMode, Tonemap};
use kms_hdr_core::preset;
use std::rc::Rc;

#[derive(Clone, Copy, PartialEq, Eq)]
pub enum Step {
    Intro,
    Panel,
    BlackLevel,
    Peak,
    WhiteRef,
    Gamut,
    WhiteBalance,
    Gamma,
    Tonemap,
    Summary,
}

pub const STEPS: &[Step] = &[
    Step::Intro,
    Step::Panel,
    Step::BlackLevel,
    Step::Peak,
    Step::WhiteRef,
    Step::Gamut,
    Step::WhiteBalance,
    Step::Gamma,
    Step::Tonemap,
    Step::Summary,
];

impl Step {
    pub fn title(self) -> &'static str {
        match self {
            Step::Intro => "Welcome",
            Step::Panel => "Panel preset",
            Step::BlackLevel => "Black level",
            Step::Peak => "Peak luminance",
            Step::WhiteRef => "SDR white reference",
            Step::Gamut => "Colour gamut & vividness",
            Step::WhiteBalance => "White balance",
            Step::Gamma => "Midtone gamma",
            Step::Tonemap => "Highlight roll-off",
            Step::Summary => "Summary",
        }
    }

    pub fn desc(self) -> &'static str {
        match self {
            Step::Intro => "Calibrate HDR & wide-gamut output for your display. Changes preview live.",
            Step::Panel => "Start from a profile tuned for your panel, then fine-tune the steps that follow.",
            Step::BlackLevel => "Lift the shadow floor just enough to recover crushed near-blacks (OLED).",
            Step::Peak => "Set peak nits to your panel's true brightness using the clipping test.",
            Step::WhiteRef => "Set how bright SDR white (desktop/UI) appears in HDR mode.",
            Step::Gamut => "Choose the output gamut and dial in saturation without over-cooking skin.",
            Step::WhiteBalance => "Neutralise any colour cast with temperature and tint.",
            Step::Gamma => "Shape the midtone curve — contrast vs shadow detail.",
            Step::Tonemap => "Soft-clip the brightest highlights instead of hard-clipping to flat white.",
            Step::Summary => "Review and save. Apply writes the config; optionally store a named profile.",
        }
    }

    pub fn pattern(self) -> Pattern {
        match self {
            Step::Intro => Pattern::Info,
            Step::Panel => Pattern::Info,
            Step::BlackLevel => Pattern::BlackLevel,
            Step::Peak => Pattern::PeakClip,
            Step::WhiteRef => Pattern::WhiteRef,
            Step::Gamut => Pattern::Gamut,
            Step::WhiteBalance => Pattern::WhiteBalance,
            Step::Gamma => Pattern::Gamma,
            Step::Tonemap => Pattern::Tonemap,
            Step::Summary => Pattern::Summary,
        }
    }
}

/// A horizontal labelled slider that writes back via `on_change`.
fn scale_row(
    label: &str,
    min: f64,
    max: f64,
    step: f64,
    val: f64,
    on_change: impl Fn(f64) + 'static,
) -> GtkBox {
    let row = GtkBox::new(Orientation::Vertical, 2);
    let l = Label::new(Some(label));
    l.set_xalign(0.0);
    let s = Scale::with_range(Orientation::Horizontal, min, max, step);
    s.set_value(val);
    s.set_hexpand(true);
    s.set_draw_value(true);
    s.set_value_pos(gtk4::PositionType::Right);
    s.connect_value_changed(move |sc| on_change(sc.value()));
    row.append(&l);
    row.append(&s);
    row
}

/// An info paragraph built from the current display/GPU state.
fn info_label(shared: &Shared) -> Label {
    let s = shared.borrow();
    let mut lines = Vec::new();
    if let Some(d) = &s.display {
        lines.push(format!(
            "Display: {} ({})",
            d.name.clone().unwrap_or_else(|| "unknown".into()),
            d.connector
        ));
        if let Some(p) = d.peak_nits {
            lines.push(format!("EDID peak: {p} nits"));
        }
        let mut gamut = Vec::new();
        if d.bt2020 {
            gamut.push("BT.2020");
        }
        if d.dci_p3 {
            gamut.push("DCI-P3");
        }
        if d.pq {
            gamut.push("PQ/ST2084");
        }
        if !gamut.is_empty() {
            lines.push(format!("Supports: {}", gamut.join(", ")));
        }
        if d.is_oled {
            lines.push("Panel: OLED (black-level/auto-dim care enabled)".into());
        }
    } else {
        lines.push("No EDID detected — using safe defaults.".into());
    }
    let pipe = if s.gpu.has_full_pipeline() {
        "DEGAMMA+CTM+GAMMA atomic pipeline"
    } else {
        "legacy per-channel gamma (NVIDIA fallback)"
    };
    lines.push(format!("GPU: {} — {}", s.gpu.label(), pipe));

    let l = Label::new(Some(&lines.join("\n")));
    l.set_xalign(0.0);
    l.set_wrap(true);
    l
}

/// Build the control panel for `step`. `notify` triggers a live re-apply + redraw.
pub fn build_controls(step: Step, shared: &Shared, notify: Rc<dyn Fn()>) -> GtkBox {
    let panel = GtkBox::new(Orientation::Vertical, 12);
    panel.set_margin_top(8);
    panel.set_margin_bottom(8);

    match step {
        Step::Intro => {
            panel.append(&info_label(shared));
        }
        Step::Panel => {
            // Recommend a preset from the detected display, if any.
            let recommended = shared
                .borrow()
                .display
                .as_ref()
                .and_then(preset::recommend);
            let rec_line = match recommended {
                Some(p) => format!("Recommended for your panel: {}", p.label),
                None => "No confident match — pick the closest panel below.".to_string(),
            };
            let rl = Label::new(Some(&rec_line));
            rl.set_xalign(0.0);
            rl.set_wrap(true);
            panel.append(&rl);

            let labels: Vec<&str> = preset::PRESETS.iter().map(|p| p.label).collect();
            let list = StringList::new(&labels);
            let dd = DropDown::new(Some(list), gtk4::Expression::NONE);
            if let Some(rp) = recommended {
                if let Some(idx) = preset::PRESETS.iter().position(|p| p.id == rp.id) {
                    dd.set_selected(idx as u32);
                }
            }
            // Per-preset rationale, kept in sync with the selection.
            let desc = Label::new(None);
            desc.set_xalign(0.0);
            desc.set_wrap(true);
            desc.add_css_class("dim-label");
            let sync_desc = {
                let desc = desc.clone();
                move |i: usize| {
                    if let Some(p) = preset::PRESETS.get(i) {
                        desc.set_text(p.desc);
                    }
                }
            };
            sync_desc(dd.selected() as usize);

            let row = GtkBox::new(Orientation::Horizontal, 8);
            let l = Label::new(Some("Panel"));
            l.set_xalign(0.0);
            l.set_hexpand(true);
            l.set_halign(Align::Start);
            row.append(&l);
            row.append(&dd);
            panel.append(&row);
            panel.append(&desc);

            {
                let sh = shared.clone();
                let n = notify.clone();
                dd.connect_selected_notify(move |d| {
                    let i = d.selected() as usize;
                    if let Some(p) = preset::PRESETS.get(i) {
                        // Load the tuned config, but keep the EDID-reported peak
                        // if the display advertised a higher real value. Copy the
                        // peak out first so the immutable borrow is released
                        // before the mutable one (no overlapping RefCell borrow).
                        let edid_peak = sh.borrow().display.as_ref().and_then(|x| x.peak_nits);
                        let mut c = p.conf();
                        if let Some(ep) = edid_peak {
                            if (ep as i32) > c.peak_nits {
                                c.peak_nits = ep as i32;
                            }
                        }
                        sh.borrow_mut().conf = c;
                    }
                    sync_desc(i);
                    n();
                });
            }
        }
        Step::BlackLevel => {
            let sh = shared.clone();
            let n = notify.clone();
            let v = shared.borrow().conf.black_lift as f64;
            panel.append(&scale_row("Black-level lift", 0.0, 100.0, 1.0, v, move |x| {
                sh.borrow_mut().conf.black_lift = x as i32;
                n();
            }));
        }
        Step::Peak => {
            let sh = shared.clone();
            let n = notify.clone();
            let v = shared.borrow().conf.peak_nits as f64;
            panel.append(&scale_row("Peak luminance (nits)", 200.0, 4000.0, 10.0, v, move |x| {
                sh.borrow_mut().conf.peak_nits = x as i32;
                n();
            }));
        }
        Step::WhiteRef => {
            let sh = shared.clone();
            let n = notify.clone();
            let v = shared.borrow().conf.sdr_nits as f64;
            panel.append(&scale_row("SDR white (nits)", 80.0, 400.0, 1.0, v, move |x| {
                sh.borrow_mut().conf.sdr_nits = x as i32;
                n();
            }));
        }
        Step::Gamut => {
            // gamut mode dropdown
            let modes = StringList::new(&[
                GamutMode::Bt2020.label(),
                GamutMode::DciP3.label(),
                GamutMode::Srgb.label(),
            ]);
            let dd = DropDown::new(Some(modes), gtk4::Expression::NONE);
            dd.set_selected(match shared.borrow().conf.gamut_mode {
                GamutMode::Bt2020 => 0,
                GamutMode::DciP3 => 1,
                GamutMode::Srgb => 2,
            });
            let row = GtkBox::new(Orientation::Horizontal, 8);
            let l = Label::new(Some("Output gamut"));
            l.set_xalign(0.0);
            l.set_hexpand(true);
            l.set_halign(Align::Start);
            row.append(&l);
            row.append(&dd);
            {
                let sh = shared.clone();
                let n = notify.clone();
                dd.connect_selected_notify(move |d| {
                    sh.borrow_mut().conf.gamut_mode = match d.selected() {
                        1 => GamutMode::DciP3,
                        2 => GamutMode::Srgb,
                        _ => GamutMode::Bt2020,
                    };
                    n();
                });
            }
            panel.append(&row);

            let sh = shared.clone();
            let n = notify.clone();
            let v = shared.borrow().conf.gamut_pct as f64;
            panel.append(&scale_row("Gamut expansion %", 0.0, 100.0, 1.0, v, move |x| {
                sh.borrow_mut().conf.gamut_pct = x as i32;
                n();
            }));

            let sh = shared.clone();
            let n = notify.clone();
            let v = shared.borrow().conf.saturation as f64;
            panel.append(&scale_row("Saturation %", 50.0, 200.0, 1.0, v, move |x| {
                sh.borrow_mut().conf.saturation = x as i32;
                n();
            }));
        }
        Step::WhiteBalance => {
            let sh = shared.clone();
            let n = notify.clone();
            let v = shared.borrow().conf.white_temp as f64;
            panel.append(&scale_row("Temperature (K)", 4000.0, 10000.0, 50.0, v, move |x| {
                sh.borrow_mut().conf.white_temp = x as i32;
                n();
            }));

            let sh = shared.clone();
            let n = notify.clone();
            let v = shared.borrow().conf.white_tint as f64;
            panel.append(&scale_row("Tint (green ↔ magenta)", -50.0, 50.0, 1.0, v, move |x| {
                sh.borrow_mut().conf.white_tint = x as i32;
                n();
            }));
        }
        Step::Gamma => {
            let sh = shared.clone();
            let n = notify.clone();
            let v = shared.borrow().conf.midtone_gamma as f64;
            panel.append(&scale_row("Midtone gamma %", 30.0, 250.0, 1.0, v, move |x| {
                sh.borrow_mut().conf.midtone_gamma = x as i32;
                n();
            }));
        }
        Step::Tonemap => {
            let row = GtkBox::new(Orientation::Horizontal, 8);
            let l = Label::new(Some("Soft highlight roll-off"));
            l.set_xalign(0.0);
            l.set_hexpand(true);
            l.set_halign(Align::Start);
            let sw = Switch::new();
            sw.set_active(shared.borrow().conf.tonemap == Tonemap::Rolloff);
            sw.set_halign(Align::End);
            row.append(&l);
            row.append(&sw);
            {
                let sh = shared.clone();
                let n = notify.clone();
                sw.connect_active_notify(move |s| {
                    sh.borrow_mut().conf.tonemap = if s.is_active() {
                        Tonemap::Rolloff
                    } else {
                        Tonemap::Clip
                    };
                    n();
                });
            }
            panel.append(&row);

            let sh = shared.clone();
            let n = notify.clone();
            let v = shared.borrow().conf.tonemap_knee as f64;
            panel.append(&scale_row("Roll-off knee %", 0.0, 100.0, 1.0, v, move |x| {
                sh.borrow_mut().conf.tonemap_knee = x as i32;
                n();
            }));
        }
        Step::Summary => {
            panel.append(&info_label(shared));
            let c = shared.borrow().conf.clone();
            let recap = format!(
                "SDR {} / peak {} nits · gamut {} {}% · sat {}% · gamma {}%\n\
                 white {}K tint {} · black-lift {} · {} (knee {}%)",
                c.sdr_nits,
                c.peak_nits,
                c.gamut_mode.as_str(),
                c.gamut_pct,
                c.saturation,
                c.midtone_gamma,
                c.white_temp,
                c.white_tint,
                c.black_lift,
                c.tonemap.as_arg(),
                c.tonemap_knee,
            );
            let l = Label::new(Some(&recap));
            l.set_xalign(0.0);
            l.set_wrap(true);
            l.add_css_class("dim-label");
            panel.append(&l);
        }
    }
    panel
}
