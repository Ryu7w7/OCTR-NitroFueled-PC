"""Loose XML imports share the existing compiler without replacing legacy files."""
from paths import ROOT
import unittest
import xml.etree.ElementTree as ET
import test_mod_packages as fixtures
import mod_library as library
from patches import all_external_patches

XML=b'''<Patch><TitleID><ID>CUSA13795</ID></TitleID><Metadata Name="Example" Author="tcosi" AppVer="01.21" AppElf="eboot.bin" isEnabled="true"><PatchList/></Metadata></Patch>'''

class XmlImportTests(unittest.TestCase):
    setUp=fixtures.PackageTests.setUp
    def scan(self):return library.scan(self.mods,self.port/'patches')
    def test_import_is_visible_to_actual_compiler_and_disabled(self):
        (self.mods/'Example.xml').write_bytes(XML)
        self.assertFalse(self.scan()['messages'])
        copied=self.port/'patches/Example.xml'
        self.assertEqual(ET.parse(copied).find('Metadata').get('isEnabled'),'false')
        self.assertEqual(len(all_external_patches([self.port/'patches'],'01.21','CUSA13795')),1)
    def test_identical_legacy_duplicate_is_not_added_twice_or_removed(self):
        legacy=self.port/'patches/Example.xml';legacy.write_bytes(XML)
        source=self.mods/'Example.xml';source.write_bytes(XML)
        self.assertFalse(self.scan()['messages']);source.unlink();self.scan()
        self.assertEqual(legacy.read_bytes(),XML)
    def test_different_legacy_file_is_preserved(self):
        legacy=self.port/'patches/Example.xml';legacy.write_bytes(XML)
        (self.mods/'Example.xml').write_bytes(XML.replace(b'Example',b'Different'))
        self.assertTrue(self.scan()['messages']);self.assertEqual(legacy.read_bytes(),XML)
    def test_removed_source_removes_only_managed_copy(self):
        source=self.mods/'Example.xml';source.write_bytes(XML);self.scan()
        source.unlink();self.scan();self.assertFalse((self.port/'patches/Example.xml').exists())
    def test_user_modified_compiler_copy_is_never_deleted(self):
        source=self.mods/'Example.xml';source.write_bytes(XML);self.scan()
        copied=self.port/'patches/Example.xml';copied.write_bytes(b'user edits')
        source.unlink();self.scan();self.assertEqual(copied.read_bytes(),b'user edits')
    def test_reload_updates_managed_copy_without_duplicate(self):
        source=self.mods/'Example.xml';source.write_bytes(XML);self.scan()
        source.write_bytes(XML.replace(b'Example',b'Updated'));self.scan();self.scan()
        self.assertEqual(ET.parse(self.port/'patches/Example.xml').find('Metadata').get('Name'),'Updated')
    def test_malformed_and_reserved_xml_not_installed(self):
        (self.mods/'Bad.xml').write_text('<Patch>')
        (self.mods/'OCTR-Mod-Library.xml').write_bytes(XML)
        self.assertEqual(len(self.scan()['messages']),2)
        self.assertFalse((self.port/'patches/Bad.xml').exists())

if __name__=='__main__':unittest.main()
