//! Pure rules of the command layer: Settings rows, Controls labels, key names and the About text.
//! Nothing here touches the disk or a window, so every rule is tested without the UI.

use gt4controls::{Controls, Curve, Stick, TriggerTuning, BUTTON_INPUTS};
use gt4l_core::display::{self, Monitor};
use gt4l_core::gs_table;
use gt4l_core::language::Language;
use gt4l_core::settings::{Kind, Page, SettingDef, Settings, SETTINGS};
use serde::Serialize;
use serde_json::{json, Value};

// Settings table (the contract of the UI branch, ui/dev/README.md) ----------------------------------

/// The upscale limit sent until gt4-gs reports the device limit.
pub const MAX_UPSCALE_DEFAULT: u32 = 12;

/// Settings rows that are never sent to the UI: the present-mode rows the launcher derives from `vsync`
/// (scripts/gs_settings.py, gs_table::render_ini callers). `upscale` follows the Resolution and the Render scale
/// (TASK-080 addendum, `display::display_plan`). The GSdx Graphics API is `graphics_api` (TASK-103), offered in the UI.
const DERIVED_TABLE_IDS: &[&str] = &["vsync", "mailbox", "upscale"];

/// The rows of the PCSX2 GS table the UI may show: offered, not mods (release 1.1.0), not derived.
/// Launcher rows TASK-118 removed from the page: GS async is always on (it was a debugging A/B switch).
const HIDDEN_LAUNCHER_ROWS: &[&str] = &["async_gs"];
/// Launcher rows only release-candidate builds ship (the F7/F8 release report and the VU code check).
const RC_ONLY_ROWS: &[&str] = &["performance_report", "check_vu_code"];
/// The renderer defines the graphics options (owner, 2026-10-30): the table rows are GSdx's, and of them only these exist
/// under CoreGS 2.0 too (launch::coregs20_environment). `coregs_sharpening` (FSR1, where GSdx has CAS) exists only there.
const CORE_GS_20_TABLE_ROWS: &[&str] = &["performance_mode", "texture_load", "fxaa"];

pub fn pcsx2_table_rows() -> Vec<&'static gs_table::Row> {
    gs_table::offered().into_iter().filter(|row| !DERIVED_TABLE_IDS.contains(&row.id.as_str())).collect()
}

/// The payload of `get_settings_table`: `{max_upscale, rows}`. Launcher rows come from `SETTINGS`, PCSX2 GS rows from
/// the table file; both use the row shape of ui/dev/README.md. The table rows only GSdx honours show only under GSdx.
pub fn settings_table(settings: &Settings, controls: &Controls) -> Value {
    settings_table_with_monitors(settings, controls, &[], &[], false)
}

/// The Monitor row's choices: one per connected monitor, labelled by its name ("DELL U3419W"; a second one with the same
/// name is "DELL U3419W 2"), stored by identifier (`display::monitor_id`); without monitors, one "Primary monitor"
/// entry. `current` is the monitor the game would open on now (the primary one until another is chosen).
fn fill_monitor_row(row: &mut Value, settings: &Settings, monitors: &[Monitor]) {
    match display::resolve_monitor(monitors, settings.get("monitor")) {
        Some(chosen) => {
            let labels = display::monitor_labels(monitors);
            row["values"] = Value::Array(
                monitors
                    .iter()
                    .zip(labels)
                    .map(|(monitor, label)| json!({ "value": display::monitor_id(monitor), "label": label }))
                    .collect(),
            );
            row["current"] = json!(display::monitor_id(chosen));
        }
        None => {
            row["values"] = json!([{ "value": display::PRIMARY, "label": "Primary monitor" }]);
            row["current"] = json!(display::PRIMARY);
        }
    }
    row["default"] = json!(display::PRIMARY);
}

/// The Graphics API row (TASK-108): the Automatic choice says which API it resolves to for the card the run would use.
fn fill_api_row(row: &mut Value, settings: &Settings, cards: &[gt4l_core::gpu::GpuDevice]) {
    let plan = gt4l_core::gpu::plan(&settings.gs_value("graphics_api"), settings.get("graphics_card"), cards, Language::fallback());
    // TASK-118: no "Automatic" in the UI. The entry is dropped and, while the stored value is still automatic, the row
    // shows the API this start resolves to (Play then stores it).
    if let Some(values) = row["values"].as_array_mut() {
        values.retain(|value| value["value"] != gt4l_core::gpu::AUTOMATIC);
    }
    if settings.gs_value("graphics_api") == gt4l_core::gpu::AUTOMATIC {
        row["current"] = json!(plan.api);
    }
}

/// The Graphics card row (TASK-108): Automatic (best), then every card by name. A stored card that is not present is
/// listed too, so the page shows what is stored; the run then falls back to Automatic.
fn fill_card_row(row: &mut Value, settings: &Settings, cards: &[gt4l_core::gpu::GpuDevice]) {
    // TASK-118: no "Automatic": the list holds the cards present, and a stored card that is gone is kept so the page
    // still shows it (the run falls back and Play stores the card it used).
    let stored = settings.get("graphics_card").to_string();
    // CoreGS 2.0 draws with Vulkan: a card without it is not a choice there.
    let coregs20 = settings.get("renderer") == gt4l_core::settings::CORE_GS_20;
    let mut names: Vec<String> =
        cards.iter().filter(|card| !coregs20 || card.has_vulkan()).filter_map(|card| card.name().map(str::to_string)).collect();
    if stored != gt4l_core::gpu::AUTOMATIC && !stored.is_empty() && !names.contains(&stored) {
        names.push(stored.clone());
    }
    let values: Vec<Value> = names.iter().map(|name| json!({ "value": name, "label": name })).collect();
    row["values"] = Value::Array(values);
    let plan = gt4l_core::gpu::plan(&settings.gs_value("graphics_api"), &stored, cards, Language::fallback());
    let current = if stored == gt4l_core::gpu::AUTOMATIC {
        plan.card.as_ref().and_then(|card| card.name()).unwrap_or("").to_string()
    } else {
        stored
    };
    row["current"] = json!(current);
}

/// The Resolution row's choices: the chosen monitor's modes ("3440x1440 (desktop)" marks its desktop mode); without
/// monitors, one "Desktop resolution" entry. `current` is the output size the game would use now.
fn fill_resolution_row(row: &mut Value, settings: &Settings, monitors: &[Monitor]) {
    match display::resolve_monitor(monitors, settings.get("monitor")) {
        Some(monitor) => {
            row["values"] = Value::Array(
                monitor
                    .modes
                    .iter()
                    .map(|size| {
                        let label = if *size == monitor.desktop { format!("{} (desktop)", size.text()) } else { size.text() };
                        json!({ "value": size.text(), "label": label })
                    })
                    .collect(),
            );
            row["current"] = json!(display::resolve_resolution(monitor, settings.get("resolution")).text());
            // The default is the symbol "desktop": it means the monitor's desktop mode at each start, not a stored size.
            row["default"] = json!(display::DESKTOP);
        }
        None => {
            row["values"] = json!([{ "value": display::DESKTOP, "label": "Desktop resolution" }]);
            row["current"] = json!(display::DESKTOP);
            row["default"] = json!(display::DESKTOP);
        }
    }
}

/// The Render scale slider's readout per step: "100% · 4x · 2560x1792" for the output size the game would use now
/// (1920 x 1080 when no monitor is known).
fn fill_render_scale_row(row: &mut Value, settings: &Settings, monitors: &[Monitor], integrated_gpu: bool) {
    let output = display::resolve_target(monitors, settings.get("monitor"), settings.get("resolution"))
        .map(|target| target.output)
        .unwrap_or(display::FALLBACK_OUTPUT);
    let mut labels = serde_json::Map::new();
    for step in display::render_scale_steps() {
        if let Ok(plan) = display::display_plan_on(output.width, output.height, step, display::PCSX2_GS_CAPS, integrated_gpu) {
            labels.insert(step.to_string(), json!(format!("{step}% \u{00B7} {}", display::describe_plan(&plan))));
        }
    }
    row["values"]["labels"] = Value::Object(labels);
    row["values"]["output"] = json!(output.text());
}

/// The settings table with the connected monitors (TASK-080 addendum: Monitor, Resolution and Render scale).
/// `integrated_gpu` is true when the game renders on an integrated GPU (issue #89): the Render scale readout then shows
/// the untouched default at 1x.
pub fn settings_table_with_monitors(
    settings: &Settings,
    controls: &Controls,
    monitors: &[Monitor],
    cards: &[gt4l_core::gpu::GpuDevice],
    integrated_gpu: bool,
) -> Value {
    let mut rows: Vec<Value> = Vec::new();
    // TASK-118: the release performance report and the VU code check are release-candidate settings; final builds do
    // not ship them, so their rows do not exist there either.
    let release_candidate = gt4l_core::LAUNCHER_VERSION.contains("-rc");
    for definition in SETTINGS {
        // TASK-118: rows the owner removed outright (always on, or done by the preset/launcher).
        if HIDDEN_LAUNCHER_ROWS.contains(&definition.id) {
            continue;
        }
        if !release_candidate && RC_ONLY_ROWS.contains(&definition.id) {
            continue;
        }
        // TASK-108: the Graphics card row sits after the Graphics API row and the FSR1 row after CAS (added with the table
        // rows below).
        // TASK-082: Skip frames when slow sits after Performance Mode, under the renderer that has it.
        if definition.id == "graphics_card" || definition.id == "coregs_sharpening" || definition.id == "skip_frames_when_slow" {
            continue;
        }
        let mut row = launcher_row(definition, settings);
        // TASK-118: the release-candidate rows get their own section (RC Settings), drawn only by RC builds.
        if RC_ONLY_ROWS.contains(&definition.id) {
            row["group"] = json!("rc");
        }
        match definition.id {
            "monitor" => fill_monitor_row(&mut row, settings, monitors),
            "resolution" => fill_resolution_row(&mut row, settings, monitors),
            "render_scale" => fill_render_scale_row(&mut row, settings, monitors, integrated_gpu),
            "vsync" if settings.get("renderer") == gt4l_core::settings::CORE_GS_20 => without_fast_vsync(&mut row),
            _ => {}
        }
        rows.push(row);
    }
    for row in pcsx2_table_rows() {
        let mut out = gs_row(row, settings);
        if !CORE_GS_20_TABLE_ROWS.contains(&row.id.as_str()) {
            out["visible_when"] = json!({ "id": "renderer", "equals": gt4l_core::settings::PCSX2_GS });
        }
        // What Performance Mode does depends on the renderer.
        if row.id == "performance_mode" && settings.get("renderer") == gt4l_core::settings::CORE_GS_20 {
            out["help_key"] = json!("setting.performance_mode.help_coregs20");
        }
        if row.id == "performance_mode" {
            rows.push(out);
            if let Some(definition) = gt4l_core::settings::definition("skip_frames_when_slow") {
                let mut skip = launcher_row(definition, settings);
                skip["visible_when"] = json!({ "id": "renderer", "equals": gt4l_core::settings::CORE_GS_20 });
                rows.push(skip);
            }
            continue;
        }
        if row.id == "cas_sharpness" {
            rows.push(out);
            if let Some(definition) = gt4l_core::settings::definition("coregs_sharpening") {
                let mut sharpening = launcher_row(definition, settings);
                sharpening["visible_when"] = json!({ "id": "renderer", "equals": gt4l_core::settings::CORE_GS_20 });
                rows.push(sharpening);
            }
            continue;
        }
        if row.id == "graphics_api" {
            fill_api_row(&mut out, settings, cards);
            rows.push(out);
            if let Some(definition) = gt4l_core::settings::definition("graphics_card") {
                let mut card = launcher_row(definition, settings);
                fill_card_row(&mut card, settings, cards);
                rows.push(card);
            }
            continue;
        }
        rows.push(out);
    }
    for trigger in ["l2", "r2"] {
        rows.push(trigger_row(trigger, controls, "deadzone"));
        rows.push(trigger_row(trigger, controls, "curve"));
    }
    for stick in [Stick::Left, Stick::Right] {
        rows.push(stick_row(stick, controls));
    }
    json!({ "max_upscale": MAX_UPSCALE_DEFAULT, "rows": rows })
}

/// CoreGS 2.0 has no "Fast" vsync (owner, 2026-10-30). A value stored for GSdx stays stored and shows as On here, which is
/// how this renderer draws it (launch::coregs20_environment).
fn without_fast_vsync(row: &mut Value) {
    if let Some(values) = row["values"].as_array_mut() {
        values.retain(|value| value["value"] != "fast");
    }
    if row["current"] == "fast" {
        row["current"] = json!("on");
    }
    row["help_key"] = json!("setting.vsync.help_coregs20");
}

fn launcher_row(definition: &SettingDef, settings: &Settings) -> Value {
    let group = match definition.page {
        Page::General => "general",
        Page::Graphics => "graphics",
        Page::Display => "display",
        Page::Advanced => "",
    };
    let page = if definition.page == Page::Advanced { "advanced" } else { "basic" };
    let (control, values, default, current) = match definition.kind {
        Kind::Choice(choices) => (
            "dropdown",
            Value::Array(
                choices
                    .iter()
                    .map(|choice| json!({ "value": choice.value, "label": choice.label, "disabled": !choice.selectable }))
                    .collect(),
            ),
            json!(definition.default),
            json!(settings.get(definition.id)),
        ),
        Kind::Toggle => ("toggle", Value::Null, json!(definition.default == "on"), json!(settings.is_on(definition.id))),
        Kind::Percent => (
            "slider",
            json!({ "min": 0, "max": 100, "step": 1, "unit": "%" }),
            json!(definition.default.parse::<u32>().unwrap_or(0)),
            json!(settings.get(definition.id).parse::<u32>().unwrap_or(0)),
        ),
        // TASK-080 Render scale: a slider in steps; `labels` (filled by the table) gives the readout per step.
        Kind::Steps { min, max, step } => (
            "slider",
            json!({ "min": min, "max": max, "step": step, "unit": "%" }),
            json!(definition.default.parse::<u32>().unwrap_or(min)),
            json!(settings.get(definition.id).parse::<u32>().unwrap_or(min)),
        ),
        // TASK-080 Monitor and Resolution: dropdowns whose choices come from the connected monitors (filled by the
        // table, `fill_monitor_row` / `fill_resolution_row`).
        // TASK-102: the language choices are the languages of the locale files (Auto first).
        Kind::Language => (
            "dropdown",
            Value::Array(
                std::iter::once(json!({ "value": "auto", "label": "Auto" }))
                    .chain(gt4l_core::language::LANGUAGE_TAGS.iter().map(|(tag, name)| json!({ "value": tag, "label": name })))
                    .collect(),
            ),
            json!(definition.default),
            json!(settings.get(definition.id)),
        ),
        Kind::Monitor | Kind::Resolution | Kind::Gpu => (
            "dropdown",
            Value::Array(Vec::new()),
            json!(definition.default),
            json!(settings.get(definition.id)),
        ),
    };
    let mut row = json!({
        "id": definition.id,
        "label": definition.title,
        "help": definition.info,
        "page": page,
        "control": control,
        "values": values,
        "default": default,
        "current": current,
        "disabled": !settings.is_active(definition.id),
    });
    if !group.is_empty() {
        row["group"] = json!(group);
    }
    row
}

/// The two choices of a toggle over an enum row (table `control = "toggle"`): (Off, On). The first choice is Off.
pub fn toggle_choices(row: &gs_table::Row) -> Option<(&str, &str)> {
    if row.kind == "enum" && gs_table::ui_control(row) == "toggle" && row.choices.len() == 2 {
        Some((row.choices[0].name.as_str(), row.choices[1].name.as_str()))
    } else {
        None
    }
}

/// The table value text of a PCSX2 GS row from a UI value: a toggle over an enum takes true (On) or false (Off);
/// every other row takes its plain value.
pub fn gs_value_text(row: &gs_table::Row, value: &Value) -> Option<String> {
    match toggle_choices(row) {
        Some((off, on)) => value.as_bool().map(|is_on| (if is_on { on } else { off }).to_string()),
        None => value_text(value, false),
    }
}

fn gs_row(row: &gs_table::Row, settings: &Settings) -> Value {
    let toggle = toggle_choices(row);
    let control = match row.kind.as_str() {
        _ if toggle.is_some() => "toggle",
        "bool" => "toggle",
        "int" | "float" => "slider",
        _ => "dropdown",
    };
    let (values, default, current) = match (toggle, row.kind.as_str()) {
        (Some((_, on)), _) => (Value::Null, json!(row.default == on), json!(settings.gs_value(&row.id) == on)),
        (None, "bool") => (
            Value::Null,
            json!(row.default == "true"),
            json!(settings.gs_value(&row.id) == "true"),
        ),
        (None, "int" | "float") => {
            let mut bounds = json!({ "min": row.min.unwrap_or(0.0), "max": row.max.unwrap_or(0.0), "step": 1 });
            if row.id == "upscale" {
                bounds["max"] = json!(row.max.unwrap_or(0.0).min(MAX_UPSCALE_DEFAULT as f64));
                bounds["limit"] = json!("max_upscale");
            }
            let number = |text: &str| text.parse::<f64>().ok().map(|n| json!(n)).unwrap_or(Value::Null);
            (bounds, number(&row.default), number(&settings.gs_value(&row.id)))
        }
        (None, _) => (
            Value::Array(
                row.choices
                    .iter()
                    .map(|choice| {
                        // The table's English label when it has one; the UI translates the value by key (TASK-102).
                        let label = if choice.label.is_empty() { choice.name.as_str() } else { choice.label.as_str() };
                        json!({ "value": choice.name, "label": label, "disabled": choice.status != "ok" })
                    })
                    .collect(),
            ),
            json!(row.default),
            json!(settings.gs_value(&row.id)),
        ),
    };
    let mut out = json!({
        "id": row.id,
        "label": row.label,
        "help": row.help,
        "page": gs_table::ui_page(row),
        "control": control,
        "values": values,
        "default": default,
        "current": current,
    });
    match row.page.as_str() {
        "Graphics" => out["group"] = json!("graphics"),
        "Display" => out["group"] = json!("display"),
        _ => {}
    }
    out
}

fn trigger_row(trigger: &str, controls: &Controls, field: &str) -> Value {
    let tuning = controls.trigger_tuning(trigger);
    let label = if field == "deadzone" { "deadzone" } else { "curve" };
    let title = format!("{} {}", trigger.to_uppercase(), label);
    if field == "deadzone" {
        json!({
            "id": format!("{trigger}_deadzone"),
            "label": title,
            "help": "How far the trigger must travel before it counts, in percent of its travel. The pressure the game \
                     reads starts above this point.",
            "page": "basic",
            "group": "controls",
            "control": "slider",
            "values": { "min": 0, "max": 95, "step": 1, "unit": "%" },
            "default": percent(gt4controls::TRIGGER_DEFAULT_DEADZONE),
            "current": percent(tuning.deadzone),
        })
    } else {
        json!({
            "id": format!("{trigger}_curve"),
            "label": title,
            "help": "How the pressure grows with the trigger travel. Linear follows the trigger; quadratic and cubic \
                     give finer control at light pressure.",
            "page": "basic",
            "group": "controls",
            "control": "dropdown",
            "values": [
                { "value": "linear", "label": "Linear" },
                { "value": "quadratic", "label": "Quadratic" },
                { "value": "cubic", "label": "Cubic" },
                { "value": "s_curve", "label": "S-Curve" },
                { "value": "smooth_power", "label": "Smooth Power (x^1.5)" },
                { "value": "sublinear", "label": "Sublinear (sqrt)" },
                { "value": "logarithmic", "label": "Logarithmic" },
            ],
            "default": "linear",
            "current": tuning.curve.as_str(),
        })
    }
}

/// A fraction as a percentage with one decimal (0.1176 -> 11.8).
fn percent(fraction: f32) -> f64 {
    (f64::from(fraction) * 1000.0).round() / 10.0
}

/// The deadzone row of a stick (`lstick_deadzone`, `rstick_deadzone`).
fn stick_row(stick: Stick, controls: &Controls) -> Value {
    let (id, label, default) = match stick {
        Stick::Left => ("lstick_deadzone", "Left stick deadzone", gt4controls::LEFT_STICK_DEFAULT_DEADZONE),
        Stick::Right => ("rstick_deadzone", "Right stick deadzone", gt4controls::RIGHT_STICK_DEFAULT_DEADZONE),
    };
    json!({
        "id": id,
        "label": label,
        "help": "How far the stick must move from the centre before the game sees it, in percent of its travel. \
                 The default is the XInput standard; lower it for a precise pad, raise it for one that drifts.",
        "page": "basic",
        "group": "controls",
        "control": "slider",
        "values": { "min": 0, "max": 95, "step": 1, "unit": "%" },
        "default": percent(default),
        "current": percent(controls.stick_deadzone(stick)),
    })
}

/// The stick a `set_setting` id tunes (`lstick_deadzone`, `rstick_deadzone`), or `None`.
pub fn stick_setting(id: &str) -> Option<Stick> {
    match id {
        "lstick_deadzone" => Some(Stick::Left),
        "rstick_deadzone" => Some(Stick::Right),
        _ => None,
    }
}

/// The payload of `get_controls` (ui/dev/README.md): the sixteen buttons in drawing order, the analog rows (trigger
/// and stick settings, settings row shape) and whether the file is kept read-only (written by a newer launcher).
pub fn controls_view(settings: &Settings, controls: &Controls, locked: bool) -> Value {
    let rows: Vec<Value> = control_rows(controls)
        .into_iter()
        .map(|row| {
            json!({
                "id": row.input,
                "name": row.ps2_name,
                "binding": row.short,
                "sources": row.sources,
                "xinput": row.xinput,
                "key": row.key,
                "is_default": row.is_default,
            })
        })
        .collect();
    let table = settings_table(settings, controls);
    let analog: Vec<Value> = table["rows"]
        .as_array()
        .map(|rows| rows.iter().filter(|row| row["group"] == "controls").cloned().collect())
        .unwrap_or_default();
    json!({ "controls": rows, "triggers": analog, "locked": locked })
}

/// The text a `set_setting` value stands for: booleans as `on`/`off` for launcher toggles and `true`/`false` for the
/// table, numbers as their decimal text, strings as they are. Other JSON shapes are refused.
pub fn value_text(value: &Value, launcher_toggle: bool) -> Option<String> {
    match value {
        Value::Bool(flag) if launcher_toggle => Some(if *flag { "on" } else { "off" }.to_string()),
        Value::Bool(flag) => Some(flag.to_string()),
        Value::Number(number) => Some(number.to_string()),
        Value::String(text) => Some(text.clone()),
        _ => None,
    }
}

pub const CONTROL_ORDER: [&str; 16] = [
    "dpad_up", "dpad_right", "dpad_down", "dpad_left", "cross", "circle", "square", "triangle", "l1", "r1", "l2", "r2",
    "l3", "r3", "select", "start",
];

/// The PS2 name of a Controls-page input, as shown on the page.
pub fn ps2_name(input: &str) -> &'static str {
    match input {
        "dpad_up" => "D-pad up",
        "dpad_right" => "D-pad right",
        "dpad_down" => "D-pad down",
        "dpad_left" => "D-pad left",
        "cross" => "Cross",
        "circle" => "Circle",
        "square" => "Square",
        "triangle" => "Triangle",
        "l1" => "L1",
        "r1" => "R1",
        "l2" => "L2",
        "r2" => "R2",
        "l3" => "L3",
        "r3" => "R3",
        "select" => "Select",
        _ => "Start",
    }
}

/// A binding as the user reads it: `Controller: A`, `Keyboard: Z`, or the raw text otherwise.
pub fn describe_source(source: &str) -> String {
    let base = source.split(" | ").next().unwrap_or(source);
    match base.split_once(':') {
        Some(("xinput", name)) => format!("Controller: {}", xinput_name(name)),
        Some(("key", name)) => format!("Keyboard: {name}"),
        _ => base.to_string(),
    }
}

fn xinput_name(name: &str) -> &str {
    match name {
        "LThumb" => "left stick click",
        "RThumb" => "right stick click",
        other => other,
    }
}

/// The short caption on a control of the drawing.
pub fn short_label(sources: &[String]) -> String {
    let Some(first) = sources.first() else {
        return "-".to_string();
    };
    let name = first.split_once(':').map_or(first.as_str(), |(_, name)| name);
    match name {
        "LThumb" => "L3".into(),
        "RThumb" => "R3".into(),
        "Back" => "Select".into(),
        "DpadUp" => "Up".into(),
        "DpadDown" => "Down".into(),
        "DpadLeft" => "Left".into(),
        "DpadRight" => "Right".into(),
        "Backspace" => "Bksp".into(),
        other => other.into(),
    }
}

/// Maps the key text the page sends to a key name the controls file accepts, or `None`.
pub fn key_name(text: &str) -> Option<String> {
    let mut characters = text.chars();
    if let (Some(character), None) = (characters.next(), characters.next()) {
        if character.is_ascii_alphanumeric() {
            return Some(character.to_ascii_uppercase().to_string());
        }
    }
    match text {
        "Enter" | "Backspace" | "Tab" | "Space" | "Up" | "Down" | "Left" | "Right" => Some(text.to_string()),
        _ => None,
    }
}

#[derive(Clone, Debug, PartialEq, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct ControlDto {
    pub input: String,
    pub ps2_name: String,
    /// Caption printed on the drawing.
    pub short: String,
    /// Raw sources, e.g. `xinput:A`, `key:Z`.
    pub sources: Vec<String>,
    /// Sources as the user reads them.
    pub described: Vec<String>,
    /// The first XInput source without its prefix and options (`A`, `LT`), or `None` when no controller input drives it.
    pub xinput: Option<String>,
    /// The first keyboard key (`Z`, `Enter`), or `None`.
    pub key: Option<String>,
    /// True when the input has exactly its built-in sources.
    pub is_default: bool,
}

/// The name after the family prefix of the first source of `family`, without options.
fn first_of_family(sources: &[String], family: &str) -> Option<String> {
    sources
        .iter()
        .find(|source| gt4controls::source_family(source) == family)
        .map(|source| gt4controls::source_base(source).split_once(':').map_or("", |(_, name)| name).to_string())
}

/// Controls-page rows in display order.
pub fn control_rows(controls: &Controls) -> Vec<ControlDto> {
    CONTROL_ORDER
        .iter()
        .map(|input| {
            let sources: Vec<String> = controls.sources(input).unwrap_or(&[]).to_vec();
            ControlDto {
                input: input.to_string(),
                ps2_name: ps2_name(input).to_string(),
                short: short_label(&sources),
                described: sources.iter().map(|source| describe_source(source)).collect(),
                xinput: first_of_family(&sources, "xinput"),
                key: first_of_family(&sources, "key"),
                is_default: controls.is_default(input),
                sources,
            }
        })
        .collect()
}

#[derive(Clone, Debug, PartialEq, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct TriggerDto {
    /// `l2` or `r2`.
    pub id: String,
    /// Deadzone in percent of travel, 0-100.
    pub deadzone: f32,
    /// `linear`, `quadratic`, `cubic`, `s_curve`, `smooth_power`, `sublinear` or `logarithmic`.
    pub curve: String,
}

pub fn trigger_rows(controls: &Controls) -> Vec<TriggerDto> {
    ["l2", "r2"]
        .iter()
        .map(|id| {
            let tuning = controls.trigger_tuning(id);
            TriggerDto {
                id: id.to_string(),
                deadzone: (tuning.deadzone * 100.0 * 10.0).round() / 10.0,
                curve: tuning.curve.as_str().to_string(),
            }
        })
        .collect()
}

/// Parses the curve name the page sends.
pub fn curve_from_name(name: &str) -> Option<Curve> {
    match name {
        "linear" => Some(Curve::Linear),
        "quadratic" => Some(Curve::Quadratic),
        "cubic" => Some(Curve::Cubic),
        "s_curve" | "scurve" | "s-curve" => Some(Curve::SCurve),
        "smooth_power" | "smooth" => Some(Curve::SmoothPower),
        "sublinear" | "root" => Some(Curve::Sublinear),
        "logarithmic" | "log" => Some(Curve::Logarithmic),
        _ => None,
    }
}

/// Trigger tuning from a percentage and a curve name. Percentages outside 0-100 are refused.
pub fn trigger_tuning(deadzone_percent: f32, curve: &str) -> Result<TriggerTuning, String> {
    if !(0.0..=100.0).contains(&deadzone_percent) {
        return Err("the deadzone must be between 0 and 100 percent".into());
    }
    let curve = curve_from_name(curve).ok_or_else(|| format!("unknown curve '{curve}'"))?;
    Ok(TriggerTuning { deadzone: deadzone_percent / 100.0, curve })
}

/// Whether an input is one the page may rebind (the digital inputs of the controls file).
pub fn bindable(input: &str) -> bool {
    BUTTON_INPUTS.contains(&input)
}

/// Text of the About page. The trademark notice matches NOTICE.md and the installer.
pub fn about_text(version: &str) -> String {
    format!(
        "GT4Recomp {version}\n\n\
         GT4Recomp is an unofficial fan project. It is not affiliated with, endorsed by or sponsored by \
         Sony Interactive Entertainment Inc. or Polyphony Digital Inc.\n\n\
         \"Gran Turismo\", \"GT\", the Gran Turismo 4 logo, the GT emblem and the Gran Turismo 4 cover \
         artwork are trademarks or copyrighted works of Sony Interactive Entertainment Inc. and/or Polyphony \
         Digital Inc. They are used here only to identify the game. No game data is included in the source \
         repository. You need your own copy of Gran Turismo 4 (USA, v2.00), or of Gran Turismo 4 Spec II 1.10 for \
         that game, to play."
    )
}

/// The credits shown on the About page: the Spec II mod and its own credit list (owner decision 2026-10-23: no
/// permission is needed; credit the authors properly and ship none of their files).
pub fn credits_text() -> &'static str {
    "Gran Turismo 4 Spec II by TheAdmiester and contributors - https://www.theadmiester.co.uk/specii/\n\n\
     Credits: Nenkai (GT4FS, GTAdhocToolchain, OpenAdhoc, file formats, reverse engineering); ddm (on-screen keyboard, \
     randomizer algorithm, event theme suggestions); ermaccer (PS2PluginInjector, code assistance); zinklesmesh (car \
     camera adjustments); pez2k (GT4FS, car specifications, GT research); xfileFIN (GT4FS); Silentwarrior112 (car colour \
     patch editor); Xenn and Submaniac (ENGN research and sound documentation); Silent (GT3 chase cam, trigger \
     sensitivity, 480p PCSX2 cheats); Alconautica, hydnhntr, JeyM, pioneerspine (UI graphics and layout).\n\n\
     Special thanks: DustinEden (Spec II 100% livestream); testers Alconautica, Alex, Carver6K, ddm, Greg Thomas, Nebu, \
     Rekoware, Romrolls, Silent, Submaniac; translators granturismo.italia, Gustingorriz, Submaniac, SWYD1.\n\n\
     GT4Recomp prepares Spec II from your own disc image and ships none of its files."
}

/// Open-source notices of the launcher's user interface. Full texts are installed with the program.
pub fn licenses_text() -> &'static str {
    "GT4Recomp is MIT licensed.\n\n\
     The launcher interface is built with Tauri 2 (MIT or Apache-2.0) and runs in Microsoft Edge WebView2, \
     the Windows system web view (Microsoft WebView2 terms).\n\n\
     Typefaces: Roboto and Roboto Condensed (Apache-2.0), and Michroma (SIL Open Font License 1.1, \
     Copyright 2011 The Michroma Project Authors), bundled in the launcher.\n\n\
     The Ko-fi cup on the home screen is Ko-fi's brand asset, used to link to the project's Ko-fi page.\n\n\
     GSdx is the PCSX2 GS renderer of the PCSX2 project (https://pcsx2.net), licensed GPL-3.0-or-later. It is the \
     gt4-gs.exe program, downloaded only when it is selected, and it runs in its own process; it is not part of \
     GT4Recomp's MIT code. Its source and licence are in the gt4-gs release.\n\n\
     The launcher uses the Rust crates listed in launcher/Cargo.lock. paraLLEl-GS (LGPL-3.0-or-later) is \
     a separate library and is not part of the launcher's interface."
}

#[cfg(test)]
mod tests {
    use super::*;


    #[test]
    fn controls_rows_cover_the_sixteen_digital_inputs_with_names_and_bindings() {
        let rows = control_rows(&Controls::default());
        assert_eq!(rows.len(), 16);
        for input in BUTTON_INPUTS {
            assert!(CONTROL_ORDER.contains(input), "{input} is missing from the page");
        }
        let cross = rows.iter().find(|row| row.input == "cross").unwrap();
        assert_eq!(cross.ps2_name, "Cross");
        assert_eq!(cross.short, "A");
        assert!(cross.described.contains(&"Controller: A".to_string()));
        assert!(cross.described.contains(&"Keyboard: W".to_string()), "{:?}", cross.described);
        let l2 = rows.iter().find(|row| row.input == "l2").unwrap();
        assert_eq!(l2.ps2_name, "L2");
        assert_eq!(cross.xinput.as_deref(), Some("A"));
        assert_eq!(cross.key.as_deref(), Some("W"));
        assert!(cross.is_default);
    }

    #[test]
    fn trigger_tuning_checks_the_percentage_and_the_curve() {
        assert!(trigger_tuning(-1.0, "linear").is_err());
        assert!(trigger_tuning(101.0, "linear").is_err());
        assert!(trigger_tuning(10.0, "exponential").is_err());
        let tuning = trigger_tuning(20.0, "cubic").unwrap();
        assert!((tuning.deadzone - 0.2).abs() < 1e-6);
        assert_eq!(tuning.curve, Curve::Cubic);
        let rows = trigger_rows(&Controls::default());
        assert_eq!(rows[0].id, "l2");
        assert!((rows[0].deadzone - 11.8).abs() < 0.05, "default 30 of 255 is 11.8 percent");
    }

    #[test]
    fn all_seven_curves_parse_and_serialize_in_tauri_model() {
        let expected = [
            ("linear", Curve::Linear),
            ("quadratic", Curve::Quadratic),
            ("cubic", Curve::Cubic),
            ("s_curve", Curve::SCurve),
            ("smooth_power", Curve::SmoothPower),
            ("sublinear", Curve::Sublinear),
            ("logarithmic", Curve::Logarithmic),
        ];
        for (name, curve) in expected {
            let tuning = trigger_tuning(15.0, name).unwrap();
            assert_eq!(tuning.curve, curve);
            let mut controls = Controls::default();
            controls.set_trigger_tuning("l2", tuning).unwrap();
            let rows = trigger_rows(&controls);
            assert_eq!(rows[0].curve, curve.as_str());
        }
    }

    #[test]
    fn key_names_follow_the_controls_file() {
        assert_eq!(key_name("z").as_deref(), Some("Z"));
        assert_eq!(key_name("7").as_deref(), Some("7"));
        assert_eq!(key_name("Enter").as_deref(), Some("Enter"));
        assert_eq!(key_name("F5"), None, "function keys are not offered for binding");
    }

    #[test]
    fn short_labels_and_descriptions_are_readable() {
        assert_eq!(short_label(&["xinput:LThumb".to_string()]), "L3");
        assert_eq!(short_label(&[]), "-");
        assert_eq!(describe_source("xinput:LThumb"), "Controller: left stick click");
        assert_eq!(describe_source("key:Enter | repeat"), "Keyboard: Enter");
    }

    #[test]
    fn about_text_carries_the_trademark_notice_and_the_ui_licenses() {
        let text = about_text("1.0.0");
        assert!(text.starts_with("GT4Recomp 1.0.0"));
        assert!(text.contains("not affiliated with, endorsed by or sponsored by"));
        assert!(licenses_text().contains("Tauri 2") && !licenses_text().contains("Slint"));
    }

    #[test]
    fn credits_name_the_spec2_authors_with_their_link_and_credit_list() {
        let text = credits_text();
        assert!(text.contains("TheAdmiester and contributors"));
        assert!(text.contains("https://www.theadmiester.co.uk/specii/"));
        for name in ["Nenkai", "ddm", "ermaccer", "pez2k", "DustinEden", "granturismo.italia"] {
            assert!(text.contains(name), "{name} is missing");
        }
        assert!(!text.contains('['), "no placeholder may remain");
        assert!(text.contains("ships none of its files"));
    }

}

#[cfg(test)]
mod table_tests {
    use super::*;

    fn ids(table: &Value) -> Vec<String> {
        table["rows"].as_array().unwrap().iter().map(|row| row["id"].as_str().unwrap().to_string()).collect()
    }

    fn row<'a>(table: &'a Value, id: &str) -> &'a Value {
        table["rows"].as_array().unwrap().iter().find(|row| row["id"] == id).unwrap()
    }

    fn monitor(device: &str, name: &str, primary: bool, desktop: display::Size, raw: &[display::Size]) -> Monitor {
        Monitor {
            device: device.into(),
            name: name.into(),
            primary,
            desktop,
            modes: display::tidy_modes(raw, desktop),
        }
    }

    fn two_monitors() -> Vec<Monitor> {
        use display::Size;
        vec![
            monitor(r"\\.\DISPLAY1", "TV", true, Size::new(1920, 1080), &[Size::new(1280, 720), Size::new(1920, 1080)]),
            monitor(
                r"\\.\DISPLAY2",
                "DELL U3419W",
                false,
                Size::new(3440, 1440),
                &[Size::new(2560, 1080), Size::new(3440, 1440), Size::new(1920, 1080)],
            ),
        ]
    }

    #[test]
    fn no_aspect_or_internal_resolution_rows_are_offered() {
        let listed = ids(&settings_table(&Settings::default(), &Controls::default()));
        for gone in ["aspect_ratio", "aspect_custom", "upscale", "aspect"] {
            assert!(!listed.contains(&gone.to_string()), "{gone} must not be offered");
        }
        for present in ["monitor", "resolution", "render_scale", "fullscreen", "vsync"] {
            assert!(listed.contains(&present.to_string()), "{present} is offered");
        }
    }

    #[test]
    fn monitor_and_resolution_list_the_connected_monitors_and_their_modes() {
        let monitors = two_monitors();
        let mut settings = Settings::default();
        let table = settings_table_with_monitors(&settings, &Controls::default(), &monitors, &[], false);
        let monitor_row = row(&table, "monitor");
        assert_eq!(monitor_row["group"], "display");
        assert_eq!(monitor_row["control"], "dropdown");
        assert_eq!(monitor_row["current"], r"\\.\DISPLAY1@1920x1080", "the primary monitor by default");
        let labels: Vec<&str> = monitor_row["values"].as_array().unwrap().iter().map(|v| v["label"].as_str().unwrap()).collect();
        // TASK-088 (1.2.0): the friendly name only; no resolution and no "(primary)".
        assert_eq!(labels, ["TV", "DELL U3419W"]);
        let resolution = row(&table, "resolution");
        assert_eq!(resolution["current"], "1920x1080");
        // TASK-088 (1.2.0): the default is the symbol "desktop" (the monitor's desktop mode at each start).
        assert_eq!(resolution["default"], "desktop");
        let modes: Vec<&str> = resolution["values"].as_array().unwrap().iter().map(|v| v["value"].as_str().unwrap()).collect();
        assert_eq!(modes, ["1920x1080", "1280x720"]);

        settings.set("monitor", r"\\.\DISPLAY2@3440x1440").unwrap();
        settings.set("resolution", "2560x1080").unwrap();
        let table = settings_table_with_monitors(&settings, &Controls::default(), &monitors, &[], false);
        assert_eq!(row(&table, "monitor")["current"], r"\\.\DISPLAY2@3440x1440");
        let resolution = row(&table, "resolution");
        assert_eq!(resolution["current"], "2560x1080");
        assert_eq!(resolution["values"][0]["label"], "3440x1440 (desktop)");
        // The render scale readout follows the chosen output: 2560x1080 gives k = 4.
        let scale = row(&table, "render_scale");
        assert_eq!(scale["values"]["labels"]["100"], "100% \u{00B7} 4x \u{00B7} 2560x1792");
        assert_eq!(scale["values"]["labels"]["50"], "50% \u{00B7} 2x \u{00B7} 1280x896");
        assert_eq!(scale["values"]["labels"]["200"], "200% \u{00B7} 8x \u{00B7} 5120x3584");
    }

    #[test]
    fn an_integrated_gpu_reads_the_default_render_scale_at_1x() {
        // Issue #89: the readout shows the plan the game gets; the slider is additive (TASK-118), so 150% from the
        // integrated 1x baseline is 3x, not 5x.
        let table = settings_table_with_monitors(&Settings::default(), &Controls::default(), &[], &[], true);
        let scale = row(&table, "render_scale");
        assert_eq!(scale["values"]["labels"]["100"], "100% \u{00B7} 1x \u{00B7} 640x448");
        assert_eq!(scale["values"]["labels"]["150"], "150% \u{00B7} 3x \u{00B7} 1920x1344");
    }

    #[test]
    fn without_monitors_the_rows_fall_back_to_primary_and_desktop() {
        let table = settings_table(&Settings::default(), &Controls::default());
        assert_eq!(row(&table, "monitor")["current"], "primary");
        assert_eq!(row(&table, "resolution")["current"], "desktop");
        let scale = row(&table, "render_scale");
        assert_eq!(scale["control"], "slider");
        assert_eq!((scale["values"]["min"].as_u64(), scale["values"]["max"].as_u64(), scale["values"]["step"].as_u64()),
                   (Some(50), Some(200), Some(25)));
        assert_eq!(scale["current"], 100);
        assert_eq!(scale["values"]["labels"]["100"], "100% \u{00B7} 3x \u{00B7} 1920x1344");
        assert!(scale.get("visible_when").is_none(), "shown under both renderers");
        assert_eq!(scale["group"], "graphics");
    }

    #[test]
    fn performance_mode_is_a_graphics_toggle_off_by_default() {
        let table = settings_table(&Settings::default(), &Controls::default());
        let mode = row(&table, "performance_mode");
        assert_eq!(mode["control"], "toggle");
        assert_eq!(mode["page"], "basic", "the main Graphics section, not Advanced");
        assert_eq!(mode["group"], Value::Null);
        assert_eq!(mode["default"], json!(false));
        assert_eq!(mode["current"], json!(false));
        assert!(mode.get("disabled").is_none(), "a stored renderer never disables it");
        assert!(mode.get("visible_when").is_none(), "shown under both renderers");
    }

    #[test]
    fn skip_frames_when_slow_is_a_coregs_toggle_off_by_default_after_performance_mode() {
        let table = settings_table(&Settings::default(), &Controls::default());
        let skip = row(&table, "skip_frames_when_slow");
        assert_eq!(skip["control"], "toggle");
        assert_eq!(skip["page"], "basic", "the main Graphics section, not Advanced");
        assert_eq!(skip["default"], json!(false));
        assert_eq!(skip["current"], json!(false));
        assert_eq!(skip["visible_when"]["equals"], gt4l_core::settings::CORE_GS_20);
        let order = ids(&table);
        let mode = order.iter().position(|id| id == "performance_mode").unwrap();
        assert_eq!(order[mode + 1], "skip_frames_when_slow");
    }

    #[test]
    fn the_readback_row_is_gone_from_the_table() {
        let table = settings_table(&Settings::default(), &Controls::default());
        assert!(!ids(&table).contains(&"hw_download_mode".to_string()));
        assert!(!pcsx2_table_rows().iter().any(|row| row.id == "hw_download_mode"));
        assert!(!gs_table::rows().iter().any(|row| row.key == "HWDownloadMode" && row.id != "performance_mode"));
    }

    #[test]
    fn the_toggle_maps_on_and_off_to_the_two_readback_choices() {
        let row = pcsx2_table_rows().into_iter().find(|row| row.id == "performance_mode").unwrap();
        assert_eq!(toggle_choices(row), Some(("accurate", "no-readbacks")));
        assert_eq!(gs_value_text(row, &json!(true)).as_deref(), Some("no-readbacks"), "On");
        assert_eq!(gs_value_text(row, &json!(false)).as_deref(), Some("accurate"), "Off");
        assert_eq!(gs_value_text(row, &json!("accurate")), None, "a toggle takes true or false");
        let other = pcsx2_table_rows().into_iter().find(|row| row.id == "fxaa").unwrap();
        assert_eq!(toggle_choices(other), None);
    }

    #[test]
    fn renderer_dropdown_offers_gsdx_and_coregs20_beta() {
        let table = settings_table(&Settings::default(), &Controls::default());
        let renderer = row(&table, "renderer");
        assert_eq!(renderer["current"], "pcsx2");
        assert_eq!(renderer["control"], "dropdown");
        let values = renderer["values"].as_array().unwrap();
        assert_eq!(values.len(), 2, "GSdx and CoreGS 2.0 only; CoreGS 1.1 is gone (TASK-096)");
        assert_eq!(values[1]["value"], "coregs20");
        assert_eq!(values[1]["disabled"], false);
        assert_eq!(values[1]["label"], "CoreGS 2.0 (beta)");
        for shared in CORE_GS_20_TABLE_ROWS {
            assert!(row(&table, shared).get("visible_when").is_none(), "{shared} works under both renderers");
        }
        for gsdx_only in ["graphics_api", "cas_mode", "cas_sharpness", "perf_overlay"] {
            assert_eq!(row(&table, gsdx_only)["visible_when"]["equals"], "pcsx2", "{gsdx_only} is for GSdx");
        }
        assert_eq!(row(&table, "coregs_sharpening")["visible_when"]["equals"], "coregs20", "FSR1 is for CoreGS 2.0");
        for both in ["renderer", "render_scale", "graphics_card", "monitor", "resolution", "fullscreen", "vsync"] {
            assert!(row(&table, both).get("visible_when").is_none(), "{both} works under both renderers");
        }
        assert!(row(&table, "performance_mode").get("help_key").is_none());
        let mut chosen = Settings::default();
        chosen.set("renderer", "coregs20").unwrap();
        let under_coregs = settings_table(&chosen, &Controls::default());
        assert_eq!(row(&under_coregs, "performance_mode")["help_key"], "setting.performance_mode.help_coregs20");
        let vsync_values = |table: &Value| -> Vec<String> {
            row(table, "vsync")["values"].as_array().unwrap().iter().map(|value| value["value"].as_str().unwrap().to_string()).collect()
        };
        assert_eq!(vsync_values(&table), ["on", "fast", "off"], "GSdx keeps Fast");
        assert_eq!(vsync_values(&under_coregs), ["on", "off"], "CoreGS 2.0 has no Fast");
        chosen.set("vsync", "fast").unwrap();
        let stored_fast = settings_table(&chosen, &Controls::default());
        assert_eq!(row(&stored_fast, "vsync")["current"], "on", "a stored Fast shows as On under CoreGS 2.0");
        chosen.set("renderer", "pcsx2").unwrap();
        assert_eq!(row(&settings_table(&chosen, &Controls::default()), "vsync")["current"], "fast", "and is still there for GSdx");
        let listed = ids(&table);
        for gone in ["internal_scale", "present_filter", "fsr_sharpness", "deinterlacing", "anti_aliasing"] {
            assert!(!listed.contains(&gone.to_string()), "{gone} was a CoreGS 1.1 row");
        }
    }

    #[test]
    fn graphics_api_dropdown_is_on_the_graphics_page_with_three_apis() {
        let table = settings_table(&Settings::default(), &Controls::default());
        let api = row(&table, "graphics_api");
        assert_eq!(api["page"], "basic");
        assert_eq!(api["control"], "dropdown");
        assert_eq!(api["default"], "auto");
        assert!(
            matches!(api["current"].as_str(), Some("vulkan") | Some("d3d12") | Some("d3d11")),
            "TASK-118: the row shows the API this start resolves to, not Automatic"
        );
        let values = api["values"].as_array().unwrap();
        let shown: Vec<(&str, &str)> =
            values.iter().map(|value| (value["value"].as_str().unwrap(), value["label"].as_str().unwrap())).collect();
        assert_eq!(
            shown,
            vec![
                ("vulkan", "Vulkan"),
                ("d3d12", "Direct3D 12"),
                ("d3d11", "Direct3D 11"),
            ]
        );
        assert!(values.iter().all(|value| value["disabled"] == false));
        // The choice is stored and shown back (the engine row keeps its own id, `renderer`).
        let mut chosen = Settings::default();
        chosen.set_gs("graphics_api", "d3d11").unwrap();
        let table = settings_table(&chosen, &Controls::default());
        assert_eq!(row(&table, "graphics_api")["current"], "d3d11");
        assert!(chosen.set_gs("graphics_api", "opengl").is_err(), "OpenGL is not offered yet");
    }

    #[test]
    fn a_stored_coregs11_renderer_is_not_a_choice_any_more() {
        let mut settings = Settings::default();
        assert!(settings.set("renderer", "coregs11").is_err());
        assert_eq!(settings.get("renderer"), "pcsx2");
    }

    #[test]
    fn mods_rows_are_never_offered_in_release_1_0_0() {
        let table = settings_table(&Settings::default(), &Controls::default());
        for row in table["rows"].as_array().unwrap() {
            assert_ne!(row["page"], "mods");
        }
        assert!(pcsx2_table_rows().iter().all(|row| row.page != "mods"));
    }

    #[test]
    fn pcsx2_rows_are_shown_without_a_renderer_rule() {
        let table = settings_table(&Settings::default(), &Controls::default());
        let fxaa = row(&table, "fxaa");
        assert!(fxaa.get("visible_when").is_none(), "shown under both renderers");
        assert_eq!(table["max_upscale"], MAX_UPSCALE_DEFAULT);
        assert!(!pcsx2_table_rows().iter().any(|row| row.id == "upscale"), "the internal resolution is derived");
    }

    #[test]
    fn derived_rows_and_the_renderer_api_are_not_offered_as_pcsx2_rows() {
        let listed = ids(&settings_table(&Settings::default(), &Controls::default()));
        assert_eq!(listed.iter().filter(|id| *id == "vsync").count(), 1, "vsync is the launcher row only");
        assert!(!pcsx2_table_rows().iter().any(|row| row.id == "mailbox"));
    }

    #[test]
    fn triggers_use_the_row_shape_with_their_ids() {
        let table = settings_table(&Settings::default(), &Controls::default());
        assert_eq!(row(&table, "l2_deadzone")["control"], "slider");
        assert_eq!(row(&table, "r2_curve")["control"], "dropdown");
        assert_eq!(row(&table, "l2_deadzone")["default"], json!(11.8), "30 of 255");
        assert_eq!(row(&table, "lstick_deadzone")["current"], json!(24.0), "7849 of 32767");
        assert_eq!(row(&table, "rstick_deadzone")["default"], json!(26.5), "8689 of 32767");
    }

    #[test]
    fn controls_view_lists_buttons_with_both_sources_and_the_analog_rows() {
        let mut controls = Controls::default();
        controls.assign("cross", "xinput:B", true).unwrap();
        let view = controls_view(&Settings::default(), &controls, false);
        let buttons = view["controls"].as_array().unwrap();
        assert_eq!(buttons.len(), 16);
        let cross = buttons.iter().find(|button| button["id"] == "cross").unwrap();
        assert_eq!(cross["xinput"], "B");
        assert_eq!(cross["key"], "W");
        assert_eq!(cross["is_default"], false);
        let circle = buttons.iter().find(|button| button["id"] == "circle").unwrap();
        assert_eq!(circle["xinput"], "A", "swapped");
        let analog: Vec<&str> = view["triggers"].as_array().unwrap().iter().map(|row| row["id"].as_str().unwrap()).collect();
        assert_eq!(analog, ["l2_deadzone", "l2_curve", "r2_deadzone", "r2_curve", "lstick_deadzone", "rstick_deadzone"]);
        assert_eq!(view["locked"], false);
        assert_eq!(stick_setting("rstick_deadzone"), Some(Stick::Right));
        assert_eq!(stick_setting("l2_deadzone"), None);
    }

    #[test]
    fn value_text_maps_booleans_per_kind() {
        assert_eq!(value_text(&json!(true), true).as_deref(), Some("on"));
        assert_eq!(value_text(&json!(true), false).as_deref(), Some("true"));
        assert_eq!(value_text(&json!(12), false).as_deref(), Some("12"));
        assert_eq!(value_text(&json!(["x"]), false), None);
    }
}
