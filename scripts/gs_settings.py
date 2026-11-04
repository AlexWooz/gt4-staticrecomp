"""TASK-039: the GS settings table, gt4-gs.ini generator and allow-list (docs/GS-SETTINGS.md).

docs/launcher/gs-settings.toml is the single source. This module:
- loads and validates the table (schema, unique ids and INI keys, ranges, statuses);
- parses user values (refusing unknown ids, unknown choices and out-of-range values);
- renders gt4-gs.ini text (PCSX2 INI syntax) in a fixed section order;
- writes the file atomically (temporary file in the same folder, then os.replace);
- emits the allow-list of INI section/key pairs and their accepted values (docs/launcher/gs-settings-allowlist.json),
  which gt4-gs.exe embeds at build time (gt4-gs 1.0.0 ignores, with one warning line each, keys and values outside it);
- maps the internal-resolution presets to the monitor height and the GPU texture limit (for the launcher, TASK-019).

Command line (repository root):
  python scripts/gs_settings.py check                 validate the table
  python scripts/gs_settings.py allowlist [--write]   print or write the allow-list JSON
  python scripts/gs_settings.py ini [--set ID=VALUE] [--out PATH]
"""
import argparse
import json
import os
from pathlib import Path
import re
import sys
import tempfile
import tomllib

ROOT = Path(__file__).resolve().parents[1]
TABLE_PATH = ROOT / 'docs/launcher/gs-settings.toml'
ALLOWLIST_PATH = ROOT / 'docs/launcher/gs-settings-allowlist.json'
SCHEMA_VERSION = 1
PAGES = ('basic', 'advanced', 'mods', 'hidden')
TYPES = ('bool', 'int', 'float', 'enum', 'string', 'path')
CONTROLS = ('slider', 'toggle', 'dropdown', 'text', 'none')
CONTROL_FOR_TYPE = {'bool': 'toggle', 'int': 'slider', 'float': 'slider', 'enum': 'dropdown', 'string': 'text',
                    'path': 'none'}
WRITTEN = ('always', 'when-set', 'launcher', 'never')
STATUS_PATTERN = re.compile(r'ok|resolved|TBD-fork|pending-TASK-\d{3}')
ID_PATTERN = re.compile(r'[a-z][a-z0-9_]*')
ENV_PATTERN = re.compile(r'GT4_[A-Z0-9_]+')
SECTION_ORDER = ('EmuCore/GS', 'EmuCore', 'Folders', 'GT4GS')
REQUIRED_FIELDS = ('id', 'label', 'help', 'page', 'order', 'control', 'section', 'key', 'type', 'default', 'written',
                   'status')
PROFILE_DEFAULTS = 'gt4-defaults'
# First line of the default INI. Kept verbatim: the default output must stay byte-identical to TASK-036.
INI_HEADER = '; gt4-gs.ini written by scripts/external_runtime_probe.py (TASK-036 minimal settings)'


class SettingsError(ValueError):
    """A settings table, value or INI request that is refused."""


def load_table(path=TABLE_PATH):
    """Returns the validated list of setting rows in table order."""
    with Path(path).open('rb') as handle:
        data = tomllib.load(handle)
    errors = table_errors(data)
    if errors:
        raise SettingsError('invalid GS settings table ' + str(path) + ': ' + '; '.join(errors))
    return list(data['setting'])


def table_errors(data):
    errors = []
    if data.get('schema_version') != SCHEMA_VERSION:
        errors.append(f'schema_version must be {SCHEMA_VERSION}')
    rows = data.get('setting')
    if not isinstance(rows, list) or not rows:
        return errors + ['no [[setting]] rows']
    profiles = data.get('profile', [])
    profile_names = [profile.get('name') for profile in profiles]
    if PROFILE_DEFAULTS not in profile_names:
        errors.append(f'missing [[profile]] {PROFILE_DEFAULTS}')
    if len(set(profile_names)) != len(profile_names):
        errors.append('duplicate [[profile]] name')
    orders = {}
    seen_ids = set()
    seen_keys = set()
    for row in rows:
        name = str(row.get('id', '?'))
        for field in REQUIRED_FIELDS:
            if field not in row:
                errors.append(f'{name}: missing {field}')
        if 'id' in row and not ID_PATTERN.fullmatch(row['id']):
            errors.append(f'{name}: id must be lowercase letters, digits and underscores')
        if name in seen_ids:
            errors.append(f'{name}: duplicate id')
        seen_ids.add(name)
        if any(field not in row for field in REQUIRED_FIELDS):
            continue
        errors += _row_errors(row)
        if 'order' in row and 'page' in row:
            slot = (row['page'], row['order'])
            if slot in orders:
                errors.append(f'{name}: order {row["order"]} is already used on page {row["page"]}')
            orders[slot] = name
        if row['section'] and row['key']:
            pair = (row['section'], row['key'])
            if pair in seen_keys:
                errors.append(f'{name}: duplicate INI key {row["section"]} {row["key"]}')
            seen_keys.add(pair)
    return errors


def _row_errors(row):
    name = row['id']
    errors = []
    if row['page'] not in PAGES:
        errors.append(f'{name}: page must be one of {", ".join(PAGES)}')
    if row['type'] not in TYPES:
        errors.append(f'{name}: type must be one of {", ".join(TYPES)}')
    if not isinstance(row['order'], int) or row['order'] < 1:
        errors.append(f'{name}: order must be an integer from 1')
    if row['control'] not in CONTROLS:
        errors.append(f'{name}: control must be one of {", ".join(CONTROLS)}')
    elif row['written'] == 'launcher':
        if row['control'] != 'none':
            errors.append(f'{name}: launcher-supplied rows have control none')
    elif row['type'] == 'enum' and row['control'] == 'toggle' and len(row.get('choices', [])) == 2:
        pass  # TASK-091: a two-choice enum may be a toggle (first choice Off, second On), e.g. performance_mode.
    elif row['type'] in CONTROL_FOR_TYPE and row['control'] != CONTROL_FOR_TYPE[row['type']]:
        errors.append(f'{name}: a {row["type"]} row needs control {CONTROL_FOR_TYPE[row["type"]]}')
    if row['written'] not in WRITTEN:
        errors.append(f'{name}: written must be one of {", ".join(WRITTEN)}')
    if not STATUS_PATTERN.fullmatch(row['status']):
        errors.append(f'{name}: status must be ok, resolved, TBD-fork or pending-TASK-NNN')
    if row['section'] and row['section'] not in SECTION_ORDER:
        errors.append(f'{name}: section must be one of {", ".join(SECTION_ORDER)}')
    if row['written'] == 'never':
        if row['section'] or row['key']:
            errors.append(f'{name}: launcher-only row must have no INI section or key')
        if not ENV_PATTERN.fullmatch(row.get('env', '')):
            errors.append(f'{name}: launcher-only row needs env = GT4_...')
    elif row['status'] == 'ok' and not (row['section'] and row['key']):
        errors.append(f'{name}: settable row needs an INI section and key')
    if row['written'] in ('always', 'launcher') and row['status'] != 'ok':
        errors.append(f'{name}: {row["written"]} rows must have status ok')
    if row['type'] == 'path' and row['written'] != 'launcher':
        errors.append(f'{name}: path rows are launcher-supplied only')
    if row['type'] == 'string' and row['status'] == 'ok':
        errors.append(f'{name}: string rows need a verified value list (status ok is not allowed yet)')
    if row['type'] == 'bool' and row['default'] not in ('true', 'false'):
        errors.append(f'{name}: bool default must be true or false')
    if row['type'] in ('int', 'float'):
        for bound in ('min', 'max'):
            if row['status'] == 'ok' and not isinstance(row.get(bound), (int, float)):
                errors.append(f'{name}: ok {row["type"]} rows need numeric min and max')
        if isinstance(row.get('min'), (int, float)) and isinstance(row.get('max'), (int, float)) \
                and row['min'] > row['max']:
            errors.append(f'{name}: min is above max')
    if row['type'] == 'enum':
        errors += _choice_errors(row)
    if 'forced' in row and not isinstance(row['forced'], bool):
        errors.append(f'{name}: forced must be true or false')
    if row.get('forced') and (row['page'] != 'hidden' or row['written'] != 'always' or row['status'] != 'ok'):
        errors.append(f'{name}: forced rows must be page hidden, written always and status ok')
    if row['status'] == 'ok' and row['written'] in ('always', 'when-set') and row['type'] != 'path':
        try:
            parse_value(row, row['default'])
        except SettingsError as error:
            errors.append(f'{name}: default refused: {error}')
    return errors


def _choice_errors(row):
    name = row['id']
    errors = []
    if 'accept_ini' in row and not isinstance(row['accept_ini'], bool):
        errors.append(f'{name}: accept_ini must be true or false')
    choices = row.get('choices', [])
    names = [choice.get('name') for choice in choices]
    if len(set(names)) != len(names):
        errors.append(f'{name}: duplicate choice names')
    for choice in choices:
        if not choice.get('name'):
            errors.append(f'{name}: choice without name')
        if not STATUS_PATTERN.fullmatch(choice.get('status', '')):
            errors.append(f'{name}: choice {choice.get("name")} needs a status')
        if 'label' in choice and not isinstance(choice['label'], str):
            errors.append(f'{name}: choice {choice.get("name")} label must be text')
        if choice.get('status') == 'ok' and row['written'] != 'never' and not choice.get('ini'):
            errors.append(f'{name}: ok choice {choice.get("name")} needs an ini value')
        if 'offered' in choice and not isinstance(choice['offered'], bool):
            errors.append(f'{name}: choice {choice.get("name")} offered must be true or false')
        if 'height' in choice and not (isinstance(choice['height'], int) and choice['height'] > 0):
            errors.append(f'{name}: choice {choice.get("name")} height must be a positive integer')
    inis = [choice.get('ini') for choice in choices if choice.get('ini')]
    if row.get('accept_ini') and len(set(inis)) != len(inis):
        errors.append(f'{name}: accept_ini needs unique ini values')
    if row['status'] == 'ok' and not any(choice.get('status') == 'ok' for choice in choices):
        errors.append(f'{name}: ok enum row needs at least one ok choice')
    return errors


def rows_by_id(rows):
    return {row['id']: row for row in rows}


def _lookup(rows, identifier):
    index = rows_by_id(rows)
    if identifier not in index:
        raise SettingsError(f"unknown GS setting '{identifier}' (ids are in docs/launcher/gs-settings.toml)")
    return index[identifier]


def parse_value(row, raw):
    """Checks one value of a row and returns its normalized name (enum, bool, int) or text (path).

    A forced row (owner, 2026-10-21: aspect 16:9, the PCSX2 OSD rows off) accepts only its default value.
    """
    value = _parse_unforced(row, raw)
    if row.get('forced'):
        default = _parse_unforced(row, row['default'])
        if value != default:
            raise SettingsError(f'{row["id"]} is fixed at {default} in this release')
    return value


def _parse_unforced(row, raw):
    name = row['id']
    text = _json_text(raw)
    if row['type'] == 'path':
        if not text or '\n' in text or '\r' in text:
            raise SettingsError(f'{name} must be a single-line folder path')
        return text
    if row['status'] != 'ok':
        raise SettingsError(f'{name} is not settable yet ({row["status"]}); see docs/GS-SETTINGS.md')
    if row['type'] == 'bool':
        if text not in ('true', 'false'):
            raise SettingsError(f'{name} must be true or false, got {text!r}')
        return text
    if row['type'] == 'int':
        if not re.fullmatch(r'-?\d+', text):
            raise SettingsError(f'{name} must be an integer, got {text!r}')
        number = int(text)
        if not row['min'] <= number <= row['max']:
            raise SettingsError(f'{name} must be from {row["min"]} to {row["max"]}, got {number}')
        return str(number)
    if row['type'] == 'float':
        if not re.fullmatch(r'-?\d+(\.\d+)?', text):
            raise SettingsError(f'{name} must be a number, got {text!r}')
        number = float(text)
        if not row['min'] <= number <= row['max']:
            raise SettingsError(f'{name} must be from {row["min"]} to {row["max"]}, got {number}')
        return text
    if row['type'] == 'enum':
        choices = {choice['name']: choice for choice in row['choices']}
        if text not in choices and row.get('accept_ini'):
            # The INI value is accepted as well (upscale: 3 = 1080p, 5 = 5x).
            text = next((choice['name'] for choice in row['choices'] if choice.get('ini') == text), text)
        if text not in choices:
            raise SettingsError(f'{name} must be one of {", ".join(sorted(choices))}, got {text!r}')
        choice = choices[text]
        if choice['status'] != 'ok':
            raise SettingsError(f'{name}={text} is not settable yet ({choice["status"]})')
        return text
    raise SettingsError(f'{name} cannot be set from a value')


def ini_value(row, name):
    """The INI text of a parsed enum name (other types: the name itself)."""
    if row['type'] == 'enum':
        return next(choice['ini'] for choice in row['choices'] if choice['name'] == name)
    return name


def settings_from_assignments(assignments, rows):
    """Turns ID=VALUE strings (command line) into a checked settings dict of normalized names."""
    settings = {}
    for item in assignments:
        if '=' not in item:
            raise SettingsError(f"--gs-setting takes ID=VALUE, got {item!r}")
        identifier, _, raw = item.partition('=')
        row = _lookup(rows, identifier)
        if row['written'] in ('never', 'launcher') or row['type'] == 'path':
            raise SettingsError(f'{identifier} is set by the launcher, not by --gs-setting')
        if identifier in settings:
            raise SettingsError(f'{identifier} is given twice')
        settings[identifier] = parse_value(row, raw)
    return settings


def settings_to_json(settings):
    return json.dumps(dict(sorted(settings.items())), sort_keys=True)


def settings_from_json(text, rows):
    """Reads a settings dict from JSON (the GT4_GS_SETTINGS environment variable) and checks it."""
    def unique_settings(pairs):
        settings = {}
        for identifier, value in pairs:
            if identifier in settings:
                raise SettingsError(f'{identifier} is given twice')
            settings[identifier] = value
        return settings

    try:
        decoded = json.loads(text, object_pairs_hook=unique_settings)
    except json.JSONDecodeError as error:
        raise SettingsError(f'GT4_GS_SETTINGS is not JSON: {error}') from error
    if not isinstance(decoded, dict):
        raise SettingsError('GT4_GS_SETTINGS must be a JSON object')
    pairs = [f'{key}={_json_text(value)}' for key, value in decoded.items()]
    return settings_from_assignments(pairs, rows)


def _json_text(value):
    if isinstance(value, bool):
        return 'true' if value else 'false'
    return str(value)


def reset_to_defaults(rows=None):
    """The GT4 default settings dict (profile gt4-defaults): the default of every settable row.

    Launcher-supplied and launcher-only rows are left out. render_ini(rows, reset_to_defaults(rows)) is the default
    gt4-gs.ini, byte for byte (tests/python/test_gs_settings.py).
    """
    rows = load_table() if rows is None else rows
    settings = {}
    for row in rows:
        if row['status'] != 'ok' or row['written'] not in ('always', 'when-set') or row['type'] == 'path':
            continue
        settings[row['id']] = row['default']
    return settings


def render_ini(rows, settings=None, launcher=None, header=INI_HEADER):
    """The gt4-gs.ini text. settings: user values by id; launcher: values the launcher supplies (folders, headless).

    'always' rows are written with their GT4 default unless settings override them; 'when-set' rows only when set;
    'launcher' rows only when launcher supplies them. Sections appear in SECTION_ORDER, rows in table order.
    """
    settings = dict(settings or {})
    launcher = dict(launcher or {})
    index = rows_by_id(rows)
    for identifier, value in settings.items():
        row = _lookup(rows, identifier)
        if row['written'] in ('never', 'launcher') or row['type'] == 'path':
            raise SettingsError(f'{identifier} is set by the launcher, not by a user setting')
        settings[identifier] = parse_value(row, value)
    for identifier, value in launcher.items():
        row = _lookup(rows, identifier)
        if row['written'] != 'launcher':
            raise SettingsError(f'{identifier} is not a launcher row')
        launcher[identifier] = parse_value(row, value)
    entries = []
    for row in rows:
        identifier = row['id']
        if not row['section'] or not row['key']:
            continue
        if row['written'] == 'always':
            value = settings.get(identifier, row['default'])
        elif row['written'] == 'when-set' and identifier in settings and                 parse_value(row, settings[identifier]) != parse_value(row, row['default']):
            value = settings[identifier]
        elif row['written'] == 'launcher' and identifier in launcher:
            value = launcher[identifier]
        else:
            continue
        entries.append((row['section'], row['key'], ini_value(index[identifier], parse_value(index[identifier], value))))
    lines = [header]
    first = True
    for section in SECTION_ORDER:
        block = [entry for entry in entries if entry[0] == section]
        if not block:
            continue
        if not first:
            lines.append('')
        first = False
        lines.append(f'[{section}]')
        lines += [f'{key} = {value}' for _, key, value in block]
    return '\n'.join(lines) + '\n'


def write_ini_atomic(path, text):
    """Writes text to path through a temporary file in the same folder, then replaces path (os.replace)."""
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    descriptor, temporary = tempfile.mkstemp(dir=path.parent, prefix=path.name + '.', suffix='.tmp')
    try:
        with os.fdopen(descriptor, 'w', encoding='utf-8') as handle:
            handle.write(text)
            handle.flush()
            os.fsync(handle.fileno())
        os.replace(temporary, path)
    except BaseException:
        try:
            os.unlink(temporary)
        except OSError:
            pass
        raise
    return path


def allowlist_rule(row):
    """What gt4-gs.exe accepts for one keyed row: the type and its values (every choice, whatever its status: the
    launcher decides what is offered; gt4-gs only refuses what PCSX2 or the fork does not know)."""
    rule = {'section': row['section'], 'key': row['key'], 'type': row['type']}
    if row['type'] == 'bool':
        rule['values'] = ['true', 'false']
    elif row['type'] == 'enum':
        # TASK-108: an INI value shared by two choices (auto and vulkan both write 14) is listed once.
        rule['values'] = list(dict.fromkeys(choice['ini'] for choice in row['choices'] if choice.get('ini')))
    elif row['type'] in ('int', 'float'):
        if isinstance(row.get('min'), (int, float)) and isinstance(row.get('max'), (int, float)):
            rule['min'] = row['min']
            rule['max'] = row['max']
    return rule


def allowlist(rows):
    keyed = sorted((row for row in rows if row['section'] and row['key']), key=lambda row: (row['section'], row['key']))
    return {'schema_version': SCHEMA_VERSION,
            'source': 'docs/launcher/gs-settings.toml',
            'note': ('INI section/key pairs and values gt4-gs.exe accepts. gt4-gs 1.0.0 and later embed this file at build '
                     'time; any other key, or a value outside its rule, is ignored with one warning line in the log. Types: '
                     'bool and enum list their values; int and float give min and max; string and path take any '
                     'single-line text.'),
            'entries': [allowlist_rule(row) for row in keyed]}


def allowlist_text(rows):
    return json.dumps(allowlist(rows), indent=2, ensure_ascii=False) + '\n'


def offered(rows):
    """Rows the launcher may show: page basic or advanced, settable, and with at least one settable value.

    Launcher-only rows (written = never) such as gs_engine are offered; launcher-supplied rows are not.
    """
    result = []
    for row in rows:
        if row['page'] not in ('basic', 'advanced') or row['status'] != 'ok' or row['written'] == 'launcher':
            continue
        if row['type'] == 'enum' and not any(choice['status'] == 'ok' for choice in row['choices']):
            continue
        result.append(row)
    return sorted(result, key=lambda row: (PAGES.index(row['page']), row['order']))


def offered_choices(row):
    """The choice names the launcher may show for an enum row (status ok and not marked offered = false). A forced row
    offers only its default."""
    if row.get('forced'):
        return [row['default']]
    return [choice['name'] for choice in row['choices'] if choice['status'] == 'ok' and choice.get('offered', True)]


# Internal resolution presets (owner decision 2026-10-16). The launcher (TASK-019) uses these two helpers; they are not a
# launcher themselves.
UPSCALE_DEFAULT_PRESET = '1080p'
UPSCALE_TEXTURE_DIVISOR = 1280  # PCSX2 GSGetMaxUpscaleMultiplier: max(max texture size / 1280, 1)


def upscale_presets(rows):
    """The offered upscale presets in order: (name, multiplier, output height)."""
    row = rows_by_id(rows)['upscale']
    return [(choice['name'], int(choice['ini']), choice['height']) for choice in row['choices']
            if choice['status'] == 'ok' and choice.get('offered', True)]


def upscale_preset_for_monitor(rows, monitor_height):
    """The launcher's initial preset: the smallest preset whose height is at least the monitor's vertical resolution;
    an unknown height (None or 0) gives 1080p; a monitor taller than every preset gets the largest."""
    presets = upscale_presets(rows)
    if not monitor_height:
        return UPSCALE_DEFAULT_PRESET
    for name, _, height in presets:
        if height >= monitor_height:
            return name
    return presets[-1][0]


def upscale_presets_within_texture_limit(rows, max_texture_size):
    """The presets the GPU can draw: multiplier <= max(max texture size / 1280, 1). The launcher greys out the others."""
    limit = max(int(max_texture_size) // UPSCALE_TEXTURE_DIVISOR, 1)
    return [name for name, multiplier, _ in upscale_presets(rows) if multiplier <= limit]


def main(argv=None):
    parser = argparse.ArgumentParser(description='GS settings table, gt4-gs.ini and allow-list (TASK-039).')
    commands = parser.add_subparsers(dest='command', required=True)
    commands.add_parser('check', help='validate docs/launcher/gs-settings.toml')
    listing = commands.add_parser('allowlist', help='print or write the allow-list JSON')
    listing.add_argument('--write', action='store_true', help='write docs/launcher/gs-settings-allowlist.json')
    generate = commands.add_parser('ini', help='print or write a gt4-gs.ini')
    generate.add_argument('--set', action='append', default=[], metavar='ID=VALUE')
    generate.add_argument('--out', type=Path, default=None)
    arguments = parser.parse_args(argv)
    try:
        rows = load_table()
        if arguments.command == 'check':
            print(f'{len(rows)} rows, table valid')
        elif arguments.command == 'allowlist':
            text = allowlist_text(rows)
            if arguments.write:
                write_ini_atomic(ALLOWLIST_PATH, text)
                print(f'wrote {ALLOWLIST_PATH}')
            else:
                sys.stdout.write(text)
        else:
            text = render_ini(rows, settings_from_assignments(arguments.set, rows))
            if arguments.out:
                write_ini_atomic(arguments.out, text)
                print(f'wrote {arguments.out}')
            else:
                sys.stdout.write(text)
    except SettingsError as error:
        print(f'error: {error}', file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
