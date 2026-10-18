from paths import ROOT
"""Declared resolution defaults, preserved preferences and capability compatibility."""
import copy,unittest
from nitro_variant_settings import choose,validate
import nitro_format as nf

class VariantSettings(unittest.TestCase):
    def declaration(self,wide=False):
        outputs=['5120x1440','3840x1080'] if wide else ['3440x1440','2560x1080']
        return {'variantSettings':{'output_res':{'default':outputs[0],'allowed':outputs},
            'render_res':{'default':'1080','allowed':['720','1080','1440']}}}
    def test_default16x9_selects21x9_before_variants(self):
        base={'output_res':'1920x1080','render_res':'1080'}
        result,changed=choose([self.declaration()],base)
        self.assertEqual(result['output_res'],'3440x1440');self.assertEqual(base['output_res'],'1920x1080')
        self.assertEqual(changed['output_res']['session'],'3440x1440')
    def test_default16x9_selects32x9_before_variants(self):
        result,_=choose([self.declaration(True)],{'output_res':'1920x1080','render_res':'1080'})
        self.assertEqual(result['output_res'],'5120x1440')
    def test_compatible_output_and_internal_preferences_are_preserved(self):
        base={'output_res':'2560x1080','render_res':'720'}
        result,changes=choose([self.declaration()],base)
        self.assertEqual(result,base);self.assertEqual(changes,{})
    def test_switching_aspects_chooses_new_mod_default(self):
        result,_=choose([self.declaration(True)],{'output_res':'3440x1440','render_res':'1440'})
        self.assertEqual(result,{'output_res':'5120x1440','render_res':'1440'})
    def test_disabled_mod_has_no_resolution_effect(self):
        base={'output_res':'1920x1080','render_res':'1080'}
        self.assertEqual(choose([],base),(base,{}))
    def test_missing_or_invalid_internal_uses_declared_default(self):
        for value in [None,'native','9999']:
            base={'output_res':'3440x1440'}
            if value is not None:base['render_res']=value
            self.assertEqual(choose([self.declaration()],base)[0]['render_res'],'1080')
    def test_unrelated_mod_preserves_render_selection(self):
        result,_=choose([{'id':'reserves'},self.declaration()],{'output_res':'1920x1080','render_res':'720'})
        self.assertEqual(result,{'output_res':'3440x1440','render_res':'720'})
    def test_incompatible_mod_domains_rejected_without_last_wins(self):
        with self.assertRaisesRegex(ValueError,'incompatible'):choose([self.declaration(),self.declaration(True)],{})
    def test_disagreeing_defaults_rejected_unless_saved_choice_compatible(self):
        one=self.declaration();two=copy.deepcopy(one);two['variantSettings']['output_res']['default']='2560x1080'
        with self.assertRaisesRegex(ValueError,'disagree'):choose([one,two],{'render_res':'1080'})
        self.assertEqual(choose([one,two],{'output_res':'3440x1440','render_res':'1080'})[1],{})
    def test_invalid_declarations_rejected(self):
        for field in [{}, {'fullscreen':{'default':'1','allowed':['1']}},
                      {'output_res':{'default':'3440x1440','allowed':['1920x1080']}},
                      {'render_res':{'default':'9999','allowed':['9999']}}]:
            with self.subTest(field=field),self.assertRaises(ValueError):validate(field)
    def test_resolve_uses_selected_height_then_retains_compiler_override(self):
        m=self.declaration();m['requiresCapabilities']=['variants']
        m['settings']={'render_res':'1080'}
        m['variants']=[{'id':'720','when':{'output_res':['3440x1440','2560x1080'],'render_res':['720']}},
                       {'id':'1080','when':{'output_res':['3440x1440','2560x1080'],'render_res':['1080','1440']}}]
        name,flat=nf.resolve(m,{'output_res':'1920x1080','render_res':'720'})
        self.assertEqual(name,'720');self.assertEqual(flat['settings']['render_res'],'1080')

if __name__=='__main__':unittest.main(verbosity=2)
