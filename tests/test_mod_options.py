"""Configurable package selection, persistence and read-only overlay regression tests."""
from paths import ROOT
import copy,json,unittest
import xml.etree.ElementTree as ET
import test_mod_packages as fixtures
import mod_format as fmt
import mod_options as options
import mod_library as library
from mod_components import sha,sha_bytes,save_json

class ModOptionTests(unittest.TestCase):
    setUp=fixtures.PackageTests.setUp
    manifest=fixtures.PackageTests.manifest
    package=fixtures.PackageTests.package
    scan=fixtures.PackageTests.scan
    enable=fixtures.PackageTests.enable
    prepare=fixtures.PackageTests.prepare
    read_game=fixtures.PackageTests.read_game

    def configured(self):
        m=self.manifest()
        m['requiresCapabilities']=['mod-options','variants','game-files']
        m['configuration']={
            'placement':{'type':'choice','label':'Position & layout','description':'Choose a position.',
                         'default':'corner','choices':{'center':'Centered','corner':'In the corner'}},
            'all-huds':{'type':'boolean','label':'Show in all HUDs','default':False}}
        m['variants']=[]
        for position in ('center','corner'):
            for all_huds in (False,True):
                value=(position+str(all_huds)).encode()
                m['variants'].append({'id':position+('-all' if all_huds else '-speedo'),
                    'whenOptions':{'placement':[position],'all-huds':[all_huds]},
                    'operations':[{'kind':'game_file','target':'data.txt','original_sha256':sha_bytes(b'original'),
                                   'patched_sha256':sha_bytes(value),'payload':position+str(all_huds)+'.txt'}]})
        files={p+str(b)+'.txt':(p+str(b)).encode() for p in ('center','corner') for b in (False,True)}
        return m,files

    def install(self):
        m,files=self.configured();package=self.package(manifest=m,files=files);self.scan();self.enable('sample')
        return package

    def test_damaged_options_file_keeps_mods_usable(self):
        package=self.install()
        path=self.port/'mod-state/options.json'
        for broken in ('{ broken','[]','{"version":9,"mods":{}}'):
            path.write_text(broken,encoding='utf-8')
            self.assertEqual(options.load(path),{})
            self.scan()
        self.configure(package)         # saving replaces the damaged file
        self.assertEqual(options.load(path)['sample'],{'placement':'center','all-huds':True})

    def test_saving_reads_settings_as_a_launch_does(self):
        m,files=self.configured()
        for variant in m['variants']:variant['when']={'widescreen_layout':['off']}
        package=self.package(manifest=m,files=files);self.scan();self.enable('sample')
        ini=self.port/'bbport.ini'
        ini.write_text('output_res=1920x1080\n',encoding='utf-8')
        self.configure(package)         # the line is missing: 'off', as in mod_content.prepare
        ini.unlink()
        self.configure(package,placement='corner')      # no settings file yet

    def configure(self,package,placement='center',all_huds='true'):
        return library.configure(self.mods,self.port/'patches','sample',sha(package),
                                 {'placement':placement,'all-huds':all_huds})

    def test_default_and_each_combination_resolve_exactly_one(self):
        m,files=self.configured()
        self.assertEqual(fmt.resolve(m,{})[0],'corner-speedo')
        for values in options.combinations(m):
            ident,flat=fmt.resolve(m,{},values)
            self.assertTrue(ident.startswith(values['placement']))
            self.assertEqual(ident.endswith('-all'),values['all-huds'])

    def test_options_save_catalog_and_apply_without_touching_source(self):
        package=self.install();before=package.read_bytes()
        self.prepare();self.assertEqual(self.read_game('data.txt'),b'cornerFalse')
        self.configure(package)
        meta=ET.parse(self.port/'patches'/library.FILENAME).find('Metadata')
        controls=meta.findall('ModOption')
        self.assertEqual([x.get('Value') for x in controls],['center','true'])
        self.assertEqual(controls[0].get('Label'),'Position & layout')
        self.prepare();self.assertEqual(self.read_game('data.txt'),b'centerTrue')
        self.assertEqual((self.game/'data.txt').read_bytes(),b'original')
        self.assertEqual(package.read_bytes(),before)

    def test_upgrade_keeps_choices_and_stale_values_use_defaults(self):
        package=self.install();self.configure(package)
        m,files=self.configured();m['version']='1.1.0'
        self.package(manifest=m,files=files,filename='update.zip')
        self.assertEqual(self.scan()['entries']['sample']['option_values'],{'placement':'center','all-huds':True})
        save_json(self.port/'mod-state/options.json',{'version':1,'mods':{'sample':{'placement':'removed','all-huds':'true','old-option':1}}})
        self.assertEqual(self.scan()['entries']['sample']['option_values'],{'placement':'corner','all-huds':False})

    def test_stale_ui_cannot_save_changed_package(self):
        package=self.install();digest=sha(package)
        m,files=self.configured();m['version']='1.1.0'
        self.package(manifest=m,files=files,filename='update.zip')
        with self.assertRaisesRegex(ValueError,'Mod changed'):
            library.configure(self.mods,self.port/'patches','sample',digest,{'placement':'center','all-huds':'true'})
        self.assertFalse((self.port/'mod-state/options.json').exists())

    def test_bad_values_are_rejected_without_changing_saved_choices(self):
        package=self.install();self.configure(package)
        path=self.port/'mod-state/options.json';before=path.read_bytes()
        for values in ({'placement':'center'},{'placement':'unknown','all-huds':'true'},
                       {'placement':'corner','all-huds':'1'}, {'placement':'corner','all-huds':'false','extra':'x'}):
            with self.assertRaises(ValueError):library.configure(self.mods,self.port/'patches','sample',sha(package),values)
            self.assertEqual(path.read_bytes(),before)

    def test_invalid_declarations_and_incomplete_or_overlapping_variants(self):
        m,_=self.configured()
        for change in ('default','type','capability','coverage','overlap','unknown-key','boolean-type'):
            candidate=copy.deepcopy(m)
            # Add the marker normally added by the package builder.
            candidate['files']=[{'file':fmt.MARKER,'sha256':sha_bytes(fmt.MARKER_CONTENT),'bytes':len(fmt.MARKER_CONTENT)}]
            # No file payload operations are needed for schema validation here.
            for variant in candidate['variants']:variant.pop('operations')
            if change=='default':candidate['configuration']['placement']['default']=[]
            if change=='type':candidate['configuration']['all-huds']['type']='script'
            if change=='capability':candidate['requiresCapabilities'].remove('mod-options')
            if change=='coverage':candidate['variants'].pop()
            if change=='overlap':candidate['variants'][0]['whenOptions'].pop('all-huds')
            if change=='unknown-key':candidate['variants'][0]['whenOptions']['unknown']=['x']
            if change=='boolean-type':candidate['variants'][0]['whenOptions']['all-huds']=[0]
            with self.subTest(change=change),self.assertRaises(ValueError):fmt.validate_manifest(candidate)

    def test_unconfigured_package_has_no_controls(self):
        self.package();self.scan()
        meta=ET.parse(self.port/'patches'/library.FILENAME).find('Metadata')
        self.assertEqual(meta.findall('ModOption'),[])

    def test_advanced_metadata_is_optional_and_round_trips(self):
        m,files=self.configured();m['configuration']['all-huds']['advanced']=True
        self.package(manifest=m,files=files);self.scan()
        controls=ET.parse(self.port/'patches'/library.FILENAME).findall('Metadata/ModOption')
        self.assertEqual([x.get('Advanced') for x in controls],['false','true'])
        m['configuration']['all-huds']['advanced']='true'
        with self.assertRaises(ValueError):fmt.expanded_variants(m)

    def test_independent_visibility_combinations_and_mixed_game_settings(self):
        m=self.manifest();m['requiresCapabilities']=['variants','mod-options']
        m['files']=[{'file':fmt.MARKER,'sha256':sha_bytes(fmt.MARKER_CONTENT),'bytes':len(fmt.MARKER_CONTENT)}]
        m['configuration']={'placement':{'type':'choice','label':'Position','default':'default',
            'choices':{'default':'Default','center':'Center','corner':'Corner'}}}
        for i in range(6):m['configuration']['view-'+str(i)]={'type':'boolean','label':'View '+str(i),'default':False,'advanced':True}
        m['variants']=[{'id':'variant-'+str(i),'whenOptions':{k:[v] for k,v in values.items()}}
                       for i,values in enumerate(options.combinations(m))]
        self.assertEqual(len(m['variants']),192);fmt.validate_manifest(m)
        self.assertEqual(len({fmt.resolve(m,{},v)[0] for v in options.combinations(m)}),192)
        for v in m['variants']:v['when']={'render_res':['1080']}
        fmt.validate_manifest(m)
        self.assertEqual(fmt.resolve(m,{'render_res':'1080'})[0],'variant-0')
        with self.assertRaises(ValueError):fmt.resolve(m,{'render_res':'720'})

    def test_grid_catalog_and_invalid_cells(self):
        m,files=self.configured()
        m['configuration']['all-huds'].update(row='Classic',column='CNK Meter')
        self.package(manifest=m,files=files);self.scan()
        control=ET.parse(self.port/'patches'/library.FILENAME).find("Metadata/ModOption[@Id='all-huds']")
        self.assertEqual((control.get('Row'),control.get('Column')),('Classic','CNK Meter'))
        for changes in ({'row':'Classic'},{'row':'Classic','column':'CNK Meter','type':'choice'},
                        {'row':'Classic','column':'CNK Meter','advanced':True}):
            rules={'first':{'type':'boolean','label':'Test','default':False}|changes}
            with self.assertRaises(ValueError):options.validate(rules)
        rules={'a':{'type':'boolean','label':'Test','default':False,'row':'Classic','column':'CNK'}}
        with self.assertRaises(ValueError):options.validate(rules|{'b':dict(rules['a'])})

    def test_independent_patch_and_art_selection_without_variant_multiplication(self):
        m,files=self.configured();m['requiresCapabilities']+=['xml-patches','code-signatures']
        xml=b'<Patch><TitleID><ID>CUSA13795</ID></TitleID><Metadata AppVer="01.21" AppElf="eboot.bin"><PatchList><Line Type="bytes" Address="0x400000" Value="01" ExpectedSHA256="'+b'0'*64+b'"/></PatchList></Metadata></Patch>'
        files.update({'on.xml':xml,'off.xml':xml,'extra.txt':b'extra'})
        m['operationSets']={'extra':[{'kind':'game_file','target':'extra.txt','original_sha256':None,'patched_sha256':sha_bytes(b'extra'),'payload':'extra.txt'}]}
        m['configuration']['stock']={'type':'boolean','label':'Stock','default':True,
            'patches':{'true':['on.xml'],'false':['off.xml']},'useOperations':{'true':[],'false':['extra']}}
        self.package(manifest=m,files=files);catalog=self.scan();self.assertFalse(catalog['messages'])
        for flag in (False,True):
            ident,flat=fmt.resolve(m,{}, {'stock':flag})
            self.assertEqual(ident,'corner-speedo')
            self.assertEqual(flat['patches'],['on.xml' if flag else 'off.xml'])
            self.assertEqual(len(flat['operations']),1 if flag else 2)
        # Two controls may request the same named art set. Apply it once so
        # centered layout plus a hidden stock meter cannot conflict with itself.
        for v in m['variants']:
            if v['whenOptions']['placement']==['center']:v['useOperations']=['extra']
        m['version']='1.0.1';self.package(manifest=m,files=files,filename='deduplicated.zip')
        for flag in (False,True):
            _,flat=fmt.resolve(m,{}, {'stock':flag,'placement':'center'})
            self.assertEqual(len(flat['operations']),2)
        bad=copy.deepcopy(m);bad['configuration']['stock']['patches']['false']=['../escape.xml']
        with self.assertRaises(ValueError):options.validate(bad['configuration'])
        bad=copy.deepcopy(m);bad['configuration']['stock']['useOperations']['false']=['missing']
        with self.assertRaises(ValueError):fmt.expanded_variants(bad)
        bad=copy.deepcopy(m);bad.pop('variants');bad.pop('operationSets')
        with self.assertRaisesRegex(ValueError,'requires variants'):fmt.expanded_variants(bad)

    def test_nine_boolean_cells_fit_bounded_configuration(self):
        rules={'placement':{'type':'choice','label':'Placement','default':'default','choices':{'default':'Default','center':'Center','corner':'Corner'}}}
        for row in ('Classic','Speedometer','Nitro-Fueled'):
            for column in ('Drift','Reserves','CNK'):
                rules[(row+'-'+column).lower()]={'type':'boolean','label':column,'default':True,'row':row,'column':column}
        options.validate(rules);self.assertEqual(len(options.combinations({'configuration':rules})),1536)
        rules['extra']={'type':'boolean','label':'Extra','default':False}
        with self.assertRaises(ValueError):options.validate(rules)

if __name__=='__main__':unittest.main()
