#!/usr/bin/env python3
"""
hdr-cal — HDR calibration wizard & test-pattern overlay for kms-hdr.

Usage:
  hdr-cal.py <pattern>        — fullscreen static colour field
  hdr-cal.py --calibrate ...  — Windows-style multi-step HDR calibration wizard

Patterns: black darkgray gray50 white red green blue sdr_hdr
"""

import sys, math, subprocess, traceback
import cairo
import gi
gi.require_version('Gtk', '4.0')
from gi.repository import Gtk, Gdk, GLib

BIN = '/usr/local/bin/kms-hdr'
GAMUT_MODES = {'bt2020', 'dci-p3', 'srgb'}

# ── Colour helpers ─────────────────────────────────────────────────────────────

STATIC_COLORS = {
    'black':    (0.000, 0.000, 0.000),
    'darkgray': (0.046, 0.046, 0.046),
    'gray50':   (0.500, 0.500, 0.500),
    'white':    (1.000, 1.000, 1.000),
    'red':      (1.000, 0.000, 0.000),
    'green':    (0.000, 1.000, 0.000),
    'blue':     (0.000, 0.000, 1.000),
}

def pq_percent(nits):
    m1, m2 = 0.1593017578125, 78.84375
    c1, c2, c3 = 0.8359375, 18.8515625, 18.6875
    y = min(max(nits / 10000.0, 0.0), 1.0)
    ym = y ** m1
    return ((c1 + c2 * ym) / (1.0 + c3 * ym)) ** m2 * 100.0

# ── CSS ───────────────────────────────────────────────────────────────────────

CSS = """
* { font-family: "Inter", "Cantarell", sans-serif; }
.wiz-root { background: #000; }
.wiz-footer {
    background: rgba(12,12,12,0.92);
    border-top: 1px solid rgba(255,255,255,0.08);
    padding: 18px 32px;
}
.wiz-title {
    color: #fff;
    font-size: 22px;
    font-weight: 700;
    margin-bottom: 2px;
}
.wiz-desc {
    color: rgba(255,255,255,0.65);
    font-size: 14px;
    margin-bottom: 12px;
}
.wiz-value {
    color: #fff;
    font-size: 15px;
    font-weight: 600;
    min-width: 180px;
}
.wiz-step-dot {
    min-width: 8px;
    min-height: 8px;
    border-radius: 4px;
    background: rgba(255,255,255,0.2);
    margin: 0 4px;
}
.wiz-step-dot.active {
    background: #fff;
    min-width: 20px;
}
.wiz-step-dot.done {
    background: rgba(255,255,255,0.55);
}
scale { color: #fff; }
scale trough { background: rgba(255,255,255,0.15); min-height: 4px; border-radius: 2px; }
scale trough highlight { background: #fff; border-radius: 2px; }
scale slider {
    background: #fff;
    border-radius: 50%;
    min-width: 18px;
    min-height: 18px;
    border: none;
    box-shadow: 0 1px 4px rgba(0,0,0,0.5);
}
button {
    background: rgba(255,255,255,0.1);
    color: #fff;
    border: 1px solid rgba(255,255,255,0.15);
    border-radius: 6px;
    padding: 8px 20px;
    font-size: 14px;
}
button:hover { background: rgba(255,255,255,0.18); }
.btn-primary {
    background: #fff;
    color: #000;
    font-weight: 700;
    border: none;
}
.btn-primary:hover { background: rgba(255,255,255,0.88); }
.btn-danger { background: rgba(220,50,50,0.18); border-color: rgba(220,50,50,0.4); color: #ff7070; }
.hint { color: rgba(255,255,255,0.3); font-size: 12px; }
"""

# ── Step definitions ───────────────────────────────────────────────────────────

STEPS = [
    'intro',
    'sdr_white',
    'peak_nits',
    'gamut',
    'midtone',
    'done',
]

STEP_TITLES = {
    'intro':     'HDR Calibration',
    'sdr_white': 'Step 1 — SDR White Brightness',
    'peak_nits': 'Step 2 — Peak Luminance',
    'gamut':     'Step 3 — Colour Gamut',
    'midtone':   'Step 4 — Midtone Contrast',
    'done':      'Calibration Complete',
}

STEP_DESC = {
    'intro': (
        'This wizard calibrates your HDR display in four steps.\n'
        'Each step shows a reference pattern — adjust the slider until the image looks correct.'
    ),
    'sdr_white': (
        'Adjust until the center white box looks like natural paper white under bright light.\n'
        'It should not look blown-out or washed out against the surrounding mid-gray.'
    ),
    'peak_nits': (
        'A dim reference box (SDR white) and a bright specular highlight are shown.\n'
        'Adjust until the highlight is clearly brighter than the reference but not blinding.'
    ),
    'gamut': (
        'Colour saturation is expanded from sRGB into a wider colour space.\n'
        'Adjust until reds, greens and blues look vivid but not artificial.'
    ),
    'midtone': (
        'Midtone gamma controls HDR contrast punch.\n'
        '100% = neutral · increase for more dramatic HDR · decrease if midtones look crushed.'
    ),
    'done': 'Settings have been saved and applied.',
}


# ── Drawing functions (each step's fullscreen pattern) ─────────────────────────

def draw_sdr_white(cr, w, h, sdr_nits):
    # Near-black background (dark HDR scene)
    cr.set_source_rgb(0.01, 0.01, 0.01)
    cr.paint()

    # Outer reference band: 18% gray (mid-gray reference)
    gray = 0.18
    bw, bh = w * 0.72, h * 0.72
    cr.set_source_rgb(gray, gray, gray)
    cr.rectangle((w - bw) / 2, (h - bh) / 2, bw, bh)
    cr.fill()

    # Inner "paper white" box
    iw, ih = w * 0.28, h * 0.36
    cr.set_source_rgb(1.0, 1.0, 1.0)
    cr.rectangle((w - iw) / 2, (h - ih) / 2, iw, ih)
    cr.fill()

    # Label inside white box
    cr.set_source_rgba(0, 0, 0, 0.5)
    cr.select_font_face('sans-serif', 0, 0)
    cr.set_font_size(min(w, h) * 0.018)
    txt = f'Paper white  ·  {sdr_nits} nits  ({pq_percent(sdr_nits):.1f}% PQ)'
    te = cr.text_extents(txt)
    cr.move_to((w - te[2]) / 2, h / 2 + te[3] / 2)
    cr.show_text(txt)

    # Reference nits label below outer box
    cr.set_source_rgba(1, 1, 1, 0.45)
    cr.set_font_size(min(w, h) * 0.014)
    sub = 'Adjust until center looks like a naturally-lit white surface'
    te2 = cr.text_extents(sub)
    cr.move_to((w - te2[2]) / 2, (h + bh) / 2 + 24)
    cr.show_text(sub)


def draw_peak_nits(cr, w, h, sdr_nits, peak_nits):
    # Black background
    cr.set_source_rgb(0.0, 0.0, 0.0)
    cr.paint()

    # SDR reference box (left)
    ref_w = w * 0.22
    ref_h = h * 0.28
    ref_x = w * 0.22
    ref_y = (h - ref_h) / 2
    cr.set_source_rgb(1.0, 1.0, 1.0)
    cr.rectangle(ref_x, ref_y, ref_w, ref_h)
    cr.fill()

    # Label on SDR box
    cr.set_source_rgba(0, 0, 0, 0.6)
    cr.select_font_face('sans-serif', 0, 0)
    fs = min(w, h) * 0.015
    cr.set_font_size(fs)
    cr.move_to(ref_x + 10, ref_y + ref_h / 2 - fs * 0.6)
    cr.show_text('SDR white')
    cr.move_to(ref_x + 10, ref_y + ref_h / 2 + fs * 0.7)
    cr.show_text(f'{sdr_nits} nits')

    # Specular highlight box (right) — slightly larger to show it's "brighter"
    hl_w = w * 0.18
    hl_h = h * 0.22
    hl_x = w * 0.58
    hl_y = (h - hl_h) / 2
    # Draw glow
    for r in range(6, 0, -1):
        alpha = 0.06 * (7 - r)
        cr.set_source_rgba(1.0, 0.97, 0.90, alpha)
        cr.rectangle(hl_x - r * 4, hl_y - r * 4, hl_w + r * 8, hl_h + r * 8)
        cr.fill()
    # Bright specular highlight (over-white = HDR specular)
    cr.set_source_rgb(1.0, 1.0, 1.0)
    cr.rectangle(hl_x, hl_y, hl_w, hl_h)
    cr.fill()

    # Label below specular
    cr.set_source_rgba(1, 1, 1, 0.55)
    cr.set_font_size(min(w, h) * 0.015)
    hl_lbl = f'HDR peak  ·  {peak_nits} nits  ({pq_percent(peak_nits):.1f}% PQ)'
    te = cr.text_extents(hl_lbl)
    cr.move_to(hl_x + (hl_w - te[2]) / 2, hl_y + hl_h + 28)
    cr.show_text(hl_lbl)

    # Instruction text
    cr.set_source_rgba(1, 1, 1, 0.35)
    cr.set_font_size(min(w, h) * 0.013)
    ins = 'Right box should appear clearly brighter than left — like a lamp vs paper'
    te2 = cr.text_extents(ins)
    cr.move_to((w - te2[2]) / 2, h * 0.82)
    cr.show_text(ins)


def draw_gamut(cr, w, h, gamut_pct):
    cr.set_source_rgb(0.04, 0.04, 0.04)
    cr.paint()

    # Color swatches: 6 saturated colours arranged in 2 rows
    # Values are approximate P3/BT.2020 primaries blended by gamut %
    t = gamut_pct / 100.0
    swatches = [
        # sRGB base -> BT.2020 expanded
        ((1.0, 0.0+t*0.2, 0.0),           'Red'),
        ((0.0+t*0.05, 0.9-t*0.1, 0.0),    'Green'),
        ((0.0, 0.3*t, 1.0),                'Blue'),
        ((1.0, 0.8-t*0.15, 0.0+t*0.05),   'Orange'),
        ((0.5-t*0.1, 0.0, 1.0),            'Violet'),
        ((0.0+t*0.05, 0.85, 0.8-t*0.1),   'Cyan'),
    ]

    cols, rows = 3, 2
    pad = 48
    sw = (w - pad * (cols + 1)) / cols
    sh = (h * 0.68 - pad * (rows + 1)) / rows
    for i, ((r, g, b), name) in enumerate(swatches):
        col = i % cols
        row = i // cols
        x = pad + col * (sw + pad)
        y = h * 0.1 + pad + row * (sh + pad)
        cr.set_source_rgb(min(r, 1), min(g, 1), min(b, 1))
        _rounded_rect(cr, x, y, sw, sh, 12)
        cr.fill()
        cr.set_source_rgba(0, 0, 0, 0.5)
        cr.select_font_face('sans-serif', 0, 0)
        cr.set_font_size(sh * 0.13)
        te = cr.text_extents(name)
        cr.move_to(x + (sw - te[2]) / 2, y + sh * 0.58)
        cr.show_text(name)

    # Gamut label
    cr.set_source_rgba(1, 1, 1, 0.5)
    cr.set_font_size(min(w, h) * 0.015)
    lbl = f'Expansion: {gamut_pct}%  →  target gamut'
    te = cr.text_extents(lbl)
    cr.move_to((w - te[2]) / 2, h * 0.86)
    cr.show_text(lbl)


def draw_midtone(cr, w, h, midtone_gamma):
    # Horizontal gradient background
    pat = _linear_grad(cr, 0, 0, w, 0)
    pat.add_color_stop_rgb(0, 0, 0, 0)
    pat.add_color_stop_rgb(1, 1, 1, 1)
    cr.set_source(pat)
    cr.paint()

    # Gamma curve indicator at 50% gray position
    mid_x = w / 2
    # Apply gamma curve visualisation: for neutral (100%), the midpoint is at 50%.
    # For gamma > 100% (more punch), midtones get darker (curve bows down).
    gamma = max(midtone_gamma, 1) / 100.0
    adj_x = mid_x + (mid_x - mid_x * (0.5 ** (1.0 / gamma))) * 0.4

    # Midtone reference line
    cr.set_source_rgba(1, 0.3, 0.3, 0.9)
    cr.set_line_width(2)
    cr.move_to(adj_x, 0)
    cr.line_to(adj_x, h)
    cr.stroke()

    # Neutral line (50%)
    cr.set_source_rgba(1, 1, 1, 0.3)
    cr.set_line_width(1)
    cr.move_to(mid_x, 0)
    cr.line_to(mid_x, h)
    cr.stroke()

    # Label
    cr.set_source_rgba(0, 0, 0, 0.6)
    cr.select_font_face('sans-serif', 0, 0)
    cr.set_font_size(min(w, h) * 0.02)
    lbl = f'Midtone gamma: {midtone_gamma}%'
    te = cr.text_extents(lbl)
    cr.move_to((w - te[2]) / 2, h * 0.55)
    cr.show_text(lbl)
    cr.set_source_rgba(1, 1, 1, 0.5)
    cr.set_font_size(min(w, h) * 0.013)
    sub = '100% = neutral  ·  >100% = more punch  ·  <100% = lifted shadows'
    te2 = cr.text_extents(sub)
    cr.move_to((w - te2[2]) / 2, h * 0.62)
    cr.show_text(sub)


def _rounded_rect(cr, x, y, w, h, r):
    cr.new_sub_path()
    cr.arc(x + w - r, y + r, r, -math.pi/2, 0)
    cr.arc(x + w - r, y + h - r, r, 0, math.pi/2)
    cr.arc(x + r, y + h - r, r, math.pi/2, math.pi)
    cr.arc(x + r, y + r, r, math.pi, 3*math.pi/2)
    cr.close_path()


def _linear_grad(cr, x0, y0, x1, y1):
    return cairo.LinearGradient(x0, y0, x1, y1)


# ── Static pattern window ──────────────────────────────────────────────────────

class StaticPatternWindow(Gtk.ApplicationWindow):
    def __init__(self, app, pattern):
        super().__init__(application=app)
        self.set_fullscreened(True)

        if pattern == 'sdr_hdr':
            box = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL)
            for rgb in [(0.20, 0.20, 0.20), (1.0, 1.0, 1.0)]:
                da = Gtk.DrawingArea()
                r, g, b = rgb
                da.set_draw_func(lambda w, cr, _w, _h, r=r, g=g, b=b:
                    (cr.set_source_rgb(r, g, b), cr.paint()))
                da.set_hexpand(True); da.set_vexpand(True)
                box.append(da)
            self.set_child(box)
        else:
            rgb = STATIC_COLORS.get(pattern, (1.0, 1.0, 1.0))
            r, g, b = rgb
            da = Gtk.DrawingArea()
            da.set_draw_func(lambda w, cr, _w, _h, r=r, g=g, b=b:
                (cr.set_source_rgb(r, g, b), cr.paint()))
            da.set_hexpand(True); da.set_vexpand(True)
            self.set_child(da)

        overlay = Gtk.Overlay()
        overlay.set_child(self.get_child())
        hint = Gtk.Label(label='Press Esc or click to close')
        hint.set_css_classes(['hint'])
        hint.set_halign(Gtk.Align.CENTER)
        hint.set_valign(Gtk.Align.END)
        hint.set_margin_bottom(20)
        overlay.add_overlay(hint)
        self.set_child(overlay)

        key = Gtk.EventControllerKey()
        key.connect('key-pressed', lambda c, k, *a: app.quit() if k == Gdk.KEY_Escape else False)
        self.add_controller(key)
        click = Gtk.GestureClick()
        click.connect('pressed', lambda *a: app.quit())
        self.add_controller(click)

        _apply_css()


# ── Wizard window ──────────────────────────────────────────────────────────────

class WizardWindow(Gtk.ApplicationWindow):
    def __init__(self, app, sdr_nits, peak_nits, gamut, bpc, gamut_mode, midtone_gamma):
        super().__init__(application=app)
        self.set_fullscreened(True)
        self._app         = app
        self._sdr_nits    = sdr_nits
        self._peak_nits   = peak_nits
        self._gamut       = gamut
        self._bpc         = bpc
        self._gamut_mode  = gamut_mode
        self._midtone     = midtone_gamma
        self._step_idx    = 0
        self._pending     = False
        self._debounce_id = 0
        self._live_proc   = None

        self.set_css_classes(['wiz-root'])
        self.connect('close-request', self._on_close)

        key = Gtk.EventControllerKey()
        key.connect('key-pressed', self._on_key)
        self.add_controller(key)

        # Drawing area (fullscreen pattern behind footer)
        self._da = Gtk.DrawingArea()
        self._da.set_draw_func(self._draw_step)
        self._da.set_hexpand(True)
        self._da.set_vexpand(True)

        # ── Footer UI ──────────────────────────────────────────────────────────
        self._title_lbl = Gtk.Label()
        self._title_lbl.set_css_classes(['wiz-title'])
        self._title_lbl.set_halign(Gtk.Align.START)

        self._desc_lbl = Gtk.Label()
        self._desc_lbl.set_css_classes(['wiz-desc'])
        self._desc_lbl.set_halign(Gtk.Align.START)
        self._desc_lbl.set_wrap(True)
        self._desc_lbl.set_max_width_chars(90)

        # Step dots
        self._dots_box = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL)
        self._dots_box.set_halign(Gtk.Align.CENTER)
        self._dots_box.set_margin_bottom(4)

        # Slider row
        self._slider = Gtk.Scale.new_with_range(Gtk.Orientation.HORIZONTAL, 0, 100, 1)
        self._slider.set_hexpand(True)
        self._slider.set_draw_value(False)
        self._slider.connect('value-changed', self._on_slider)

        self._value_lbl = Gtk.Label()
        self._value_lbl.set_css_classes(['wiz-value'])
        self._value_lbl.set_halign(Gtk.Align.END)

        slider_row = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL, spacing=16)
        slider_row.set_margin_top(4)
        slider_row.append(self._slider)
        slider_row.append(self._value_lbl)

        self._slider_box = Gtk.Box(orientation=Gtk.Orientation.VERTICAL)
        self._slider_box.append(slider_row)

        # Buttons
        self._btn_back = Gtk.Button(label='← Back')
        self._btn_back.connect('clicked', self._go_back)

        self._btn_next = Gtk.Button(label='Next →')
        self._btn_next.set_css_classes(['btn-primary'])
        self._btn_next.connect('clicked', self._go_next)

        self._btn_cancel = Gtk.Button(label='Cancel')
        self._btn_cancel.set_css_classes(['btn-danger'])
        self._btn_cancel.connect('clicked', lambda *a: app.quit())

        btn_row = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL, spacing=10)
        btn_row.set_halign(Gtk.Align.END)
        btn_row.append(self._btn_cancel)
        btn_row.append(self._btn_back)
        btn_row.append(self._btn_next)

        # Footer layout
        left_col = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=2)
        left_col.set_hexpand(True)
        left_col.append(self._dots_box)
        left_col.append(self._title_lbl)
        left_col.append(self._desc_lbl)
        left_col.append(self._slider_box)

        footer_row = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL, spacing=32)
        footer_row.append(left_col)
        footer_row.append(btn_row)
        footer_row.set_valign(Gtk.Align.CENTER)

        footer = Gtk.Box(orientation=Gtk.Orientation.VERTICAL)
        footer.set_css_classes(['wiz-footer'])
        footer.append(footer_row)

        # Root layout
        root = Gtk.Box(orientation=Gtk.Orientation.VERTICAL)
        root.append(self._da)
        root.append(footer)
        self.set_child(root)

        _apply_css()
        self._refresh_step()

    # ── Step navigation ────────────────────────────────────────────────────────

    def _go_next(self, *_):
        step = STEPS[self._step_idx]
        if step == 'done':
            self._do_apply(save=True)
            self._app.quit()
            return
        if self._step_idx < len(STEPS) - 1:
            self._step_idx += 1
        self._refresh_step()

    def _go_back(self, *_):
        if self._step_idx > 0:
            self._step_idx -= 1
            self._refresh_step()

    def _on_key(self, ctrl, keyval, *args):
        if keyval == Gdk.KEY_Escape:
            self._app.quit()
        elif keyval in (Gdk.KEY_Return, Gdk.KEY_KP_Enter):
            self._go_next()
        return False

    def _refresh_step(self):
        step = STEPS[self._step_idx]

        # Update dots
        for child in list(self._dots_box):
            self._dots_box.remove(child)
        for i, s in enumerate(STEPS):
            if s in ('intro', 'done'): continue
            dot = Gtk.Box()
            classes = ['wiz-step-dot']
            if i == self._step_idx: classes.append('active')
            elif i < self._step_idx: classes.append('done')
            dot.set_css_classes(classes)
            self._dots_box.append(dot)

        self._title_lbl.set_label(STEP_TITLES[step])
        self._desc_lbl.set_label(STEP_DESC[step])

        # Configure slider per step
        visible = True
        if step == 'sdr_white':
            self._slider.set_range(80, 400)
            self._slider.set_value(self._sdr_nits)
            self._update_value_label()
        elif step == 'peak_nits':
            self._slider.set_range(400, 1600)
            self._slider.set_value(self._peak_nits)
            self._update_value_label()
        elif step == 'gamut':
            self._slider.set_range(0, 100)
            self._slider.set_value(self._gamut)
            self._update_value_label()
        elif step == 'midtone':
            self._slider.set_range(30, 250)
            self._slider.set_value(self._midtone)
            self._update_value_label()
        else:
            visible = False

        self._slider_box.set_visible(visible)
        self._value_lbl.set_visible(visible)

        # Button labels
        is_first = self._step_idx == 0
        is_last  = step == 'done'
        self._btn_back.set_visible(not is_first)
        self._btn_next.set_label('Apply & Close' if is_last else
                                  'Start Calibration →' if step == 'intro' else 'Next →')
        self._da.queue_draw()

    def _update_value_label(self):
        step = STEPS[self._step_idx]
        if step == 'sdr_white':
            v = self._sdr_nits
            self._value_lbl.set_label(f'{v} nits  ({pq_percent(v):.1f}% PQ)')
        elif step == 'peak_nits':
            v = self._peak_nits
            self._value_lbl.set_label(f'{v} nits  ({pq_percent(v):.1f}% PQ)')
        elif step == 'gamut':
            self._value_lbl.set_label(f'{self._gamut}%')
        elif step == 'midtone':
            v = self._midtone
            desc = 'neutral' if 95 <= v <= 105 else ('punch' if v > 105 else 'lifted')
            self._value_lbl.set_label(f'{v}%  ({desc})')

    def _on_slider(self, slider):
        v = int(slider.get_value())
        step = STEPS[self._step_idx]
        if step == 'sdr_white':   self._sdr_nits  = v
        elif step == 'peak_nits': self._peak_nits = v
        elif step == 'gamut':     self._gamut     = v
        elif step == 'midtone':   self._midtone   = v
        self._update_value_label()
        self._da.queue_draw()
        if not self._pending:
            self._pending = True
            self._debounce_id = GLib.timeout_add(250, self._live_apply)

    def _live_apply(self):
        self._debounce_id = 0
        self._pending = False
        self._do_apply(save=False)
        return GLib.SOURCE_REMOVE

    def _on_close(self, *_):
        if self._debounce_id:
            GLib.source_remove(self._debounce_id)
            self._debounce_id = 0
        if self._live_proc is not None:
            try:
                self._live_proc.kill()
            except ProcessLookupError:
                pass
        return False

    def _do_apply(self, save=False):
        flag = '--save' if save else '--no-vt-switch'
        # Reap the previous live-preview process before spawning another, so
        # pkexec invocations don't stack up while the user drags a slider.
        if not save and self._live_proc is not None:
            try:
                self._live_proc.kill()
                self._live_proc.wait(timeout=1)
            except (ProcessLookupError, subprocess.TimeoutExpired):
                pass
        cmd = [
            'pkexec', BIN,
            flag,
            '--sdr-nits',      str(self._sdr_nits),
            '--peak-nits',     str(self._peak_nits),
            '--gamut',         str(self._gamut),
            '--bpc',           str(self._bpc),
            '--gamut-mode',    self._gamut_mode,
            '--midtone-gamma', str(self._midtone),
        ]
        proc = subprocess.Popen(cmd, close_fds=True)
        if save:
            proc.wait()
        else:
            self._live_proc = proc

    # ── Drawing ────────────────────────────────────────────────────────────────

    def _draw_step(self, widget, cr, w, h):
        try:
            self._draw_step_body(cr, w, h)
        except Exception as exc:
            traceback.print_exc()
            cr.set_source_rgb(0.18, 0.0, 0.0)
            cr.paint()
            cr.set_source_rgb(1, 1, 1)
            cr.select_font_face('sans-serif', 0, 0)
            cr.set_font_size(min(w, h) * 0.02)
            msg = f'Draw error: {exc}'
            te = cr.text_extents(msg)
            cr.move_to((w - te[2]) / 2, h / 2)
            cr.show_text(msg)

    def _draw_step_body(self, cr, w, h):
        step = STEPS[self._step_idx]
        if step == 'intro':
            cr.set_source_rgb(0.03, 0.03, 0.03)
            cr.paint()
            # Title
            cr.set_source_rgb(1, 1, 1)
            cr.select_font_face('sans-serif', 0, 1)
            cr.set_font_size(min(w, h) * 0.045)
            title = 'HDR Display Calibration'
            te = cr.text_extents(title)
            cr.move_to((w - te[2]) / 2, h * 0.38)
            cr.show_text(title)
            cr.select_font_face('sans-serif', 0, 0)
            cr.set_font_size(min(w, h) * 0.018)
            cr.set_source_rgba(1, 1, 1, 0.5)
            sub = 'Make sure HDR is enabled on this display before starting'
            te2 = cr.text_extents(sub)
            cr.move_to((w - te2[2]) / 2, h * 0.48)
            cr.show_text(sub)
        elif step == 'sdr_white':
            draw_sdr_white(cr, w, h, self._sdr_nits)
        elif step == 'peak_nits':
            draw_peak_nits(cr, w, h, self._sdr_nits, self._peak_nits)
        elif step == 'gamut':
            draw_gamut(cr, w, h, self._gamut)
        elif step == 'midtone':
            draw_midtone(cr, w, h, self._midtone)
        elif step == 'done':
            cr.set_source_rgb(0.02, 0.02, 0.02)
            cr.paint()
            cr.set_source_rgb(1, 1, 1)
            cr.select_font_face('sans-serif', 0, 0)
            cr.set_font_size(min(w, h) * 0.022)
            lines = [
                f'SDR White:     {self._sdr_nits} nits  ({pq_percent(self._sdr_nits):.1f}% PQ)',
                f'Peak Luminance: {self._peak_nits} nits  ({pq_percent(self._peak_nits):.1f}% PQ)',
                f'Gamut:          {self._gamut}% expansion  ({self._gamut_mode})',
                f'Midtone Gamma:  {self._midtone}%',
            ]
            for i, line in enumerate(lines):
                te = cr.text_extents(line)
                cr.set_source_rgba(1, 1, 1, 0.85)
                cr.move_to((w - te[2]) / 2, h * 0.32 + i * min(w, h) * 0.042)
                cr.show_text(line)


_CSS_LOADED = False


def _apply_css():
    global _CSS_LOADED
    if _CSS_LOADED:
        return
    css = Gtk.CssProvider()
    css.load_from_string(CSS)
    Gtk.StyleContext.add_provider_for_display(
        Gdk.Display.get_default(), css, Gtk.STYLE_PROVIDER_PRIORITY_APPLICATION)
    _CSS_LOADED = True


# ── Entry point ────────────────────────────────────────────────────────────────

def _get_arg(args, flag, default):
    try: return args[args.index(flag) + 1]
    except (ValueError, IndexError): return str(default)


def _get_int_arg(args, flag, default, lo, hi):
    raw = _get_arg(args, flag, default)
    try:
        v = int(raw)
    except ValueError:
        print(f'hdr-cal: invalid value {raw!r} for {flag}; using default {default}',
              file=sys.stderr)
        v = default
    return min(max(v, lo), hi)


def main():
    args = sys.argv[1:]

    if '--calibrate' in args:
        sdr_nits      = _get_int_arg(args, '--sdr-nits',      203,   80, 400)
        peak_nits     = _get_int_arg(args, '--peak-nits',     800,  400, 1600)
        gamut         = _get_int_arg(args, '--gamut',         100,    0, 100)
        bpc           = _get_int_arg(args, '--bpc',            10,    6, 16)
        gamut_mode    = _get_arg(args, '--gamut-mode',    'bt2020')
        midtone_gamma = max(1, _get_int_arg(args, '--midtone-gamma', 100, 1, 250))

        if gamut_mode not in GAMUT_MODES:
            print(f'hdr-cal: invalid --gamut-mode {gamut_mode!r}; using bt2020',
                  file=sys.stderr)
            gamut_mode = 'bt2020'

        def on_activate(app):
            WizardWindow(app, sdr_nits, peak_nits, gamut, bpc, gamut_mode, midtone_gamma).present()

        app = Gtk.Application(application_id='ru.sigmachan.HdrCal.Wizard')
        app.connect('activate', on_activate)
        app.run([])
    else:
        pattern = args[0] if args else 'white'

        def on_activate(app):
            StaticPatternWindow(app, pattern).present()

        app = Gtk.Application(application_id='ru.sigmachan.HdrCal.Pattern')
        app.connect('activate', on_activate)
        app.run([])


if __name__ == '__main__':
    main()
