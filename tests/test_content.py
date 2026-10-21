from paths import ROOT
from pathlib import Path
import struct
import os
import subprocess
import tempfile
import unittest
from unittest.mock import patch
import content_profile

EXE=ROOT/'out/content-test'

class ProfileTests(unittest.TestCase):
    def test_linux_launch_passes_dlc_directory(self):
        # Run the actual profile invocation with a recorder in place of Python.
        script = (ROOT / 'run.sh').read_text()
        start = script.index('"$PYTHON" scripts/content_profile.py')
        command = script[start:].split('\n#', 1)[0]
        for override in ('', '/custom DLC/packages'):
            with self.subTest(override=override):
                env = dict(os.environ, BB_DLC_DIR=override)
                result = subprocess.run(
                    ['bash', '-eu', '-c',
                     'record() { printf "%s\\0" "$@"; }\n'
                     'PYTHON=record; game="/game dump"; data="/user data"; out="$data/out"\n' + command],
                    env=env, capture_output=True, check=True)
                args = result.stdout.decode().rstrip('\0').split('\0')
                self.assertEqual(args[args.index('--dlc-dir') + 1], override or '/user data/dlc')

    def test_sfo_parameters_and_explicit_trial_profile(self):
        with tempfile.TemporaryDirectory() as tmp:
            p=Path(tmp);(p/'sce_sys').mkdir();(p/'sce_sys/param.sfo').write_bytes(b'fixture')
            with patch.object(content_profile,'sfo',return_value={'USER_DEFINED_PARAM_1':13,'USER_DEFINED_PARAM_4':0xffffffff}):
                content_profile.prepare(p,p,'trial')
            self.assertEqual(struct.unpack('<8s6I',(p/'content.bin').read_bytes()),(b'BBCONT02',1,13,0,0,0xffffffff,0))

    def test_dlc_packages_and_folders(self):
        def sfo_bytes(entries):
            keys=data=b'';index=[]
            for k,v in entries:
                vb=v.encode()+b'\0';index.append((len(keys),0x204,len(vb),len(vb),len(data)));keys+=k.encode()+b'\0';data+=vb
            start=20+16*len(entries)
            return struct.pack('<4sIIII',b'\0PSF',0x101,start,start+len(keys),len(entries))+b''.join(struct.pack('<HHIII',*e) for e in index)+keys+data
        def pkg(entries):
            param=sfo_bytes(entries)
            header=bytearray(0x200);header[:4]=b'\x7fCNT'
            struct.pack_into('>I',header,0x10,1);struct.pack_into('>I',header,0x18,0x100)
            struct.pack_into('>IIIIII',header,0x100,0x1000,0,0,0,0x200,len(param))
            return bytes(header)+param
        with tempfile.TemporaryDirectory() as tmp:
            p=Path(tmp);(p/'sce_sys').mkdir()
            (p/'sce_sys/param.sfo').write_bytes(sfo_bytes([('TITLE_ID','CUSA13795')]))
            dlc=p/'dlc';(dlc/'kart/sce_sys').mkdir(parents=True)
            (dlc/'a.pkg').write_bytes(pkg([('CATEGORY','ac'),('CONTENT_ID','UP0002-CUSA13795_00-CTRDELUXEPACK001'),('TITLE','Deluxe'),('TITLE_ID','CUSA13795')]))
            (dlc/'other.pkg').write_bytes(pkg([('CATEGORY','ac'),('CONTENT_ID','UP0000-CUSA00001_00-SOMEOTHERGAME001'),('TITLE_ID','CUSA00001')]))
            (dlc/'broken.pkg').write_bytes(b'not a package')
            (dlc/'kart/sce_sys/param.sfo').write_bytes(sfo_bytes([('CATEGORY','ac'),('CONTENT_ID','UP0002-CUSA13795_00-CTRCODEKART00001'),('TITLE_ID','CUSA13795')]))
            content_profile.prepare(p,p/'out',dlc_dir=dlc)
            blob=(p/'out/content.bin').read_bytes()
            self.assertEqual(blob[:8],b'BBCONT02');self.assertEqual(struct.unpack_from('<I',blob,28)[0],2)
            self.assertEqual({blob[32:48],blob[52:68]},{b'CTRDELUXEPACK001',b'CTRCODEKART00001'})
            with patch.dict('os.environ',{'BB_DLC_DISABLED':'CTRCODEKART00001'}):
                content_profile.prepare(p,p/'out',dlc_dir=dlc)
            self.assertEqual((p/'out/content.bin').read_bytes()[28:52],struct.pack('<I',1)+b'CTRDELUXEPACK001'.ljust(20,b'\0'))
            with patch.dict('os.environ',{'BB_DLC':'0'}):
                content_profile.prepare(p,p/'out',dlc_dir=dlc)
            self.assertEqual(len((p/'out/content.bin').read_bytes()),32)
            # What the launchers list for each item of the dlc folder.
            self.assertEqual(content_profile.describe_addon(dlc/'a.pkg','CUSA13795'),('CTRDELUXEPACK001','Deluxe',''))
            self.assertEqual(content_profile.describe_addon(dlc/'kart','CUSA13795')[0],'CTRCODEKART00001')
            self.assertIn('not an add-on',content_profile.describe_addon(dlc/'other.pkg','CUSA13795')[2])
            self.assertEqual(content_profile.describe_addon(dlc/'broken.pkg','CUSA13795')[2],'not a PS4 package')
            (dlc/'empty').mkdir()
            self.assertIn('param.sfo',content_profile.describe_addon(dlc/'empty','CUSA13795')[2])

    def test_invalid_sfo_parameter_rejected(self):
        with tempfile.TemporaryDirectory() as tmp:
            p=Path(tmp);(p/'sce_sys').mkdir();(p/'sce_sys/param.sfo').write_bytes(b'fixture')
            with patch.object(content_profile,'sfo',return_value={'USER_DEFINED_PARAM_1':'13'}):
                with self.assertRaisesRegex(ValueError,'invalid user-defined'):
                    content_profile.prepare(p,p)

@unittest.skipUnless(EXE.exists(),'run bash build.sh --test first')
class ContentTests(unittest.TestCase):
    def run_case(self,*args):
        return subprocess.run([str(EXE.resolve()),*args],capture_output=True,text=True,timeout=5)

    def test_lifecycle_and_output_boundaries(self):
        r=self.run_case();self.assertEqual(r.returncode,0,r.stdout+r.stderr)

    def test_missing_profile_does_not_report_loaded(self):
        r=self.run_case('--missing');self.assertEqual(r.returncode,21)
        self.assertIn('needs --content-profile',r.stderr)

    def test_other_modules_are_counted_independently(self):
        r=self.run_case('--unknown');self.assertEqual(r.returncode,0,r.stdout+r.stderr)
