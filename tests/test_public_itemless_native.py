"""Public Itemless hook contracts against a supplied USA/EU 1.21 executable."""
from paths import ROOT
from pathlib import Path
import os
import struct
import unittest
from prepare import parse_self

@unittest.skipUnless(os.environ.get('BB_TEST_GAME'), 'requires a local game dump')
class PublicItemlessNative(unittest.TestCase):
    def test_native_hooks_and_event_slots(self):
        elf, _, headers, *_ = parse_self((Path(os.environ['BB_TEST_GAME']) / 'eboot.bin').read_bytes())
        def read(address, size):
            segment = next(p for p in headers if p['type'] in (1, 0x61000010) and
                           p['vaddr'] <= address < p['vaddr'] + p['filesz'])
            at = segment['offset'] + address - segment['vaddr']
            return elf[at:at + size]
        prefix = bytes.fromhex('55 48 89 e5 41 57 41 56 41 55 41 54 53 48 83 ec')
        for address, stack in ((0xda300, 0x68), (0xd8fb0, 0x58), (0xda4f0, 0x58), (0xda7f0, 0x58)):
            self.assertEqual(read(address, 17), prefix + bytes([stack]))
        for slot, target in ((0x2549ea8, 0xa14890), (0x2549ee0, 0xa14910),
                             (0x2549ee8, 0xa14e20), (0x2549ec0, 0xa148c0)):
            self.assertEqual(struct.unpack('<Q', read(slot, 8))[0], target)
        self.assertEqual(read(0xa548e0, 17), prefix + b'\x38')
        # Offline Online-entry errors call this same single-OK modal helper.
        def call_target(address):
            code = read(address, 5)
            self.assertEqual(code[0], 0xe8)
            return address + 5 + struct.unpack('<i', code[1:])[0]
        self.assertEqual(read(0xa62721, 2), bytes.fromhex('31 f6'))  # null options
        self.assertEqual(call_target(0xa62723), 0xa548e0)
        self.assertEqual(call_target(0xa549cb), 0xa056f0)
        self.assertEqual(call_target(0xa05752), 0x9f9790)
        # The input dispatch gate runs before VSC graph listeners or behaviors.
        self.assertEqual(read(0x24de10, 17), prefix + b'\x48')
        # CPartyManager::_isInPrivateLobby is field 15 in its reflected layout.
        field_name = struct.unpack('<Q', read(0x26ae198, 8))[0]
        self.assertEqual(read(field_name, 18), b'_isInPrivateLobby\0')
        data_shift = 0x1d34380 - field_name
        self.assertIn(data_shift, (0, 0x40))
        self.assertEqual(struct.unpack('<H', read(0x1da7f30 - data_shift + 15 * 2, 2))[0], 0x6c)
        # Native setter and party reset use that field on the persistent manager.
        setter = 0x115b6b2 - (0x10 if data_shift else 0)
        code = read(setter, 10)
        self.assertEqual(code[:3], bytes.fromhex('48 8b 0d'))
        self.assertEqual(setter + 7 + struct.unpack('<i', code[3:7])[0], 0x2acf700)
        self.assertEqual(code[7:], bytes.fromhex('88 41 6c'))
        self.assertEqual(read(0xa5b69e, 11), bytes.fromhex('48 8b 05 5b 40 07 02 c6 40 6c 00'))
        # Native participant list and account ID used by matchmaking.
        self.assertEqual(read(0xa178f0, 7), bytes.fromhex('48 8b 05 69 98 0b 02'))
        self.assertEqual(read(0xa1795a, 7), bytes.fromhex('4c 8b ae a8 00 00 00'))
        # Search builder uses the primary local account AND the live party records.
        self.assertEqual(read(0xa5d286, 7), bytes.fromhex('48 8b 0d d3 3e 07 02'))
        self.assertEqual(read(0xa5d2bd, 7), bytes.fromhex('4c 8b be a8 00 00 00'))
        self.assertEqual(read(0xa5d3be, 7), bytes.fromhex('48 8b 05 3b 23 07 02'))
        self.assertEqual(read(0xa5d329, 7), bytes.fromhex('49 8b 47 30 8b 48 0c'))
        self.assertEqual(read(0xa5d357, 4), bytes.fromhex('80 79 2c 00'))
        self.assertEqual(read(0xa5d51e, 4), bytes.fromhex('48 8b 71 18'))
        # startSearch serializes the JSON third argument through bdByteBuffer::writeString.
        self.assertEqual(read(0xda40d, 10), bytes.fromhex('48 8b 7d 98 48 89 de 4c 89 ea'))
        # Join callback adopts the room ID into the native manager used by the asset gate.
        self.assertEqual(read(0xa17112, 7), bytes.fromhex('48 89 83 58 09 00 00'))
        # The manager's destructor is intercepted before it frees the tracked owner.
        self.assertEqual(read(0xa14550, 14), bytes.fromhex('55 48 89 e5 41 57 41 56 41 54 53 48 89 fb'))
        for slot, target in ((0x2549eb0, 0xa148a0), (0x2549eb8, 0xa148b0)):
            self.assertEqual(struct.unpack('<Q', read(slot, 8))[0], target)
        for address in (0xa97a00, 0xa9cbe0):
            self.assertEqual(read(address, 6), bytes.fromhex('55 48 89 e5 53 50'))

    def test_go_to_lobby_graph_contract(self):
        import sys
        sys.path.insert(0, str(ROOT / 'scripts'))
        import gcn_shaders as iga
        from igz_file import Igz
        game = Path(os.environ['BB_TEST_GAME'])
        def graph(archive, suffix):
            pak = game / 'archives' / archive
            entry = next(e for e in iga.pak_entries(pak) if e[0].endswith(suffix))
            with pak.open('rb') as f:
                return Igz(iga.read_entry(f, *entry[1:]))
        button = graph('update.pak', 'octane_lobby_modeselect_gotolobbybutton.igz')
        self.assertIn('igGuiEventFocusInputPressed', button.strings)
        self.assertEqual(dict(button.objects())[0x16d0], 'igVscConstEnumAccessor')
        self.assertEqual(button.u32(0x16d0 + 0x10), 8)
        private = graph('update.pak', 'octane_lobby_privategame_buttonready.igz')
        self.assertIn('Scripts.Graph.Octane_Lobby_PrivateGame_ButtonReady', private.strings)
        self.assertIn('CNetworkNodes::lobbyStartGame_VSC', private.strings)
        self.assertIn('igGuiEventFocusInputPressed', private.strings)
        self.assertEqual(dict(private.objects())[0x15f8], 'igVscConstEnumAccessor')
        self.assertEqual(private.u32(0x15f8 + 0x10), 8)
        root = graph('update.pak', 'octane_lobby_modeselect_root.igz')
        self.assertEqual(root.string_at(0x3240 + 0x10), 'Scripts.Graph.Octane_Lobby_ModeSelect_Root')
        menu = graph('update2.pak', '/ui/octane_lobby_modeselect.igz')
        self.assertEqual(dict(menu.objects())[0x1c48], 'igGuiVscBehavior')
        self.assertEqual(menu.u64(0x1c48 + 0x20), 16)
        external = [(menu.strings[a], menu.strings[b]) for a, b in
                    struct.iter_unpack('<II', menu.fixup('EXNM')[3][:menu.fixup('EXNM')[1] * 8])
                    if not b & 0x80000000]
        self.assertEqual(external[16], ('Octane_Lobby_ModeSelect_GoToLobbyButton',) * 2)
