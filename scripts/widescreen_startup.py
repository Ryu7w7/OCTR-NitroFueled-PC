"""Startup/legal text proportions, scoped to named placeables. Author: tcosi.
SPDX-License-Identifier: GPL-2.0-or-later
"""
import struct, math
class IGZ:
 def __init__(self,data):
  self.data=b=data;self.chunks=[];self.fix={}
  assert b[:8]==bytes.fromhex('015a47490a000000')
  p=16
  while True:
   _,_,o,n=struct.unpack_from('<4I',b,p);p+=16
   if not o:break
   self.chunks.append((o,n))
  p,end=self.chunks[0];end+=p
  while p<end:
   kind=b[p:p+4].decode('ascii');count,size,head=struct.unpack_from('<3I',b,p+4)
   if not size:break
   raw=b[p+head:p+size];result=[]
   if kind in ('TSTR','TMET','TDEP'):
    i=0
    for _ in range(count*(2 if kind=='TDEP' else 1)):
     stop=raw.index(0,i);result.append(raw[i:stop].decode());i=stop+1
     if i<len(raw) and raw[i]==0:i+=1
   elif kind.startswith('R') and kind!='ROOT':
    value=shift=last=0
    for v in (x for byte in raw for x in (byte&15,byte>>4)):
     value|=(v&7)<<shift;shift+=3
     if not v&8:
      last+=value*4;result.append(last);value=shift=0
      if len(result)==count:break
   else:result=list(struct.unpack('<'+str(len(raw)//4)+'I',raw))
   self.fix[kind]=result;p+=size
  self.objects=[]
  for ptr in self.fix['RVTB']:
   o=self.offset(ptr);t=struct.unpack_from('<I',b,o)[0]
   self.objects.append(dict(ptr=ptr,offset=o,type=self.fix['TMET'][t]))
  if self.fix.get('ROOT') and self.fix.get('ONAM'):
   root=self.offset(self.fix['ROOT'][0]);names=self.offset(self.fix['ONAM'][0])
   count=struct.unpack_from('<I',b,root+12)[0]
   po=self.offset(self.q(root+32));pn=self.offset(self.q(names+32))
   objects={x['ptr']:x for x in self.objects}
   for i in range(count):
    ptr=self.q(po+i*8);name=self.fix['TSTR'][self.q(pn+i*16)]
    if ptr in objects:objects[ptr]['name']=name
 def offset(self,ptr):return self.chunks[(ptr>>27)+1][0]+(ptr&0x7ffffff)
 def q(self,o):return struct.unpack_from('<Q',self.data,o)[0]


TEXT = {'Text_legaltext', 'Text_legaltext001', 'Text_legaltext002'}

def correct_text(data, factor):
    """Shrink only startup text x scale; background, sprites and wrapping stay unchanged."""
    if not math.isfinite(factor) or not 0 < factor <= 1:
        raise ValueError('Invalid startup text aspect factor')
    try:
        igz = IGZ(data)
        result = bytearray(data)
        found = set()
        for obj in igz.objects:
            if obj['type'] != 'igGuiPlaceable' or obj.get('name') not in TEXT:
                continue
            at = obj['offset'] + 0x84
            x, y = struct.unpack_from('<2f', data, at)
            if not (math.isfinite(x) and math.isfinite(y) and 0 < x <= 4 and x == y):
                raise ValueError('Unexpected startup text scale')
            struct.pack_into('<f', result, at, x * factor)
            found.add(obj['name'])
        if not {'Text_legaltext', 'Text_legaltext001'} <= found:
            raise ValueError('Startup text placeables were not found')
        return bytes(result)
    except (AssertionError, IndexError, KeyError, UnicodeError, struct.error) as error:
        raise ValueError('Unsupported startup UI layout') from error
