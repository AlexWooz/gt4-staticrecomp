"""TASK-100 S1: Spec II patch-set tools (docs/SPEC2-PATCHES.md).

Pure checks: they never read the owner's discs. The beta container, the injector ELF and the diff are exercised with
synthetic images built here, and the disc-derived constants are pinned so an accidental edit fails.
"""
import hashlib
import json
import os
import struct
import sys
import tempfile
import unittest
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'scripts'))
import spec2_beta_elf as beta  # noqa: E402
import spec2_patches as patches  # noqa: E402
import spec2_iop_image as iop_image  # noqa: E402
import prepare_perf_runtime as runtime  # noqa: E402


class PinTests(unittest.TestCase):
    def test_disc_constants_are_pinned(self):
        self.assertEqual(beta.ISO_SIZE, 2654109696)
        self.assertEqual(beta.ISO_SHA256, '1f49b9d8077801188860b62c3efdbc631ba8af7df979373811ce8c60dcc48ebe')
        self.assertEqual(beta.CORE_SHA256, '0cf52c3ad8e9c1ddba55e7538a35dfed1c02cd0e38e5c348bdf80975046617ff')
        self.assertEqual(beta.CORE_SIZE, 2269619)
        self.assertEqual(beta.SALSA_KEY, b'PolyphonyDigital')
        self.assertEqual(beta.CONTAINER_TAG, 0x0101)

    def test_spec2_patch_pin_matches_the_tool(self):
        pin = json.loads((ROOT / 'docs/inputs/spec2-patches.json').read_text(encoding='utf-8'))
        self.assertEqual(pin['schema_version'], 1)
        self.assertEqual(pin['counts'], {'total': 35, 'text': 18, 'data': 17, 'payload': 0})
        self.assertEqual(pin['elf']['sha256'],
                         'db08421bc26bdc0298e80cb02a8f2067ddf640f0575af5dfe0548f1fd73ac39d')
        self.assertEqual(pin['static_patches']['words'], 1)
        self.assertEqual(pin['static_patches']['detail'][0]['address'], 0x1001E8)
        self.assertEqual(pin['static_patches']['detail'][0]['old_words'], [0x0C178EAA])
        self.assertEqual(pin['static_patches']['detail'][0]['new_words'], [0x0C1A29DA])
        self.assertEqual(pin['beta']['records'][1]['size'], 0x55FBBC)
        self.assertEqual(pin['beta']['records'][2]['size'], 0xD45FC)
        # The beta's two 128-byte container headers are pinned by size and hash only: no disc bytes in git.
        self.assertNotIn('strings', pin['beta'])
        self.assertEqual([h['size_bytes'] for h in pin['beta']['headers']], [128, 128])
        for header in pin['beta']['headers']:
            self.assertRegex(header['sha256'], r'^[0-9a-f]{64}$')

    def test_beta_pin_matches_the_tool(self):
        pin = json.loads((ROOT / 'docs/inputs/spec2-beta.json').read_text(encoding='utf-8'))
        self.assertEqual(pin['derived_elf']['sha256'],
                         '2e5621ede797ff064eef4c51eff7d4ad01e0cefb82bc9895107ee818192aa039')
        self.assertEqual(pin['container']['inflated_size_bytes'], 6505204)
        self.assertEqual(pin['container']['records'][1]['address'], 0x100000)
        self.assertEqual(pin['static_diff_vs_spec2']['addresses_outside_the_plugin'], [0x1001E8])


class CrcTests(unittest.TestCase):
    def test_table_is_the_loader_polynomial(self):
        # MSB-first Castagnoli table: the first entries are distinctive and were read from the loader's table.
        self.assertEqual(beta._CRC32C[0], 0x00000000)
        self.assertEqual(beta._CRC32C[1], 0x1EDC6F41)
        self.assertEqual(beta._CRC32C[2], 0x3DB8DE82)
        self.assertEqual(beta._CRC32C[255], 0x8ACABEB5)

    def test_detects_a_flipped_byte(self):
        data = bytes(range(64))
        self.assertNotEqual(beta.crc32c(data), beta.crc32c(data[:-1] + bytes([data[-1] ^ 1])))


class SalsaTests(unittest.TestCase):
    def test_keystream_is_pinned(self):
        # A change in the state layout or the rounds changes this hash; the disc-derived evidence for the variant is
        # in docs/SPEC2-PATCHES.md.
        stream = beta.salsa20_stream_xor(beta.SALSA_KEY, bytes.fromhex('0800fafd86b828ed'), bytes(128))
        self.assertEqual(stream[:16].hex(), 'edf8d425eceaa4009ab024b00c6f1a78')
        self.assertEqual(hashlib.sha256(stream).hexdigest(),
                         '4a26d4dea67fd2954bb84d07f30c56229b78703bf05bb1b005d5b572a3778ed4')
        self.assertEqual(len(stream), 128)

    def test_round_trip(self):
        data = b'the quick brown fox ' * 7
        stream = beta.salsa20_stream_xor(beta.SALSA_KEY, b'\x01' * 8, data)
        self.assertEqual(beta.salsa20_stream_xor(beta.SALSA_KEY, b'\x01' * 8, stream), data)


class ContainerTests(unittest.TestCase):
    def build(self, records, entry=0x100008):
        payload = bytearray()
        for name in (b'A' * 128, b'B' * 128):
            payload += struct.pack('<H', len(name)) + name
        payload += struct.pack('<iI', len(records), entry)
        for address, body in records:
            payload += struct.pack('<II', address, len(body)) + body
        return bytes(payload)

    def test_round_trip_and_trailing_bytes(self):
        payload = self.build([(0x65FBA4, bytes(24)), (0x100000, bytes(64)), (0x65FC00, bytes(32))])
        headers, entry, records = beta.parse_container(payload)
        self.assertEqual(entry, 0x100008)
        self.assertEqual([r['address'] for r in records], [0x65FBA4, 0x100000, 0x65FC00])
        with self.assertRaises(ValueError):
            beta.parse_container(payload + b'x')

    def test_inflate_checks_the_declared_size(self):
        body = zlib.compressobj(9, zlib.DEFLATED, -15)
        plain = struct.pack('<HI', beta.CONTAINER_TAG, 4) + body.compress(b'abcd') + body.flush()
        payload, size = beta.inflate_container(plain)
        self.assertEqual((payload, size), (b'abcd', 4))
        second = zlib.compressobj(9, zlib.DEFLATED, -15)
        bad = struct.pack('<HI', beta.CONTAINER_TAG, 8) + second.compress(b'abcd') + second.flush()
        with self.assertRaises(ValueError):
            beta.inflate_container(bytes(bad))

    def test_build_elf_places_the_reginfo_after_the_text(self):
        elf = beta.build_elf(0x100008, [
            {'address': 0x65FBA4, 'body': bytes(24)},
            {'address': 0x100000, 'body': bytes(0x1000)},
            {'address': 0x65FC00, 'body': bytes(0x100)},
        ])
        self.assertEqual(elf[:4], b'\x7fELF')
        words = beta._word_map(elf)
        self.assertEqual(words[0x100000], 0)
        # .reginfo is a separate PT_LOAD at the text end, data at its own address.
        self.assertIn(0x65FC00, words)


class InjectorTests(unittest.TestCase):
    @staticmethod
    def make_elf(text_words, data_words):
        """One PT_LOAD at 0x100000 and one at 0x65FC00, no sections."""
        text = b''.join(struct.pack('<I', w) for w in text_words)
        data = b''.join(struct.pack('<I', w) for w in data_words)
        header_size = 52 + 32 * 2
        text_off = 0x1000
        data_off = (text_off + len(text) + 0xFFF) & ~0xFFF
        out = bytearray(data_off + len(data))
        out[:16] = b'\x7fELF\x01\x01\x01' + b'\0' * 9
        struct.pack_into('<HHIIIIIHHHHHH', out, 16, 2, 8, 1, 0x100008, 52, 0, 0, header_size, 32, 2, 0, 0, 0)
        struct.pack_into('<8I', out, 52, 1, text_off, 0x100000, 0x100000, len(text), len(text), 7, 4096)
        struct.pack_into('<8I', out, 84, 1, data_off, 0x65FC00, 0x65FC00, len(data), len(data), 6, 4096)
        out[text_off:text_off + len(text)] = text
        out[data_off:data_off + len(data)] = data
        return bytes(out)

    def test_interpreter_records_an_injected_store(self):
        # At 0x68A480: a0 = 0x661230; a1 = 0x42; sw a1,0(a0); jr ra; nop.
        program = [
            0x3C020066,  # lui   v0,0x0066
            0x34441230,  # ori   a0,v0,0x1230
            0x24050042,  # addiu a1,zero,0x42
            0xAC850000,  # sw    a1,0(a0)
            0x03E00008,  # jr    ra
            0x00000000,  # nop
        ]
        image = bytearray(self.make_elf([0x00000000] * 4, [0x00000000] * 4))
        # Place the program at the payload VA by adding a third PT_LOAD: easiest is to extend the text segment.
        text_off = 0x1000
        bias = 0x68A480 - 0x100000
        if len(image) < text_off + bias + len(program) * 4:
            image.extend(bytes(text_off + bias + len(program) * 4 - len(image)))
        for index, word in enumerate(program):
            struct.pack_into('<I', image, text_off + bias + index * 4, word)
        struct.pack_into('<I', image, 52 + 16, text_off + bias + len(program) * 4)
        elf = patches.ElfImage(bytes(image), 'synthetic')
        machine, _, table = patches.build_patch_table(elf)
        writes = [(p['addr'], p['new']) for p in table]
        self.assertIn((0x661230, 0x42), writes)
        self.assertEqual(table[0]['region'], 'data')
        self.assertEqual(table[0]['kind'], 'data_word')

    def test_perturbation_is_stable_for_the_synthetic_image(self):
        program = [0x3C020066, 0x34441230, 0x24050042, 0xAC850000, 0x03E00008, 0x00000000]
        image = bytearray(self.make_elf([0] * 4, [0] * 4))
        text_off = 0x1000
        bias = 0x68A480 - 0x100000
        image.extend(bytes(max(0, text_off + bias + 24 - len(image))))
        for index, word in enumerate(program):
            struct.pack_into('<I', image, text_off + bias + index * 4, word)
        struct.pack_into('<I', image, 52 + 16, text_off + bias + 24)
        elf = patches.ElfImage(bytes(image), 'synthetic')
        base = [(p['addr'], p['new']) for p in patches.build_patch_table(elf)[2]]
        other = [(p['addr'], p['new']) for p in patches.build_patch_table(elf, sp=0x01F00000)[2]]
        self.assertEqual(base, other)


class DiffTests(unittest.TestCase):
    def test_coalesces_contiguous_words_and_skips_ranges(self):
        old = {0x1000 + 4 * i: 1 for i in range(8)}
        new = dict(old)
        new[0x1008] = 2
        new[0x100C] = 3
        new[0x1014] = 9
        blocks = patches.diff_words(old, new)
        self.assertEqual([(b['address'], b['words']) for b in blocks], [(0x1008, 2), (0x1014, 1)])
        self.assertEqual(blocks[0]['old_words'], [1, 1])
        self.assertEqual(blocks[0]['new_words'], [2, 3])
        skipped = patches.diff_words(old, new, skip_ranges=[(0x1008, 0x1010)])
        self.assertEqual([(b['address'], b['words']) for b in skipped], [(0x1014, 1)])

    def test_classify_marks_strings_and_instructions(self):
        entry = {'addr': 0x6F02B0, 'size': 4, 'new': 0x30687865, 'old': 0}
        patches.classify(entry, None, 0)
        self.assertEqual(entry['kind'], 'string')
        entry = {'addr': 0x1001E8, 'size': 4, 'new': 0x0C1A29DA, 'old': 0}
        patches.classify(entry, None, 0)
        self.assertEqual(entry['kind'], 'jal_into_payload')
        self.assertEqual(entry['target'], 0x68A768)


class VuTests(unittest.TestCase):
    @staticmethod
    def make_elf(text_bytes, data_bytes, data_va=0x65FC00):
        text_off, data_off = 0x1000, 0x560C00
        out = bytearray(max(text_off + len(text_bytes), data_off + len(data_bytes)))
        out[:16] = b'\x7fELF\x01\x01\x01' + b'\0' * 9
        struct.pack_into('<HHIIIIIHHHHHH', out, 16, 2, 8, 1, 0x100008, 52, 0, 0, 52 + 64, 32, 2, 0, 0, 0)
        struct.pack_into('<8I', out, 52, 1, text_off, 0x100000, 0x100000, len(text_bytes), len(text_bytes), 7, 4096)
        struct.pack_into('<8I', out, 84, 1, data_off, data_va, data_va, len(data_bytes), len(data_bytes), 6, 4096)
        out[text_off:text_off + len(text_bytes)] = text_bytes
        out[data_off:data_off + len(data_bytes)] = data_bytes
        return bytes(out)

    def test_vu1_blob_identity(self):
        import spec2_vu
        blob = bytes(range(256)) * 64 + bytes(0x20)  # 0x3FF0 bytes
        blob = blob[:0x3FF0]
        spec2 = bytearray(self.make_elf(bytes(0x1000), bytes(0x59C110 - 0x560C00 + 0x3FF0)))
        spec2[0x59C110:0x59C110 + 0x3FF0] = blob
        v200 = bytearray(self.make_elf(bytes(0x1000), bytes(0x525DD0 - 0x1000 + 0x3FF0)))
        v200[0x525DD0:0x525DD0 + 0x3FF0] = blob
        report = spec2_vu.vu1_report(bytes(spec2), bytes(v200))
        self.assertTrue(report['identical_to_v200'])
        self.assertEqual(report['decrypted_sha256'], report['v200_decrypted_sha256'])
        broken = bytearray(v200)
        broken[0x525DD0] ^= 1
        self.assertFalse(spec2_vu.vu1_report(bytes(spec2), bytes(broken))['identical_to_v200'])

    def test_vu0_upload_is_read_from_the_data_segment(self):
        import spec2_vu
        data = bytearray(0x731180 - 0x65FC00 + 0x920)
        data[0x731180 - 0x65FC00:0x731180 - 0x65FC00 + 4] = b'\x11\x22\x33\x44'
        spec2 = self.make_elf(bytes(0x1000), bytes(data))
        self.assertEqual(spec2_vu.spec2_file_offset(0x731180), 0x560C00 + 0x731180 - 0x65FC00)
        self.assertEqual(spec2_vu.spec2_file_offset(0x100010), 0x1010)
        with self.assertRaises(ValueError):
            spec2_vu.spec2_file_offset(0x80000000)

    def test_vu_pin_matches_the_tool_contract(self):
        pin = json.loads((ROOT / 'docs/inputs/spec2-vu.json').read_text(encoding='utf-8'))
        self.assertEqual(pin['vu1']['expected_prefix'], 'ae6b6cb3b4406ff3')
        self.assertTrue(pin['vu1']['decrypted_sha256'].startswith('ae6b6cb3b4406ff3'))
        self.assertTrue(pin['vu1']['identical_to_v200'])
        self.assertEqual(pin['vu0']['upload_bytes'], 0x920)
        self.assertEqual(pin['vu0']['vaddr'], 0x731180)
        self.assertGreater(pin['vu0']['differing_bytes_in_the_overlap'], 0)
        self.assertEqual(len(pin['vu0']['vcallms_entries']), 14)


class IopTests(unittest.TestCase):
    def test_pin_is_complete_and_policied(self):
        pin = json.loads((ROOT / 'docs/inputs/spec2-iop.json').read_text(encoding='utf-8'))
        allowed = set(pin['policies'])
        self.assertEqual(len(pin['modules']), 35)
        for module in pin['modules']:
            self.assertIn(module['policy'], allowed, module['path'])
            self.assertRegex(module['sha256'], r'^[0-9a-f]{64}$')
            self.assertGreater(module['size_bytes'], 0)
        self.assertEqual(sum(m['size_bytes'] for m in pin['modules']), pin['total_bytes'])
        new = {m['path'] for m in pin['modules'] if m['vs_v200'] == 'new'}
        self.assertEqual(new, {'/IRX/LGAUD.IRX', '/IRX/PPP.IRX', '/IRX/PPPOE.IRX',
                               '/NET/DNAS_MC.ERX', '/NET/DNAS_NET.ERX', '/NET/NTG_ISCK.ERX'})
        self.assertIn('pppoe.irx', pin['module_load_list_from_text']['names'])


class DerivedElfTests(unittest.TestCase):
    @staticmethod
    def make_elf(words):
        text = b''.join(struct.pack('<I', w) for w in words)
        out = bytearray(0x1000 + len(text))
        out[:16] = b'\x7fELF\x01\x01\x01' + b'\0' * 9
        struct.pack_into('<HHIIIIIHHHHHH', out, 16, 2, 8, 1, 0x100008, 52, 0, 0, 84, 32, 1, 0, 0, 0)
        struct.pack_into('<8I', out, 52, 1, 0x1000, 0x100000, 0x100000, len(text), len(text), 7, 4096)
        out[0x1000:0x1000 + len(text)] = text
        return bytes(out)

    def test_apply_and_revert_restore_the_image(self):
        import spec2_elf_patch
        image = self.make_elf([0x11111111, 0x22222222, 0x33333333])
        patches = [{'addr': 0x100004, 'size': 4, 'old': 0x22222222, 'new': 0xDEADBEEF, 'source': 'injector'}]
        patched = spec2_elf_patch.apply_patches(image, patches)
        self.assertNotEqual(patched, image)
        self.assertEqual(struct.unpack_from('<I', patched, 0x1004)[0], 0xDEADBEEF)
        self.assertEqual(spec2_elf_patch.apply_patches(patched, patches, reverse=True), image)

    def test_apply_refuses_a_mismatched_old_word(self):
        import spec2_elf_patch
        image = self.make_elf([0x11111111])
        patches = [{'addr': 0x100000, 'size': 4, 'old': 0x99999999, 'new': 0x1, 'source': 'injector'}]
        with self.assertRaises(ValueError):
            spec2_elf_patch.apply_patches(image, patches)

    def test_pin_matches_the_patch_table(self):
        import spec2_elf_patch
        elf_pin = json.loads((ROOT / 'docs/inputs/spec2-elf.json').read_text(encoding='utf-8'))
        patch_pin = json.loads((ROOT / 'docs/inputs/spec2-patches.json').read_text(encoding='utf-8'))
        iso_pin = json.loads((ROOT / 'docs/inputs/spec2-iso.json').read_text(encoding='utf-8'))
        injector = [p for p in patch_pin['patches'] if p.get('source', 'injector') == 'injector']
        self.assertEqual(elf_pin['patches_applied'], len(injector))
        self.assertEqual(elf_pin['original']['sha256'], patch_pin['elf']['sha256'])
        self.assertEqual(iso_pin['disc']['sha256'], '6a3484b9672c10a92154ecee6faa5bbcfd4ed8cd538d8283e429c3fe6e1704b2')
        self.assertEqual(iso_pin['files']['/SCUS_974.36;1']['sha256'], patch_pin['elf']['sha256'])
        self.assertTrue(elf_pin['entry_call']['kept'])
        for patch in injector:
            address = patch['addr']
            self.assertTrue(0x100000 <= address < 0x65FBBC or 0x65FC00 <= address < 0x65FC00 + 0xD45FC,
                            hex(address))


class EeGenerateTests(unittest.TestCase):
    def test_pin_is_consistent(self):
        pin = json.loads((ROOT / 'docs/inputs/spec2-ee-generated.json').read_text(encoding='utf-8'))
        elf = json.loads((ROOT / 'docs/inputs/spec2-elf.json').read_text(encoding='utf-8'))
        self.assertEqual(pin['input_elf']['sha256'], elf['derived']['sha256'])
        self.assertEqual(pin['translator']['sha256'],
                         '5aec705e31da349c941414fd54be264000459c3cb0d22727e7d810b5b24b03c0')
        self.assertEqual(pin['units'], 346)
        self.assertEqual(pin['files'], pin['units'] + 2)  # table.cpp and the ledger/summary file
        self.assertRegex(pin['inventory_sha256'], r'^[0-9a-f]{64}$')
        self.assertIn('1332727 instructions', pin['stats'])
        self.assertIn('functions 18597', pin['stats'])
        self.assertEqual(pin['entry_seeds']['sha256'],
                         '041c186448d75756a3fc4413c53e7c829b46bf13f83c699ca4a998734a35ec8a')


class EntrySeedTests(unittest.TestCase):
    """TASK-100 S8c: the widened seed scan for entries the translator's narrow luipair window misses."""

    def test_seed_file_format_and_dedup(self):
        import spec2_entry_seeds
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / 'seeds.txt'
            addresses = spec2_entry_seeds.write_seed_file(path, [0x3CA690, 0x100010, 0x3CA690])
            self.assertEqual(addresses, [0x100010, 0x3CA690])
            self.assertEqual(path.read_text(encoding='utf-8').splitlines(), ['2', '0x00100010', '0x003CA690'])

    def test_options_menu_target_is_found_by_the_wide_luipair_scan(self):
        assets = Path(os.environ.get('GT4_ASSETS_ROOT', 'C:/Antigravity/gt4-staticrecomp'))
        elf = assets / 'private/slice272-spec2-elf/SCUS_974.36'
        if not elf.exists():
            self.skipTest('derived Spec II ELF not present')
        import spec2_entry_seeds
        result = spec2_entry_seeds.collect(elf)
        # `lui s0,0x003D` at 0x003CC8EC, then `addiu s0,s0,-22896` 53 instructions later: the Options halt.
        self.assertEqual(result['sources']['luipair'].get(0x3CA690), 'lui at 0x003CC8EC')
        self.assertIn(0x3CA690, result['candidates'])
        # A zero/ASCII word in the plugin data is not an entry.
        self.assertNotIn(0x68CFC0, result['candidates'])


class IopRebootImageTests(unittest.TestCase):
    """S7b: the DNAS310.IMG romdir parser and the pin derived from the owner's disc (hashes only, no disc bytes)."""

    @staticmethod
    def make_image(modules, bookkeeping=('RESET', 'ROMDIR', 'EXTINFO')):
        def entry(name, size):
            return name.encode('latin-1').ljust(10, b'\x00') + (0).to_bytes(2, 'little') + size.to_bytes(4, 'little')
        rows = [(name, 0) for name in bookkeeping] + [(name, len(data)) for name, data in modules]
        header = b''.join(entry(name, size) for name, size in rows) + b'\x00' * 16
        body = b''
        for _, data in modules:
            body += data + b'\x00' * ((16 - len(data) % 16) % 16)
        return header + b'\x00' * ((16 - len(header) % 16) % 16) + body

    def test_romdir_layout_and_module_hashes(self):
        first = b'\x7fELF' + bytes(range(28))
        second = b'\x7fELF' + bytes(range(12))
        image = self.make_image([('SYSMEM', first), ('STDIO', second)])
        parsed = iop_image.parse_image(image)
        self.assertEqual([module['name'] for module in parsed['modules']], ['SYSMEM', 'STDIO'])
        self.assertEqual([module['size'] for module in parsed['modules']], [len(first), len(second)])
        self.assertTrue(all(module['elf'] for module in parsed['modules']))
        self.assertEqual(parsed['modules'][0]['sha256'], hashlib.sha256(first).hexdigest())
        self.assertEqual(parsed['modules'][1]['hle'], iop_image.HLE_SURFACE['STDIO'])
        self.assertEqual(parsed['modules'][0]['hle'], iop_image.HLE_SURFACE['SYSMEM'])

    def test_a_file_that_is_not_a_romdir_is_refused(self):
        with self.assertRaises(ValueError):
            iop_image.parse_image(b'\x7fELF' + b'\x00' * 64)

    def test_pin_matches_the_tool(self):
        pin = json.loads((ROOT / 'docs/inputs/spec2-dnas310.json').read_text(encoding='utf-8'))
        self.assertEqual(pin['spec2_iso_sha256'],
                         '6a3484b9672c10a92154ecee6faa5bbcfd4ed8cd538d8283e429c3fe6e1704b2')
        self.assertEqual(pin['image']['path'], '/DNAS310.IMG;1')
        self.assertEqual(pin['image']['size_bytes'], 282545)
        self.assertEqual(pin['image']['sha256'],
                         'c860a99d314b4e9e1b19c41988a76fc9ea3aa172c4770dfa05cef5f482d45424')
        names = [module['name'] for module in pin['image']['modules']]
        self.assertEqual(len(names), 16)
        self.assertEqual(names[0], 'SYSMEM')
        self.assertEqual(names[-1], 'STDIO')
        self.assertEqual(len(set(names)), len(names))
        self.assertTrue(pin['comparison']['same_module_order'])
        self.assertEqual(pin['comparison']['identical_modules'], [])
        for module in pin['image']['modules']:
            self.assertEqual(module['hle'], iop_image.HLE_SURFACE[module['name']])
            self.assertRegex(module['sha256'], r'^[0-9a-f]{64}$')
            self.assertTrue(module['elf'])
        # The pin carries sizes and hashes only: no module bytes.
        self.assertNotIn('bytes', pin['image'])
        self.assertNotIn('data', pin['image'])


class Spec2RuntimeCapsuleTests(unittest.TestCase):
    """S7b: the Spec II IOP reboot capsule (scripts/prepare_perf_runtime.py --spec2-iop) and its pin."""

    def test_overlay_applies_and_reverses(self):
        source = ('#include "sif.hpp"\n\n#include "../hw.hpp"\n#include "iop.hpp"\n#include "io.hpp"\n'
                  '#include "kernel.hpp"\n\nvoid reboot() {\n'
                  '    if (path.find("IOPRP") != std::string::npos) g_fileio_async = true;\n'
                  '    if (const auto space = path.find(\' \'); space != std::string::npos) '
                  'read_loadfile_version(path.substr(space + 1));\n}\n')
        result = runtime.apply_spec2_iop(source)
        self.assertIn('#include "spec2_iop.hpp"', result)
        self.assertIn('#include "iop/sif.hpp"', result)
        self.assertNotIn('../hw.hpp', result)
        self.assertIn('gt4::spec2_iop::async_fileio_boot_image(path, g_loadfile_version)', result)
        self.assertNotIn('if (path.find("IOPRP") != std::string::npos) g_fileio_async = true;', result)
        # The derivation is reversible (apply_overlay checks this too; assert the reverse text here).
        inverse = result.replace('gt4::spec2_iop::async_fileio_boot_image(path, g_loadfile_version)',
                                 'path.find("IOPRP") != std::string::npos')
        self.assertIn('if (path.find("IOPRP") != std::string::npos) g_fileio_async = true;', inverse)

    def test_overlay_refuses_a_changed_anchor(self):
        with self.assertRaises(ValueError):
            runtime.apply_spec2_iop('void reboot() {\n}\n')

    def test_capsule_pin_is_consistent(self):
        pin = json.loads((ROOT / 'docs/inputs/slice276-spec2-iop-runtime.json').read_text(encoding='utf-8'))
        self.assertEqual(pin['donor_sha256']['iop/sif.cpp'],
                         '182a24218d719a669b3742867f9901b49d2e1043393f2211e93363dc0180dacc')
        self.assertIn('sif.cpp', pin['source_sha256'])
        self.assertEqual(pin['spec2_iop_profile'], 'dnas310-reboot-v1')
        self.assertEqual(pin['mtvu_runtime_profile'], 'task084-frame-cost-v1')
        self.assertEqual(pin['vu0_aot_profile'], 'overlay-v1')
        self.assertEqual(pin['gs_stream_profile'], 'task035-seam-v1')
        self.assertRegex(pin['receipt_sha256'], r'^[0-9a-f]{64}$')


if __name__ == '__main__':
    unittest.main()
