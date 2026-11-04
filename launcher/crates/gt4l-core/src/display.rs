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

/// Render scale slider: 50% to 200% in steps of 25%, default 100%.
pub const RENDER_SCALE_MIN: u32 = 50;
pub const RENDER_SCALE_MAX: u32 = 200;
pub const RENDER_SCALE_STEP: u32 = 25;
pub const RENDER_SCALE_DEFAULT: u32 = 100;

/// The internal multiplier an integrated GPU gets while the Render scale is the untouched default (issue #89): GT4's
/// native 640 x 448 frame, so a 1440 x 900 screen starts at native resolution instead of 3x. It is also the baseline the
/// slider climbs from: 125% is 2x, 150% is 3x, up to 5x at 200%.
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
    /// Whether the integrated-GPU default applied: the render scale is the untouched default on an integrated GPU, so
    /// the multiplier is [`INTEGRATED_DEFAULT_MULTIPLIER`] instead of the scaled value (issue #89).
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

/// Whether a render scale is one of the slider's steps (50, 75, ..., 200).
pub fn is_render_scale(percent: u32) -> bool {
    (RENDER_SCALE_MIN..=RENDER_SCALE_MAX).contains(&percent) && (percent - RENDER_SCALE_MIN) % RENDER_SCALE_STEP == 0
}

/// The slider's steps in order.
pub fn render_scale_steps() -> Vec<u32> {
    (RENDER_SCALE_MIN..=RENDER_SCALE_MAX).step_by(RENDER_SCALE_STEP as usize).collect()
}

/// Whether the integrated-GPU default applies: the GPU is integrated and the Render scale is still the default. A
/// stored scale that was changed (any value but 100%) is kept as the player chose it. Note that the stored value
/// cannot tell an untouched 100% from a chosen one, so a 100% chosen on an integrated GPU gets the same default. The
/// flag only drives the log line: the multiplier itself always climbs from [`INTEGRATED_DEFAULT_MULTIPLIER`] on an
/// integrated GPU, so the slider has no jump at 125%.
pub fn integrated_default_applies(render_scale: u32, integrated_gpu: bool) -> bool {
    integrated_gpu && render_scale == RENDER_SCALE_DEFAULT
}

/// The render plan for an output size, a render scale (percent) and a renderer. Pure. Discrete GPUs and every
/// changed scale use [`display_plan_on`] with `integrated_gpu` false.
///
/// Refused: a zero or out-of-range side, a ratio outside 1:2 .. 8:1 after turning a portrait input sideways, and a
/// render scale that is not a slider step.
pub fn display_plan(
    output_width: u32,
    output_height: u32,
    render_scale: u32,
    caps: RendererCaps,
) -> Result<DisplayPlan, PlanError> {
    display_plan_on(output_width, output_height, render_scale, caps, false)
}

/// [`display_plan`] for the GPU the game will use: on an integrated GPU the slider's baseline is
/// [`INTEGRATED_DEFAULT_MULTIPLIER`] instead of k (issue #89), and each 25% step adds one multiplier. Otherwise the plan is
/// the same as [`display_plan`].
pub fn display_plan_on(
    output_width: u32,
    output_height: u32,
    render_scale: u32,
    caps: RendererCaps,
    integrated_gpu: bool,
) -> Result<DisplayPlan, PlanError> {
    if !is_render_scale(render_scale) {
        return Err(PlanError(format!(
            "render scale {render_scale}% is not one of {RENDER_SCALE_MIN}% to {RENDER_SCALE_MAX}% in steps of {RENDER_SCALE_STEP}%"
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
    // TASK-118 (owner, 2026-10-26): the slider is additive. The plan at 100% is the baseline (on an integrated GPU that
    // is the 1x of issue #89, not k), and each 25% step adds or removes one whole multiplier. Before, a changed scale
    // multiplied k by the percentage: on the laptop 100% gave 1x and 125% gave 4x, a four-times jump for one notch.
    let integrated_default = integrated_default_applies(render_scale, integrated_gpu);
    let baseline = if integrated_gpu { INTEGRATED_DEFAULT_MULTIPLIER } else { base_multiplier };
    let steps = (render_scale as i32 - RENDER_SCALE_DEFAULT as i32) / RENDER_SCALE_STEP as i32;
    let multiplier = (baseline as i32 + steps).clamp(1, caps.max_multiplier.max(1) as i32) as u32;
    Ok(DisplayPlan {
        output_width: width,
        output_height: height,
        portrait_input,
        integrated_default,
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
            let result = plan(width, height, 100);
            assert_eq!(result.base_multiplier, k, "{width}x{height}");
            assert_eq!(result.multiplier, k, "{width}x{height} at 100%");
            assert!(640 * k >= width && 448 * k >= height, "never below the output on either axis");
        }
    }

    #[test]
    fn render_scale_moves_one_multiplier_per_step() {
        // Owner (2026-10-26): each 25% step is one whole multiplier from the 100% plan, never a multiplication of it.
        // A discrete 3x baseline: 50/75/100/125/150/175/200% -> 1/2/3/4/5/6/7x.
        let steps: Vec<u32> = render_scale_steps().iter().map(|&scale| plan(1920, 1080, scale).multiplier).collect();
        assert_eq!(steps, vec![1, 2, 3, 4, 5, 6, 7]);
        assert_eq!(describe_plan(&plan(1920, 1080, 125)), "4x \u{00B7} 2560x1792");
        // 2560x1440 has a 4x baseline: 100% stays 4x and one notch is 5x (the old formula gave 8x at 200%).
        assert_eq!(plan(2560, 1440, 75).multiplier, 3);
        assert_eq!(plan(2560, 1440, 100).multiplier, 4);
        assert_eq!(plan(2560, 1440, 125).multiplier, 5);
        assert_eq!(plan(2560, 1440, 200).multiplier, 8);
        // Clamped at the bottom (1x) and at the gt4-gs maximum (12x).
        assert_eq!(plan(640, 480, 50).multiplier, 1);
        assert_eq!(plan(3840, 2160, 200).multiplier, 10, "6x baseline + 4 steps");
        assert_eq!(plan(7680, 4320, 200).multiplier, 12, "12x baseline clamped");
        let small = RendererCaps { max_multiplier: 6 };
        assert_eq!(display_plan(3840, 2160, 200, small).unwrap().multiplier, 6);
    }

    #[test]
    fn integrated_gpu_climbs_from_1x_without_a_jump() {
        // issue #89: 100% on an integrated GPU is 1x. The slider used to jump to 4x at 125% (k 3 x 125%); it now climbs
        // one multiplier per step, so the laptop can try 2x and 3x.
        let integrated: Vec<u32> = render_scale_steps()
            .iter()
            .map(|&scale| display_plan_on(1366, 768, scale, PCSX2_GS_CAPS, true).unwrap().multiplier)
            .collect();
        assert_eq!(integrated, vec![1, 1, 1, 2, 3, 4, 5]);
        let plan_100 = display_plan_on(1366, 768, 100, PCSX2_GS_CAPS, true).unwrap();
        assert!(plan_100.integrated_default);
        assert_eq!((plan_100.base_multiplier, plan_100.multiplier), (3, 1));
        assert!(!display_plan_on(1366, 768, 125, PCSX2_GS_CAPS, true).unwrap().integrated_default);
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
        let phone = plan(3088, 1440, 100);
        assert_eq!((phone.base_multiplier, phone.internal_width, phone.internal_height), (5, 3200, 2240));
        assert!(phone.ultrawide_on && !phone.portrait_input);
        // The same phone reporting its natural (portrait) orientation: turned sideways, same plan.
        let portrait = plan(1440, 3088, 100);
        assert!(portrait.portrait_input);
        assert_eq!((portrait.output_width, portrait.output_height), (3088, 1440));
        assert_eq!(portrait.multiplier, phone.multiplier);
        assert_eq!(portrait.aspect, phone.aspect);
        let square = plan(1080, 1080, 100);
        assert_eq!(square.base_multiplier, 3);
        assert!(square.ultrawide_on);
    }

    #[test]
    fn invalid_inputs_are_refused() {
        assert!(display_plan(0, 1080, 100, PCSX2_GS_CAPS).is_err());
        assert!(display_plan(1920, 0, 100, PCSX2_GS_CAPS).is_err());
        assert!(display_plan(100, 100, 100, PCSX2_GS_CAPS).is_err(), "below 320");
        assert!(display_plan(20000, 2000, 100, PCSX2_GS_CAPS).is_err(), "above 16384");
        assert!(display_plan(16000, 1000, 100, PCSX2_GS_CAPS).is_err(), "ratio 16:1");
        for scale in [0, 49, 60, 101, 225, 1000] {
            assert!(display_plan(1920, 1080, scale, PCSX2_GS_CAPS).is_err(), "{scale}%");
        }
        assert_eq!(render_scale_steps(), vec![50, 75, 100, 125, 150, 175, 200]);
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

    // Issue #89 (1.3.0): integrated GPUs start at native resolution while the render scale is the untouched default.

    #[test]
    fn an_integrated_gpu_with_the_default_scale_renders_at_native_1x() {
        // 1440 x 900 is k 3 on a discrete card (1920 x 1344); the integrated default gives 1x = 640 x 448.
        let plan = display_plan_on(1440, 900, RENDER_SCALE_DEFAULT, PCSX2_GS_CAPS, true).unwrap();
        assert_eq!(plan.base_multiplier, 3);
        assert_eq!(plan.multiplier, INTEGRATED_DEFAULT_MULTIPLIER);
        assert_eq!((plan.internal_width, plan.internal_height), (640, 448));
        assert!(plan.integrated_default);
    }

    #[test]
    fn a_discrete_gpu_keeps_the_scaled_multiplier_at_the_default_scale() {
        let plan = display_plan_on(1440, 900, RENDER_SCALE_DEFAULT, PCSX2_GS_CAPS, false).unwrap();
        assert_eq!(plan.multiplier, 3);
        assert!(!plan.integrated_default);
        assert_eq!(plan, display_plan(1440, 900, RENDER_SCALE_DEFAULT, PCSX2_GS_CAPS).unwrap());
    }

    #[test]
    fn a_changed_render_scale_climbs_from_the_integrated_baseline() {
        // The player's choice wins, one multiplier per 25% step from the integrated 1x baseline (TASK-118, owner
        // 2026-10-26): 50/75% stay at 1x, 125% is 2x, 200% is 5x.
        for (scale, expected) in [(50, 1), (75, 1), (125, 2), (150, 3), (200, 5)] {
            let plan = display_plan_on(1440, 900, scale, PCSX2_GS_CAPS, true).unwrap();
            assert_eq!(plan.multiplier, expected, "{scale}%");
            assert!(!plan.integrated_default, "{scale}%");
        }
    }

    #[test]
    fn the_integrated_default_never_exceeds_the_renderer_cap() {
        let capped = RendererCaps { max_multiplier: 1 };
        assert_eq!(display_plan_on(1920, 1080, RENDER_SCALE_DEFAULT, capped, true).unwrap().multiplier, 1);
        let zero_cap = RendererCaps { max_multiplier: 0 };
        assert_eq!(display_plan_on(1920, 1080, RENDER_SCALE_DEFAULT, zero_cap, true).unwrap().multiplier, 1);
    }

    #[test]
    fn the_integrated_default_applies_only_to_the_default_scale_on_an_integrated_gpu() {
        assert!(integrated_default_applies(100, true));
        assert!(!integrated_default_applies(100, false));
        assert!(!integrated_default_applies(125, true));
        assert!(!integrated_default_applies(50, true));
    }
}
