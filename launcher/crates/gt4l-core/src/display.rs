//! Display target and the automatic render plan (TASK-080 addendum, owner design 2026-10-21; decision 0071).
//!
//! The player picks a **Monitor** and a **Resolution** (the output size W x H) and a **Render scale**. There is no
//! aspect choice and no internal-resolution choice:
//! - the aspect is always the output size's ratio (`GT4_DISPLAY_ASPECT=auto` with `GT4_OUTPUT_SIZE`); at 16:9 nothing
//!   changes, at any other ratio the game's P1/P2 path (camera Hor+/Vert+, race view fill, HUD centred 16:9) turns on
//!   by itself;
//! - the internal render resolution is never below the output: `k = ceil(max(W / 640, H / 448))`, where 640 x 448 is
//!   GT4's native frame (wide screens stretch its 640 width), and the effective multiplier is `k x render scale`,
//!   rounded to a multiplier the renderer accepts.
//!
//! Everything here is pure and platform-independent (Windows reads the monitors in `gt4l-platform::display`; Android
//! and iOS will feed their own screen size into [`display_plan`]).

use std::fmt;

/// GT4's native frame (the game draws 640 x 448 at 480p; 16:9 is the same frame stretched).
pub const NATIVE_FRAME_WIDTH: u32 = 640;
pub const NATIVE_FRAME_HEIGHT: u32 = 448;

/// The game treats a ratio within this relative distance of 16:9 as exactly 16:9 (`native/display_aspect.hpp`
/// `kSnapTolerance`); 1366 x 768 is 16:9.
pub const SNAP_TOLERANCE: f64 = 0.005;
/// The ratio range the game accepts (`kMinRatio`, `kMaxRatio`); the output size must give a ratio inside it.
pub const MIN_RATIO: f64 = 0.5;
pub const MAX_RATIO: f64 = 8.0;
/// Output sizes the game accepts for `GT4_OUTPUT_SIZE` (each side), the same rule as `native/display_aspect.cpp`.
pub const MIN_OUTPUT_SIDE: u32 = 320;
pub const MAX_OUTPUT_SIDE: u32 = 16384;

/// Render scale: **the internal frame's height as a percentage of the screen's height** (owner, 2026-11-05; TASK-180
/// test3). 1x is 448 px high, so on a 1440-pixel-high screen 1x is 31% and 2x is 62%; 100% is "as tall as the screen".
/// The whole multiplier is the one closest to the percentage, so the slider, its label and the stored value are one
/// number on every platform (the old model, one 25% notch = one multiplier from the screen's native multiplier, called
/// 1x "0%" on a phone and meant 5x at 100% there).
pub const RENDER_SCALE_MIN: u32 = 1;
pub const RENDER_SCALE_MAX: u32 = 1200;
pub const RENDER_SCALE_STEP: u32 = 1;
pub const RENDER_SCALE_DEFAULT: u32 = 100;
/// The multiplier GT4's own default scale gives on a screen where the frame is smaller than the screen (the owner's
/// 2x default on Android, and the same "sharper than 1x, cheaper than native" choice elsewhere).
pub const RENDER_SCALE_DEFAULT_MULTIPLIER: u32 = 2;

/// The internal multiplier an integrated GPU gets while the Render scale is the untouched default (issue #89): GT4's
/// native 640 x 448 frame. Kept for the plans of older files; the height percentage needs no GPU-dependent baseline.
pub const INTEGRATED_DEFAULT_MULTIPLIER: u32 = 1;

/// What a renderer accepts as an internal-resolution multiplier.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct RendererCaps {
    /// Whole multipliers 1 to `max_multiplier` only (no fractional scale).
    pub max_multiplier: u32,
}

/// The PCSX2 GS (gt4-gs): `[EmuCore/GS] upscale_multiplier` takes the whole numbers 1 to 12 (the gt4-gs allow-list,
/// docs/launcher/gs-settings-allowlist.json; the table row `upscale`). A value above the GPU's own limit
/// (max texture size / 1280) is lowered by gt4-gs itself with a notice; gt4-gs does not report that limit yet.
pub const PCSX2_GS_CAPS: RendererCaps = RendererCaps { max_multiplier: 12 };

/// The plan for one output size.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct DisplayPlan {
    /// The output size, landscape (a portrait input is turned sideways: the game is landscape only).
    pub output_width: u32,
    pub output_height: u32,
    /// Whether the input was portrait (height above width) and was turned.
    pub portrait_input: bool,
    /// Always false since the Render scale is a percentage of the screen height (TASK-180 test3): kept for the callers
    /// that log the plan, and for the older integrated-GPU rule of issue #89, which no longer changes the multiplier.
    pub integrated_default: bool,
    /// k = ceil(max(W / 640, H / 448)): the smallest whole multiplier whose frame covers the output on both axes.
    pub base_multiplier: u32,
    /// The render scale in percent that was applied.
    pub render_scale: u32,
    /// One whole multiplier per 25% step from the 100% plan (the integrated GPU's 1x baseline included), clamped to
    /// 1 .. caps.max_multiplier.
    pub multiplier: u32,
    /// 640 x multiplier by 448 x multiplier.
    pub internal_width: u32,
    pub internal_height: u32,
    /// W / H.
    pub aspect: f64,
    /// Whether the aspect is not 16:9 (within the game's snap tolerance): the game's P1/P2 aspect path is on
    /// (Hor+ for wider screens, Vert+ for narrower ones such as 16:10 and 4:3).
    pub ultrawide_on: bool,
}

/// Why an output size or render scale is refused.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct PlanError(pub String);

impl fmt::Display for PlanError {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        formatter.write_str(&self.0)
    }
}

impl std::error::Error for PlanError {}

fn ceil_div(value: u32, divisor: u32) -> u32 {
    value.div_ceil(divisor)
}

/// Whether a ratio is 16:9 for the game (within [`SNAP_TOLERANCE`]).
pub fn is_sixteen_nine(ratio: f64) -> bool {
    (ratio / (16.0 / 9.0) - 1.0).abs() <= SNAP_TOLERANCE
}

/// Whether a render scale is a value the slider can store (the slider's own positions come from
/// [`render_scale_steps_for`] for the current screen).
pub fn is_render_scale(percent: u32) -> bool {
    (RENDER_SCALE_MIN..=RENDER_SCALE_MAX).contains(&percent)
}

/// The multiplier 100% gives on an output: the smallest whole multiple of the native frame that covers it. Kept for the
/// log line of a plan (the scale itself no longer climbs from it).
pub fn base_multiplier(output_width: u32, output_height: u32) -> u32 {
    let (width, height) = if output_height > output_width { (output_height, output_width) } else { (output_width, output_height) };
    ceil_div(width, NATIVE_FRAME_WIDTH).max(ceil_div(height, NATIVE_FRAME_HEIGHT)).max(1)
}

/// The landscape height of an output (a portrait input is turned sideways by the plan).
fn landscape_height(output: Size) -> u32 {
    output.height.min(output.width)
}

/// TASK-180 test3: the lowest slider position on an output, the percentage of 1x there (31% on a 1440-high screen).
pub fn render_scale_floor(output: Size) -> u32 {
    percent_for_multiplier(1, landscape_height(output).max(1))
}

/// The render plan for an output size, a render scale (percent of the screen height) and a renderer. Pure.
///
/// Refused: a zero or out-of-range side, a ratio outside 1:2 .. 8:1 after turning a portrait input sideways, and a
/// render scale outside [`RENDER_SCALE_MIN`] .. [`RENDER_SCALE_MAX`].
pub fn display_plan(
    output_width: u32,
    output_height: u32,
    render_scale: u32,
    caps: RendererCaps,
) -> Result<DisplayPlan, PlanError> {
    display_plan_on(output_width, output_height, render_scale, caps, false)
}

/// [`display_plan`] for the GPU the game will use. The multiplier is the one closest to the stored percentage of the
/// screen's height on every platform; `integrated_gpu` only feeds the log flag now (the old model climbed from a
/// 1x baseline on an integrated GPU, which made 1x "0%" on a phone).
pub fn display_plan_on(
    output_width: u32,
    output_height: u32,
    render_scale: u32,
    caps: RendererCaps,
    _integrated_gpu: bool,
) -> Result<DisplayPlan, PlanError> {
    if !is_render_scale(render_scale) {
        return Err(PlanError(format!(
            "render scale {render_scale}% is outside {RENDER_SCALE_MIN}% to {RENDER_SCALE_MAX}%"
        )));
    }
    let portrait_input = output_height > output_width;
    let (width, height) = if portrait_input { (output_height, output_width) } else { (output_width, output_height) };
    let side_ok = |side: u32| (MIN_OUTPUT_SIDE..=MAX_OUTPUT_SIDE).contains(&side);
    if !side_ok(width) || !side_ok(height) {
        return Err(PlanError(format!(
            "output size {output_width}x{output_height} is outside {MIN_OUTPUT_SIDE} to {MAX_OUTPUT_SIDE} pixels per side"
        )));
    }
    let aspect = width as f64 / height as f64;
    if !(MIN_RATIO..=MAX_RATIO).contains(&aspect) {
        return Err(PlanError(format!("output size {output_width}x{output_height} is not a display ratio the game accepts")));
    }
    let base_multiplier = ceil_div(width, NATIVE_FRAME_WIDTH).max(ceil_div(height, NATIVE_FRAME_HEIGHT));
    let multiplier = multiplier_for_percent(render_scale, height, caps);
    Ok(DisplayPlan {
        output_width: width,
        output_height: height,
        portrait_input,
        // The new model has no GPU-dependent baseline: the percentage alone decides (kept for the log line of Play).
        integrated_default: false,
        base_multiplier,
        render_scale,
        multiplier,
        internal_width: NATIVE_FRAME_WIDTH * multiplier,
        internal_height: NATIVE_FRAME_HEIGHT * multiplier,
        aspect,
        ultrawide_on: !is_sixteen_nine(aspect),
    })
}

/// "4x · 2560x1792": the slider's readout for a plan.
pub fn describe_plan(plan: &DisplayPlan) -> String {
    format!("{}x \u{00B7} {}x{}", plan.multiplier, plan.internal_width, plan.internal_height)
}

// Render scale as a percentage of the screen ------------------------------------------------------------------------

/// TASK-180 test3 (owner, 2026-11-05): the Render scale percentage means **the internal frame's height against the
/// screen's height**: 1x is 448 px high, so on a 1440-pixel-high screen 1x is 31% and 2x is 62%. Height is the axis
/// chosen on purpose: every multiplier scales the frame's height by exactly the same factor (448 -> 896 -> 1344), while
/// the width already carries the aspect (640 at 4:3, stretched to the screen's wider shape by the game's own aspect
/// path), so a width-based percentage would mix the aspect into the number the player reads. The frame height is what
/// the multiplier is *for* (the vertical resolution), and it is the one number that does not change with the screen's
/// shape. Before this, 100% meant "the screen's native multiplier" (5x at 3088x1440) and 1x was labelled 0%.
pub fn percent_for_multiplier(multiplier: u32, output_height: u32) -> u32 {
    if output_height == 0 {
        return RENDER_SCALE_DEFAULT;
    }
    let percent = (u64::from(NATIVE_FRAME_HEIGHT) * u64::from(multiplier.max(1)) * 100 + u64::from(output_height) / 2)
        / u64::from(output_height);
    percent.clamp(1, u64::from(RENDER_SCALE_MAX)) as u32
}

/// The whole multiplier closest to a percentage of the screen's height (ties go to the smaller one, so a rounded-down
/// label never grows the internal resolution behind the player's back).
pub fn multiplier_for_percent(percent: u32, output_height: u32, caps: RendererCaps) -> u32 {
    if output_height == 0 {
        return 1;
    }
    let numerator = u64::from(percent) * u64::from(output_height);
    let denominator = 100 * u64::from(NATIVE_FRAME_HEIGHT);
    // Nearest whole multiplier: round half down.
    let multiplier = (2 * numerator + denominator - 1) / (2 * denominator);
    (multiplier.max(1) as u32).min(caps.max_multiplier.max(1))
}

/// The percentage the slider shows for a multiplier on a screen of this height ("31%").
pub fn render_scale_label(multiplier: u32, output_height: u32) -> String {
    format!("{}%", percent_for_multiplier(multiplier, output_height))
}

/// The slider positions on an output: one per multiplier the renderer accepts, in order, each as the percentage of the
/// screen's height it gives there. This is the whole scale model: label, slider and stored value are the same number.
pub fn render_scale_steps_for(output_height: u32, caps: RendererCaps) -> Vec<u32> {
    let mut steps: Vec<u32> = (1..=caps.max_multiplier.max(1)).map(|m| percent_for_multiplier(m, output_height)).collect();
    steps.sort_unstable();
    steps.dedup();
    steps
}

/// The default Render scale on an output: 2x, as the owner asked for (owner, 2026-11-05), in the same percentage.
pub fn render_scale_default_for(output_height: u32) -> u32 {
    percent_for_multiplier(2, output_height)
}

/// The stored Render scale a value written under the old notch model becomes, with the log line that explains it.
/// Windows: the multiplier the old value meant on this screen is kept, so the player's picture does not change.
/// Android: the phone's notches were never a percentage the player chose (test1's "125" became 6x on the S23 and the
/// menus ran at 12 fps), so the value goes to the 2x default (owner, 2026-11-05).
pub fn migrate_render_scale(old_percent: u32, output: Size, integrated_gpu: bool, caps: RendererCaps, android: bool) -> (u32, String) {
    let height = landscape_height(output).max(1);
    if android {
        let percent = percent_for_multiplier(RENDER_SCALE_DEFAULT_MULTIPLIER, height);
        return (
            percent,
            format!(
                "settings: render_scale {old_percent} was stored under the old scale model; on Android it goes to the 2x default, {percent}% of the screen height"
            ),
        );
    }
    let (multiplier, percent) = migrate_notch_scale(old_percent, output, integrated_gpu, caps);
    (
        percent,
        format!(
            "settings: render_scale {old_percent} was stored under the old scale model; it meant {multiplier}x on this screen, stored as {percent}% of its height"
        ),
    )
}

/// TASK-180 test3: a Render scale stored under the old "one notch of 25% = one multiplier" model, converted to the
/// multiplier it meant on the screen and GPU of that time, then to the percentage of this screen's height. The old
/// model is `multiplier = baseline + (percent - 100) / 25`, with baseline k (the smallest multiplier whose frame covers
/// the output) on a discrete GPU and 1 on an integrated one. Returns `(old multiplier, new percentage)`.
pub fn migrate_notch_scale(old_percent: u32, output: Size, integrated_gpu: bool, caps: RendererCaps) -> (u32, u32) {
    let baseline = if integrated_gpu { INTEGRATED_DEFAULT_MULTIPLIER } else { base_multiplier(output.width, output.height) };
    // 25 was the old model's notch; the constant is written out here because the current step is 1%.
    let steps = (old_percent as i32 - 100) / 25;
    let multiplier = (baseline as i32 + steps).clamp(1, caps.max_multiplier.max(1) as i32) as u32;
    (multiplier, percent_for_multiplier(multiplier, landscape_height(output)))
}

// Monitors ---------------------------------------------------------------------------------------------------------

/// A width x height pair in physical pixels.
#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub struct Size {
    pub width: u32,
    pub height: u32,
}

impl Size {
    pub const fn new(width: u32, height: u32) -> Size {
        Size { width, height }
    }

    /// "2560x1440".
    pub fn text(&self) -> String {
        format!("{}x{}", self.width, self.height)
    }

    /// Parses "WxH" (decimal digits only, each side 1 .. 16384). The game applies the same rule to
    /// `GT4_OUTPUT_SIZE` (with the side range above).
    pub fn parse(text: &str) -> Option<Size> {
        let (width, height) = text.split_once('x')?;
        let side = |part: &str| -> Option<u32> {
            if part.is_empty() || part.len() > 5 || !part.chars().all(|c| c.is_ascii_digit()) {
                return None;
            }
            part.parse::<u32>().ok().filter(|value| (1..=MAX_OUTPUT_SIDE).contains(value))
        };
        Some(Size { width: side(width)?, height: side(height)? })
    }
}

/// One connected monitor as the platform reads it.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Monitor {
    /// The GDI device name, e.g. `\\.\DISPLAY1` (what the game's `GT4_MONITOR` takes).
    pub device: String,
    /// The friendly name ("DELL U3419W"); empty when the system has none.
    pub name: String,
    pub primary: bool,
    /// The current desktop mode.
    pub desktop: Size,
    /// The modes the monitor supports: unique sizes, sorted (largest width first, then largest height).
    pub modes: Vec<Size>,
}

/// Smallest mode offered: the game's own 640 x 480 picture (smaller modes are display oddities, not choices).
pub const MIN_MODE: Size = Size::new(640, 480);

/// Unique sizes, at least [`MIN_MODE`] on both sides, sorted largest first. The desktop size is always included.
pub fn tidy_modes(raw: &[Size], desktop: Size) -> Vec<Size> {
    let mut modes: Vec<Size> = raw
        .iter()
        .copied()
        .chain(std::iter::once(desktop))
        .filter(|size| size.width >= MIN_MODE.width && size.height >= MIN_MODE.height)
        .collect();
    modes.sort_by(|a, b| b.cmp(a));
    modes.dedup();
    if modes.is_empty() {
        modes.push(desktop);
    }
    modes
}

/// The stored identifier of a monitor: the device name and its desktop size, `\\.\DISPLAY1@3440x1440`. Stable across
/// launches; not an index (monitor order changes when screens are plugged in or out).
pub fn monitor_id(monitor: &Monitor) -> String {
    format!("{}@{}", monitor.device, monitor.desktop.text())
}

/// Splits a stored monitor identifier into the device name and the desktop size. `None` when it is not of that form.
pub fn parse_monitor_id(text: &str) -> Option<(&str, Size)> {
    let (device, size) = text.rsplit_once('@')?;
    if device.is_empty() || device.len() > 64 || !device.chars().all(|c| c.is_ascii_graphic()) {
        return None;
    }
    Some((device, Size::parse(size)?))
}

/// The stored value that means "the primary monitor" (the default until the player picks one).
pub const PRIMARY: &str = "primary";
/// The stored value that means "the monitor's desktop mode" (the default resolution).
pub const DESKTOP: &str = "desktop";

/// The monitor's friendly name ("DELL U3419W"); a monitor without one is called by its device ("DISPLAY1").
pub fn monitor_base_name(monitor: &Monitor) -> String {
    if monitor.name.trim().is_empty() {
        monitor.device.trim_start_matches(r"\\.\").to_string()
    } else {
        monitor.name.trim().to_string()
    }
}

/// The labels of the Monitor row, one per monitor in order: the friendly name only (no resolution, no "primary").
/// A monitor whose name is already taken gets the smallest suffix that tells it apart: " 2", then " 3" and so on.
pub fn monitor_labels(monitors: &[Monitor]) -> Vec<String> {
    let mut used: Vec<String> = Vec::new();
    let mut labels = Vec::new();
    for monitor in monitors {
        let base = monitor_base_name(monitor);
        let mut label = base.clone();
        let mut number = 2;
        while used.contains(&label) {
            label = format!("{base} {number}");
            number += 1;
        }
        used.push(label.clone());
        labels.push(label);
    }
    labels
}

/// The monitor a stored value names. Order: the exact identifier; the same device; a monitor with the same desktop
/// size; the primary monitor; the first monitor. `None` only for an empty list.
pub fn resolve_monitor<'a>(monitors: &'a [Monitor], stored: &str) -> Option<&'a Monitor> {
    let primary = || monitors.iter().find(|monitor| monitor.primary).or_else(|| monitors.first());
    if stored == PRIMARY {
        return primary();
    }
    let Some((device, desktop)) = parse_monitor_id(stored) else {
        return primary();
    };
    monitors
        .iter()
        .find(|monitor| monitor.device == device && monitor.desktop == desktop)
        .or_else(|| monitors.iter().find(|monitor| monitor.device == device))
        .or_else(|| monitors.iter().find(|monitor| monitor.desktop == desktop))
        .or_else(primary)
}

/// The output size a stored resolution gives on a monitor: the stored size when the monitor supports it, otherwise
/// (and for [`DESKTOP`]) the monitor's desktop mode.
pub fn resolve_resolution(monitor: &Monitor, stored: &str) -> Size {
    match Size::parse(stored) {
        Some(size) if monitor.modes.contains(&size) => size,
        _ => monitor.desktop,
    }
}

/// The desktop size a stored monitor identifier recorded when it was saved (`\\.\DISPLAY2@3440x1440` gives 3440x1440).
/// `None` for "primary" and for anything else without a size.
pub fn desktop_at_save(stored_monitor: &str) -> Option<Size> {
    parse_monitor_id(stored_monitor).map(|(_, size)| size)
}

/// The mode a stored explicit resolution asked for when this monitor no longer has it: `Some(desktop)` then (the
/// launcher logs it and plays at the desktop mode). `None` for "desktop", a mode the monitor supports, or no size.
pub fn resolution_fallback(monitor: &Monitor, stored: &str) -> Option<Size> {
    match Size::parse(stored) {
        Some(size) if !monitor.modes.contains(&size) => Some(monitor.desktop),
        _ => None,
    }
}

/// What the game is started with: the monitor's device name and the output size.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct DisplayTarget {
    pub device: String,
    pub output: Size,
}

/// The target for the stored Monitor and Resolution values; `None` when no monitor is known (the game then opens on
/// the primary monitor at its desktop size, as before).
pub fn resolve_target(monitors: &[Monitor], stored_monitor: &str, stored_resolution: &str) -> Option<DisplayTarget> {
    let monitor = resolve_monitor(monitors, stored_monitor)?;
    Some(DisplayTarget { device: monitor.device.clone(), output: resolve_resolution(monitor, stored_resolution) })
}

/// The output size used when no monitor is known (tests, a platform without monitors): 1920 x 1080, which gives 3x,
/// the table default the 1.0.0 launcher always wrote.
pub const FALLBACK_OUTPUT: Size = Size::new(1920, 1080);

#[cfg(test)]
mod tests {
    use super::*;

    fn plan(width: u32, height: u32, scale: u32) -> DisplayPlan {
        display_plan(width, height, scale, PCSX2_GS_CAPS).unwrap()
    }

    #[test]
    fn the_floor_is_the_percentage_of_1x() {
        // 1x is 448 pixels high: 31% on the owner's S23 Ultra (1440), 41% on a 1080p screen, 56% on 800.
        assert_eq!(render_scale_floor(Size::new(3088, 1440)), 31);
        assert_eq!(render_scale_floor(Size::new(2316, 1080)), 41);
        assert_eq!(render_scale_floor(Size::new(1920, 1080)), 41);
        assert_eq!(render_scale_floor(Size::new(1440, 3088)), 31, "portrait sizes count as landscape");
        assert_eq!(render_scale_floor(Size::new(1280, 800)), 56);
        assert_eq!(base_multiplier(3088, 1440), 5);
    }

    #[test]
    fn the_owner_k_table() {
        for (width, height, k) in [
            (1920, 1080, 3),
            (1920, 1200, 3),
            (1680, 1050, 3),
            (2560, 1080, 4),
            (2560, 1440, 4),
            (3440, 1440, 6),
            (3088, 1440, 5),
            (3840, 2160, 6),
        ] {
            let result = plan(width, height, percent_for_multiplier(k, height));
            assert_eq!(result.base_multiplier, k, "{width}x{height}");
            assert_eq!(result.multiplier, k, "{width}x{height} at its k percentage");
            assert!(640 * k >= width && 448 * k >= height, "never below the output on either axis");
        }
    }

    #[test]
    fn aspect_and_the_ultrawide_path() {
        assert!(!plan(1920, 1080, 100).ultrawide_on, "16:9: nothing changes");
        assert!(!plan(1366, 768, 100).ultrawide_on, "1366x768 snaps to 16:9 as in the game");
        assert!(!plan(3840, 2160, 100).ultrawide_on);
        for (width, height) in [(2560, 1080), (3440, 1440), (3088, 1440), (1920, 1200), (1680, 1050), (1280, 1024)] {
            assert!(plan(width, height, 100).ultrawide_on, "{width}x{height}");
        }
        assert!((plan(3440, 1440, 100).aspect - 3440.0 / 1440.0).abs() < 1e-12);
    }

    #[test]
    fn a_phone_screen_and_portrait_input_are_safe() {
        let phone = plan(3088, 1440, percent_for_multiplier(5, 1440));
        assert_eq!((phone.base_multiplier, phone.internal_width, phone.internal_height), (5, 3200, 2240));
        assert!(phone.ultrawide_on && !phone.portrait_input);
        // The same phone reporting its natural (portrait) orientation: turned sideways, same plan.
        let portrait = plan(1440, 3088, percent_for_multiplier(5, 1440));
        assert!(portrait.portrait_input);
        assert_eq!((portrait.output_width, portrait.output_height), (3088, 1440));
        assert_eq!(portrait.multiplier, phone.multiplier);
        assert_eq!(portrait.aspect, phone.aspect);
        let square = plan(1080, 1080, 100);
        assert_eq!(square.base_multiplier, 3);
        assert_eq!(square.multiplier, 2, "100% is 448 pixels of 1080: 2x");
        assert!(square.ultrawide_on);
    }

    #[test]
    fn invalid_inputs_are_refused() {
        assert!(display_plan(0, 1080, 100, PCSX2_GS_CAPS).is_err());
        assert!(display_plan(1920, 0, 100, PCSX2_GS_CAPS).is_err());
        assert!(display_plan(100, 100, 100, PCSX2_GS_CAPS).is_err(), "below 320");
        assert!(display_plan(20000, 2000, 100, PCSX2_GS_CAPS).is_err(), "above 16384");
        assert!(display_plan(16000, 1000, 100, PCSX2_GS_CAPS).is_err(), "ratio 16:1");
        for scale in [0, 1201, 5000] {
            assert!(display_plan(1920, 1080, scale, PCSX2_GS_CAPS).is_err(), "{scale}%");
        }
        assert!(display_plan(1920, 1080, 1, PCSX2_GS_CAPS).is_ok(), "1% is 1x on a big screen");
        assert!(display_plan(1920, 1080, 1200, PCSX2_GS_CAPS).is_ok(), "12x on a short screen is 1200%");
    }

    fn monitor(device: &str, name: &str, primary: bool, desktop: Size) -> Monitor {
        let raw = [Size::new(800, 600), Size::new(1280, 720), Size::new(1920, 1080), desktop, Size::new(320, 200)];
        Monitor { device: device.into(), name: name.into(), primary, desktop, modes: tidy_modes(&raw, desktop) }
    }

    #[test]
    fn modes_are_unique_sorted_and_without_tiny_sizes() {
        let wide = monitor(r"\\.\DISPLAY2", "DELL U3419W", false, Size::new(3440, 1440));
        assert_eq!(
            wide.modes,
            vec![Size::new(3440, 1440), Size::new(1920, 1080), Size::new(1280, 720), Size::new(800, 600)]
        );
        assert_eq!(monitor_base_name(&wide), "DELL U3419W");
    }

    #[test]
    fn labels_are_the_friendly_name_with_a_suffix_only_for_duplicates() {
        let wide = monitor(r"\\.\DISPLAY2", "DELL U3419W", false, Size::new(3440, 1440));
        let unnamed = monitor(r"\\.\DISPLAY1", "", true, Size::new(1920, 1080));
        assert_eq!(monitor_labels(&[wide.clone(), unnamed.clone()]), vec!["DELL U3419W".to_string(), "DISPLAY1".to_string()]);
        let twin = monitor(r"\\.\DISPLAY3", "DELL U3419W", false, Size::new(2560, 1080));
        let third = monitor(r"\\.\DISPLAY4", "DELL U3419W", false, Size::new(1920, 1080));
        assert_eq!(
            monitor_labels(&[wide, twin, unnamed, third]),
            vec!["DELL U3419W", "DELL U3419W 2", "DISPLAY1", "DELL U3419W 3"]
        );
    }

    #[test]
    fn a_stored_mode_the_monitor_lost_falls_back_to_the_desktop() {
        let wide = monitor(r"\\.\DISPLAY2", "DELL", false, Size::new(3440, 1440));
        assert_eq!(resolution_fallback(&wide, "1920x1080"), None, "a mode the monitor has");
        assert_eq!(resolution_fallback(&wide, "5120x2160"), Some(Size::new(3440, 1440)), "a lost mode");
        assert_eq!(resolution_fallback(&wide, DESKTOP), None, "desktop is never a fallback");
        assert_eq!(desktop_at_save(r"\\.\DISPLAY2@3440x1440"), Some(Size::new(3440, 1440)));
        assert_eq!(desktop_at_save(PRIMARY), None);
    }

    #[test]
    fn monitors_resolve_by_identifier_device_size_then_primary() {
        let monitors = vec![
            monitor(r"\\.\DISPLAY1", "TV", true, Size::new(1920, 1080)),
            monitor(r"\\.\DISPLAY2", "DELL", false, Size::new(3440, 1440)),
        ];
        let id = monitor_id(&monitors[1]);
        assert_eq!(id, r"\\.\DISPLAY2@3440x1440");
        assert_eq!(parse_monitor_id(&id), Some((r"\\.\DISPLAY2", Size::new(3440, 1440))));
        assert_eq!(resolve_monitor(&monitors, PRIMARY).unwrap().device, r"\\.\DISPLAY1");
        assert_eq!(resolve_monitor(&monitors, &id).unwrap().device, r"\\.\DISPLAY2");
        // Desktop size changed since it was stored: the same device still wins.
        assert_eq!(resolve_monitor(&monitors, r"\\.\DISPLAY2@2560x1080").unwrap().device, r"\\.\DISPLAY2");
        // Device numbers moved (screens re-plugged): the one with the stored desktop size.
        assert_eq!(resolve_monitor(&monitors, r"\\.\DISPLAY7@3440x1440").unwrap().device, r"\\.\DISPLAY2");
        // Gone entirely: the primary.
        assert_eq!(resolve_monitor(&monitors, r"\\.\DISPLAY7@5120x1440").unwrap().device, r"\\.\DISPLAY1");
        assert_eq!(resolve_monitor(&monitors, "garbage").unwrap().device, r"\\.\DISPLAY1");
        assert!(resolve_monitor(&[], PRIMARY).is_none());
    }

    #[test]
    fn resolutions_resolve_to_a_supported_mode_or_the_desktop() {
        let wide = monitor(r"\\.\DISPLAY2", "DELL", false, Size::new(3440, 1440));
        assert_eq!(resolve_resolution(&wide, DESKTOP), Size::new(3440, 1440));
        assert_eq!(resolve_resolution(&wide, "1920x1080"), Size::new(1920, 1080));
        assert_eq!(resolve_resolution(&wide, "2560x1600"), Size::new(3440, 1440), "not a mode of this monitor");
        let target = resolve_target(std::slice::from_ref(&wide), PRIMARY, "1280x720").unwrap();
        assert_eq!(target, DisplayTarget { device: r"\\.\DISPLAY2".into(), output: Size::new(1280, 720) });
        assert!(resolve_target(&[], PRIMARY, DESKTOP).is_none());
    }

    #[test]
    fn sizes_parse_strictly() {
        assert_eq!(Size::parse("2560x1440"), Some(Size::new(2560, 1440)));
        for bad in ["", "2560", "2560x", "x1440", "2560X1440", "-1x5", "2560x1440x1", " 2560x1440", "0x0", "99999x1"] {
            assert_eq!(Size::parse(bad), None, "{bad:?}");
        }
    }

    // TASK-180 test3 (owner, 2026-11-05): the Render scale is the internal frame height as a percentage of the screen
    // height, on every platform. These are the screens the owner named, plus the phone the bug was found on.

    #[test]
    fn the_render_scale_percentage_is_the_frame_height_over_the_screen_height() {
        // The owner's example: a 1440-pixel-high screen has 1x (448) at about 31% and 2x (896) at about 62%.
        for screen in [Size::new(3088, 1440), Size::new(1920, 1080), Size::new(2560, 1440), Size::new(3440, 1440)] {
            let one = percent_for_multiplier(1, screen.height);
            let two = percent_for_multiplier(2, screen.height);
            assert_eq!(one, (448u64 * 100 / u64::from(screen.height)) as u32, "{screen:?} 1x");
            assert_eq!(two, percent_for_multiplier(2, screen.height));
            if screen.height == 1440 {
                assert_eq!((one, two), (31, 62), "the owner's numbers");
            }
        }
        for (screen, one, two, three) in [
            (Size::new(3088, 1440), 31, 62, 93),
            (Size::new(2400, 1080), 41, 83, 124),
            (Size::new(1920, 1080), 41, 83, 124),
            (Size::new(2560, 1440), 31, 62, 93),
            (Size::new(3440, 1440), 31, 62, 93),
            (Size::new(1280, 800), 56, 112, 168),
        ] {
            assert_eq!(percent_for_multiplier(1, screen.height), one, "{screen:?} 1x");
            assert_eq!(percent_for_multiplier(2, screen.height), two, "{screen:?} 2x");
            assert_eq!(percent_for_multiplier(3, screen.height), three, "{screen:?} 3x");
        }
    }

    #[test]
    fn every_screen_reaches_1x_and_2x_from_its_own_percentages() {
        let caps = RendererCaps { max_multiplier: 12 };
        for screen in [
            Size::new(3088, 1440),
            Size::new(2400, 1080),
            Size::new(1920, 1080),
            Size::new(2560, 1440),
            Size::new(3440, 1440),
            Size::new(1280, 800),
        ] {
            for multiplier in 1..=6 {
                let percent = percent_for_multiplier(multiplier, screen.height);
                assert_eq!(
                    multiplier_for_percent(percent, screen.height, caps),
                    multiplier,
                    "{screen:?} {multiplier}x at {percent}%"
                );
                let plan = display_plan(screen.width, screen.height, percent, caps).unwrap();
                assert_eq!(plan.multiplier, multiplier, "{screen:?} at {percent}%");
                assert_eq!(plan.internal_height, 448 * multiplier);
                // The label, the slider position and the stored value are the same number: the plan kept it.
                assert_eq!(plan.render_scale, percent);
                assert_eq!(render_scale_label(multiplier, screen.height), format!("{percent}%"));
            }
        }
    }

    #[test]
    fn the_slider_steps_are_one_per_multiplier_and_start_at_1x() {
        let caps = RendererCaps { max_multiplier: 6 };
        let steps = render_scale_steps_for(1440, caps);
        assert_eq!(steps, vec![31, 62, 93, 124, 156, 187], "one position per multiplier, the smallest = 1x");
        assert_eq!(steps[0], render_scale_floor(Size::new(3088, 1440)), "the floor is 1x, not 0%");
        // A portrait surface is read as the landscape one the game draws (the plan turns it).
        assert_eq!(render_scale_floor(Size::new(1440, 3088)), 31);
    }

    #[test]
    fn the_default_scale_is_2x() {
        assert_eq!(percent_for_multiplier(RENDER_SCALE_DEFAULT_MULTIPLIER, 1440), 62);
        assert_eq!(render_scale_default_for(1440), 62);
        let plan = display_plan(3088, 1440, render_scale_default_for(1440), PCSX2_GS_CAPS).unwrap();
        assert_eq!(plan.multiplier, 2);
        assert_eq!((plan.internal_width, plan.internal_height), (1280, 896));
    }

    #[test]
    fn the_multiplier_is_clamped_by_the_renderer_and_never_zero() {
        let capped = RendererCaps { max_multiplier: 1 };
        assert_eq!(display_plan(1920, 1080, 300, capped).unwrap().multiplier, 1);
        let zero_cap = RendererCaps { max_multiplier: 0 };
        assert_eq!(display_plan(1920, 1080, 100, zero_cap).unwrap().multiplier, 1);
        assert_eq!(multiplier_for_percent(1, 4320, PCSX2_GS_CAPS), 1, "1% of a 4K screen is still 1x");
        assert_eq!(multiplier_for_percent(1200, 448, PCSX2_GS_CAPS), 12, "1200% of 448 px is 12x");
        assert_eq!(multiplier_for_percent(300, 800, PCSX2_GS_CAPS), 5, "300% of 800 is 2400 px: 5x");
        assert_eq!(multiplier_for_percent(300, 4320, PCSX2_GS_CAPS), 12, "12x is the PCSX2 GS cap");
    }

    #[test]
    fn an_out_of_range_scale_is_refused() {
        assert!(!is_render_scale(0));
        assert!(!is_render_scale(RENDER_SCALE_MAX + 1));
        assert!(is_render_scale(1));
        assert!(display_plan(1920, 1080, 0, PCSX2_GS_CAPS).is_err());
        assert!(display_plan(1920, 1080, 1201, PCSX2_GS_CAPS).is_err());
    }

    #[test]
    fn an_old_notch_scale_migrates_to_the_multiplier_it_meant() {
        // Test1 stored "125" on the owner's S23 (3088 x 1440). The old rule climbed from the screen's own multiplier
        // (5x, CoreGS 2.0 reporting a discrete-class device there): 125% was 6x, which the new model stores as 187%.
        let (multiplier, percent) = migrate_notch_scale(125, Size::new(3088, 1440), false, PCSX2_GS_CAPS);
        assert_eq!(multiplier, 6);
        assert_eq!(percent, 187);
        // On a 1920 x 1080 PC the old 100% was 3x (k = 3); the new value keeps exactly that picture, at 124%.
        let (multiplier, percent) = migrate_notch_scale(100, Size::new(1920, 1080), false, PCSX2_GS_CAPS);
        assert_eq!((multiplier, percent), (3, 124));
        // An integrated laptop at the untouched 100% was 1x (issue #89): it stays 1x, at 56% on 1280 x 800.
        let (multiplier, percent) = migrate_notch_scale(100, Size::new(1280, 800), true, PCSX2_GS_CAPS);
        assert_eq!((multiplier, percent), (1, 56));
        // Values below the baseline clamp to 1x, never to 0x.
        assert_eq!(migrate_notch_scale(0, Size::new(1920, 1080), false, PCSX2_GS_CAPS), (1, 41));
    }
}
