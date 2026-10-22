"""Read-only PAK inspection; layout based on kishimisu/Crash-NST-Modding-Tool src/pak.
Only extracted copies are written; original archives are never modified.
"""
from pathlib import Path
import struct,lzma,zlib,json,mmap
from mod_file_paths import resolve
class Archive:
 def __init__(self,path):
  self.path=resolve(path)
  self._file=self.path.open('rb')
  self.data=b=mmap.mmap(self._file.fileno(),0,access=mmap.ACCESS_READ)
  u=lambda o:struct.unpack_from('<I',b,o)[0]
  assert u(0)==0x1a414749
  self.version=u(4);count=u(12);self.sector=u(16);names=struct.unpack_from('<Q',b,40)[0]
  self.files=[]
  for i in range(count):
   offset,ordinal,size,compression=struct.unpack_from('<4I',b,56+count*4+i*16)
   offset|=(ordinal&0xff)<<32
   n=names+u(names+i*4);end=b.find(b'\0',n);n=end+1;end=b.find(b'\0',n)
   self.files.append(dict(index=i,path=b[n:end].decode(),offset=offset,ordinal=ordinal,size=size,compression=compression))
  p=56+count*20;self.tables=[]
  for fmt,n in zip(('I','H','B'),struct.unpack_from('<3I',b,28)):
   self.tables.append(struct.unpack_from('<'+str(n)+fmt,b,p));p+=n*struct.calcsize(fmt)
 def extract(self,f):
  b=self.data;off=f['offset'];size=f['size'];comp=f['compression']
  if comp==0xffffffff:return b[off:off+size]
  table,shift=(self.tables[2],7) if size<=127*self.sector else ((self.tables[1],15) if size<=32767*self.sector else (self.tables[0],31))
  parts=[]
  for i in range((size+32767)//32768):
   entry=table[(comp&0xfffffff)+i];p=off+(entry&((1<<shift)-1))*self.sector;n=min(32768,size-i*32768)
   if not entry>>shift:part=b[p:p+n]
   else:
    prefix=4 if self.version>=13 else 2
    length=int.from_bytes(b[p:p+prefix],'little');kind=comp>>28;p+=prefix
    if kind==1:part=zlib.decompress(b[p:p+length],-15)
    elif kind==2:
     props=b[p:p+5];v=props[0];lc=v%9;v//=9;lp=v%5;pb=v//5
     decoder=lzma.LZMADecompressor(format=lzma.FORMAT_RAW,filters=[dict(id=lzma.FILTER_LZMA1,dict_size=struct.unpack_from('<I',props,1)[0],lc=lc,lp=lp,pb=pb)])
     part=decoder.decompress(b[p+5:p+5+length],max_length=n)
    else:raise ValueError(kind)
   assert len(part)>=n,(f['path'],i,len(part),n)
   parts.append(part[:n])
  return b''.join(parts)
