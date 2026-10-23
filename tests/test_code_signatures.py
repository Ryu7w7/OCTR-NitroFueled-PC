from paths import ROOT
import hashlib,unittest
from pathlib import Path
from xml.etree import ElementTree as ET
from prepare import parse_self
from patches import compile_external,eboot_segments
from test_prepare import fixture

class CodeSignatureTests(unittest.TestCase):
    def setUp(self):
        self.elf=parse_self(fixture())[0]
    def patch(self,**attributes):
        meta=ET.Element('Metadata')
        ET.SubElement(ET.SubElement(meta,'PatchList'),'Line',Type='bytes',Address='0x400002',Value='9090',**attributes)
        return [('mod',Path('OCTR-Mod-Content.xml'),meta)]
    def compile(self,selected,elf=None):
        elf=self.elf if elf is None else elf
        return compile_external(selected,eboot_segments(elf),elf,strict=True,require_verification=True)
    def test_accepts_unrelated_executable_changes(self):
        selected=self.patch(ExpectedSHA256=hashlib.sha256(bytes([2,3])).hexdigest())
        changed=bytearray(self.elf);changed[0x100e]=99
        self.assertNotEqual(hashlib.sha256(changed).digest(),hashlib.sha256(self.elf).digest())
        self.assertEqual(self.compile(selected,changed),[(2,b'\x90\x90')])
    def test_rejects_changed_patch_site(self):
        selected=self.patch(ExpectedSHA256=hashlib.sha256(bytes([2,3])).hexdigest())
        changed=bytearray(self.elf);changed[0x1002]=99
        with self.assertRaisesRegex(ValueError,'code signature mismatch'):self.compile(selected,changed)
    def test_rejects_unsigned_write(self):
        with self.assertRaisesRegex(ValueError,'unverified executable write'):self.compile(self.patch())
    def test_rejects_non_file_backed_site(self):
        selected=self.patch(ExpectedSHA256=hashlib.sha256(b'\0\0').hexdigest())
        selected[0][2].find('.//Line').set('Address','0x400018')
        with self.assertRaisesRegex(ValueError,'code signature mismatch'):self.compile(selected)
    def test_rejects_malformed_expected_hash(self):
        with self.assertRaisesRegex(ValueError,'invalid ExpectedSHA256'):self.compile(self.patch(ExpectedSHA256='bad'))
    def test_existing_pattern_can_locate_compatible_code(self):
        self.assertEqual(self.compile(self.patch(Signature='02 03')),[(2,b'\x90\x90')])
    def test_signature_and_expected_must_both_match(self):
        with self.assertRaisesRegex(ValueError,'code signature mismatch'):
            self.compile(self.patch(Signature='02 03',ExpectedSHA256=hashlib.sha256(b'xx').hexdigest()))
    def test_expected_hash_pins_fixed_address_payload(self):
        selected=self.patch(Signature='04 05',ExpectedSHA256=hashlib.sha256(bytes([4,5])).hexdigest())
        with self.assertRaisesRegex(ValueError,'fixed-address code signature moved'):
            self.compile(selected)
