"""localized_strings operations on a small synthetic .lng (IGZ) only; no game data."""
from paths import ROOT
import struct,unittest
import test_mod_packages as fixtures
from test_mod_assets import pak
from igz_file import Igz,write_strings,pack_offsets
from mod_components import sha_bytes
import archive_patch,mod_strings
import mod_format as fmt

FIELD='CRacerBaseData::_displayName'

def lng(texts):
    """An IGZ with one igLocalizedStringData per {(object, field): text}."""
    names=sorted({o for o,_ in texts});fields=sorted({f for _,f in texts})
    strings=list(dict.fromkeys([*texts.values(),*names,*fields,'SystemData','metafield']))
    exnm=b''.join(struct.pack('<II',strings.index(n),strings.index('SystemData')) for n in names)
    exnm+=b''.join(struct.pack('<II',strings.index(f),strings.index('metafield')) for f in fields)
    data=bytearray();starts=[]
    for (o,f),text in texts.items():
        starts.append(len(data))
        data+=struct.pack('<QQQQQ',0,0,strings.index(text),names.index(o),fields.index(f))
    blocks=[('TSTR',len(strings),write_strings(strings)),('TMET',1,write_strings(['igLocalizedStringData'])),
        ('MTSZ',1,struct.pack('<I',40)),('EXNM',len(names)+len(fields),exnm),
        ('RVTB',len(starts),pack_offsets(starts)),('RSTT',len(starts),pack_offsets([s+16 for s in starts])),('ROFS',0,b'')]
    fixups=b''
    for tag,count,raw in blocks:
        raw+=bytes(-len(raw)%4);fixups+=struct.pack('<4sIII',tag.encode(),count,16+len(raw),16)+raw
    header=bytearray(0x800);struct.pack_into('<5I',header,0,0x49475A01,10,0,0,len(blocks))
    struct.pack_into('<4I',header,0x14,0,0x800,len(fixups),16)
    struct.pack_into('<4I',header,0x24,0,0x800+len(fixups),len(data),16)
    return bytes(header)+fixups+bytes(data)

ORIGINAL=lng({('Driver_A',FIELD):'Crash',('Driver_B',FIELD):'Coco',('Skin_A','CMarketplaceItem::_storeDisplayName'):'Crash'})

def names(blob):
    z=Igz(blob);return {k:z.string_at(off+0x10) for k,off in mod_strings.texts(z).items()}

def op(strings,original=ORIGINAL):
    return {'kind':'localized_strings','archive':'update3.pak','asset':'ui/example.igz','index':0,
        'original_sha256':sha_bytes(original),'strings':strings}

class StringTests(unittest.TestCase):
    def test_texts_change_and_unchanged_file_is_identical(self):
        self.assertEqual(mod_strings.apply(ORIGINAL,{}),ORIGINAL)
        out=mod_strings.apply(ORIGINAL,{('Driver_A',FIELD):'Mario',('Driver_B',FIELD):'Sonic ñ'})
        self.assertEqual(names(out),{('Driver_A',FIELD):'Mario',('Driver_B',FIELD):'Sonic ñ',
            ('Skin_A','CMarketplaceItem::_storeDisplayName'):'Crash'})
        with self.assertRaisesRegex(ValueError,'No localized string'):
            mod_strings.apply(ORIGINAL,{('Driver_C',FIELD):'Link'})
    def test_validation(self):
        m={'schemaVersion':2,'id':'names','name':'n','version':'1.0.0','author':'a','description':'d',
           'requiresCapabilities':['localized-strings'],'compatibility':{'online':'allowed'},
           'files':[{'file':fmt.MARKER,'sha256':'0'*64,'bytes':19}],'operations':[op({'Driver_A':{FIELD:'Mario'}})]}
        fmt.validate_manifest(m)
        for bad in ({'Driver_A':{}},{},{'Driver_A':{FIELD:'a\0b'}}):
            with self.assertRaises(ValueError):fmt.validate_manifest(m|{'operations':[op(bad)]})
        with self.assertRaisesRegex(ValueError,'missing capabilities'):fmt.validate_manifest(m|{'requiresCapabilities':[]})
        with self.assertRaises(ValueError):fmt.validate_manifest(m|{'operations':[op({'Driver_A':{FIELD:'M'}})|{'patched_sha256':'0'*64}]})

class ComposeTests(unittest.TestCase):
    setUp=fixtures.PackageTests.setUp
    manifest=fixtures.PackageTests.manifest
    package=fixtures.PackageTests.package
    scan=fixtures.PackageTests.scan
    enable=fixtures.PackageTests.enable
    prepare=fixtures.PackageTests.prepare
    def names_package(self,ident,strings):
        m=self.manifest(ident)|{'requiresCapabilities':['localized-strings'],'operations':[op(strings)]}
        self.package(ident,m)
    def entry(self):
        source=self.game/'archives/update3.pak';patch,=self.port.rglob('*.bbpatch')
        record=archive_patch.view(source,patch,60,16);offset,extra,length,_=struct.unpack('<4I',record)
        return archive_patch.view(source,patch,offset|((extra&255)<<32),length)
    def test_different_texts_of_one_entry_compose(self):
        (self.game/'archives').mkdir();(self.game/'archives/update3.pak').write_bytes(pak(ORIGINAL))
        self.names_package('mario',{'Driver_A':{FIELD:'Mario'}})
        self.names_package('sonic',{'Driver_B':{FIELD:'Sonic'}})
        self.enable(*self.scan()['entries']);self.prepare()
        self.assertEqual(names(self.entry()),{('Driver_A',FIELD):'Mario',('Driver_B',FIELD):'Sonic',
            ('Skin_A','CMarketplaceItem::_storeDisplayName'):'Crash'})
    def test_same_text_conflicts(self):
        (self.game/'archives').mkdir();(self.game/'archives/update3.pak').write_bytes(pak(ORIGINAL))
        self.names_package('mario',{'Driver_A':{FIELD:'Mario'}})
        self.names_package('luigi',{'Driver_A':{FIELD:'Luigi'}})
        self.enable(*self.scan()['entries'])
        with self.assertRaisesRegex(ValueError,'Mod conflicts'):self.prepare()
    def test_whole_entry_edit_conflicts_with_texts(self):
        (self.game/'archives').mkdir();(self.game/'archives/update3.pak').write_bytes(pak(ORIGINAL))
        self.names_package('mario',{'Driver_A':{FIELD:'Mario'}})
        m=self.manifest('edit')|{'requiresCapabilities':['archive-assets'],'operations':[
            {'kind':'archive_asset','archive':'update3.pak','asset':'ui/example.igz','index':0,
             'payload':'payload.bin','original_sha256':sha_bytes(ORIGINAL),'patched_sha256':sha_bytes(b'edit')}]}
        self.package('edit',m,{'payload.bin':b'edit'});self.enable(*self.scan()['entries'])
        with self.assertRaisesRegex(ValueError,'Mod conflicts'):self.prepare()

if __name__=='__main__':unittest.main()
