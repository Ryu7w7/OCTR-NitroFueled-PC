"""Read-only PAK inspection; layout based on kishimisu/Crash-NST-Modding-Tool src/pak.
Only extracted copies are written; original archives are never modified.
"""
from pathlib import Path
from contextlib import contextmanager
import struct,lzma,zlib,json,mmap
from mod_file_paths import resolve
class Archive:
 def __init__(self,path):
  self.path=resolve(path)
  self._file=self.path.open('rb')
  self.data=b=mmap.mmap(self._file.fileno(),0,access=mmap.ACCESS_READ)
  u=lambda o:struct.unpack_from('<I',b,o)[0]
  if len(b)<56 or u(0)!=0x1a414749:
   b.close();self._file.close()
   raise ValueError(f'{self.path.name} is not a game archive (a damaged or incomplete copy of the game)')
  try:
   self._index(b,u)
  except (struct.error,UnicodeDecodeError,ValueError) as error:
   # An index that points past the file: an incomplete copy (an interrupted download or
   # extraction) or a file changed by another tool.
   size=len(b);b.close();self._file.close()
   raise ValueError(f'{self.path.name} is damaged or incomplete ({size:,} bytes; its index reads past '
                    f'its end): copy it again from your dump') from error
 def _index(self,b,u):
  self.version=u(4);count=u(12);self.sector=u(16);names=struct.unpack_from('<Q',b,40)[0]
  if count>(len(b)-56)//20 or names+u(48)>len(b) or u(48)<count*4:
   raise ValueError('Truncated archive index')
  name_data=bytes(b[names:names+u(48)])
  self.files=[]
  for i in range(count):
   offset,ordinal,size,compression=struct.unpack_from('<4I',b,56+count*4+i*16)
   offset|=(ordinal&0xff)<<32
   n=u(names+i*4);end=name_data.find(b'\0',n)
   if end<0:raise ValueError('Truncated archive names')
   n=end+1;end=name_data.find(b'\0',n)
   if end<0:raise ValueError('Truncated archive names')
   self.files.append(dict(index=i,path=name_data[n:end].decode(),offset=offset,ordinal=ordinal,size=size,compression=compression))
  p=56+count*20;self.tables=[]
  for fmt,n in zip(('I','H','B'),struct.unpack_from('<3I',b,28)):
   self.tables.append(struct.unpack_from('<'+str(n)+fmt,b,p));p+=n*struct.calcsize(fmt)
 @contextmanager
 def nested(self,entry):
  """Read an uncompressed child archive directly from the parent's mapping."""
  start,size=entry['offset'],entry['size']
  if entry['compression']!=0xffffffff or start<0 or size<56 or start+size>len(self.data):
   raise ValueError('Invalid nested archive range')
  child=object.__new__(Archive)
  child.path=self.path
  child.data=memoryview(self.data)[start:start+size]
  try:
   u=lambda o:struct.unpack_from('<I',child.data,o)[0]
   if u(0)!=0x1a414749:raise ValueError('Invalid nested archive header')
   child._index(child.data,u)
   yield child
  finally:
   child.data.release()
 def extract(self,f):
  b=self.data;off=f['offset'];size=f['size'];comp=f['compression']
  if comp==0xffffffff:return bytes(b[off:off+size])
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
