"""CPU-only native gravity/contact checks; never starts the game or GPU."""
import ctypes as C
import math
import os
import struct
import subprocess
import tempfile
import threading
import unittest
from pathlib import Path
from prepare import parse_self

F=lambda x:C.c_float(x).value
REG={k:512+8*i for i,k in enumerate('r15 r14 r13 r12 rbp rbx r11 r10 r9 r8 rdi rsi rdx rcx rax flags resume'.split())}
P=C.c_void_p
def buffer(size):
    b=C.create_string_buffer(size)
    assert C.addressof(b)%16==0
    return b
def ptr(b,offset,value): C.c_void_p.from_address(C.addressof(b)+offset).value=value
def flt(b,offset,value): C.c_float.from_address(C.addressof(b)+offset).value=value
def u16(b,offset,value): C.c_uint16.from_address(C.addressof(b)+offset).value=value
def floats(b,offset): return list((C.c_float*4).from_address(C.addressof(b)+offset))

class Flight:
    def __init__(self,native):
        self.n=native
        self.params=C.CFUNCTYPE(None,P,C.c_float,C.c_float)(native.base+0x6e7a20)
        self.setup=C.CFUNCTYPE(None,P,P,C.c_int,C.c_int,C.c_float,C.c_float)(native.base+0x6e7af0)
        self.prepare=C.CFUNCTYPE(None,P,P,P,C.c_int,P,P)(native.base+0x80ec70)
        self.accumulate=C.CFUNCTYPE(None,P,P,P,P,C.c_int,P,P)(native.base+0x878840)
        self.finish=C.CFUNCTYPE(None,P,P,P,C.c_int,P,P,P,P,P,P,P,C.c_int,C.c_int)(native.base+0x878bb0)
        # Return after native position/quaternion writes, before world notification/broadphase.
        C.memmove(native.base+0x8791ac,b'\xe9'+struct.pack('<i',0x879bcc-(0x8791ac+5)),5)
        self.world=buffer(0x700);self.motions=buffer(0x100);self.props=buffer(0x40)
        self.props_manager=buffer(0x40);self.context=buffer(0x40);self.modifiers=buffer(0x480)
        self.A=buffer(0x20);self.B=buffer(0x20);self.task=buffer(0xa0)
        self.gravity=buffer(0x10);self.index=C.c_int(1)
        self.info=C.addressof(self.world)+0x330
        ptr(self.world,0xe0,C.addressof(self.motions))
        ptr(self.world,0x5d0,C.addressof(self.props_manager))
        ptr(self.props_manager,0x28,C.addressof(self.props))
        ptr(self.context,0,C.addressof(self.world))
        ptr(self.context,0x30,C.addressof(self.modifiers))
        ptr(self.task,0x28,self.info)
        flt(self.props,0x8,1)
        flt(self.props,0x10,1e12);flt(self.props,0x14,1e12)
        u16(self.motions,0x80+0x3a,0x7f80)
        u16(self.motions,0x80+0x3c,0x7f80)
        flt(self.motions,0x80+0x1c,1)

    def reset(self,velocity,gravity):
        for i in range(3):
            flt(self.motions,0x80+i*4,0)
            flt(self.motions,0x80+0x40+i*4,velocity[i])
            flt(self.gravity,i*4,gravity[i])


class Native:
 def __init__(self,elf):
  elf=bytes(elf)
  phoff=struct.unpack_from('<Q',elf,32)[0];phsize,phnum=struct.unpack_from('<HH',elf,54)
  segments=[struct.unpack_from('<II6Q',elf,phoff+i*phsize) for i in range(phnum)]
  self.size=max(s[3]+s[6] for s in segments if s[0]==1)
  self.libc=C.CDLL(None);self.libc.mmap.restype=P
  self.libc.mmap.argtypes=[P,C.c_size_t,C.c_int,C.c_int,C.c_int,C.c_long]
  self.base=self.libc.mmap(None,self.size,7,0x22,-1,0);assert self.base!=P(-1).value
  for s in segments:
   if s[0]==1:C.memmove(self.base+s[3],elf[s[2]:s[2]+s[5]],s[5])
  self.cookie=C.c_uint64(0x1234)
  displacement=C.c_int32.from_address(self.base+0xe4a234).value
  C.c_void_p.from_address(self.base+0xe4a238+displacement).value=C.addressof(self.cookie)
 def close(self):
  self.libc.munmap.argtypes=[P,C.c_size_t];self.libc.munmap(self.base,self.size)

class Contact:
 def __init__(self,n):
  self.n=n;self.flight=Flight(n)
  self.solve=C.CFUNCTYPE(P,P,P,P,P,P,P,P,P,P)(n.base+0x8c3620)
  self.stepinfo=C.CFUNCTYPE(None,P,P,C.c_int,C.c_int)(n.base+0x6f6d30)
  self.range=buffer(0x40);self.block=buffer(0x1000);self.results=buffer(0x1000)
  self.stage=buffer(0x40);self.static=buffer(0x20)
  self.jac=C.addressof(self.block)+0x20
  ptr(self.range,0,C.addressof(self.block));u16(self.range,0xa,1)
  C.c_int.from_address(C.addressof(self.range)+0xc).value=1
  ptr(self.range,0x20,C.addressof(self.results));u16(self.range,0x2a,1)
  C.c_int.from_address(C.addressof(self.range)+0x2c).value=1
  C.c_ubyte.from_address(n.base+0x8c366b+C.c_int32.from_address(n.base+0x8c3666).value).value=1
  self.setnormal((0,1,0))
  C.c_ubyte.from_address(self.jac+0x32).value=1
  C.c_float.from_address(self.jac+0x34).value=1e20
  C.c_float.from_address(self.jac+0x70).value=1
  C.c_float.from_address(self.jac+0x74).value=0
  u16(self.flight.motions,0x80+0x26,0x3f80)
 def setnormal(self,normal):
  for i,v in enumerate(normal):C.c_float.from_address(self.jac+0x50+4*i).value=v
 def project(self,iteration=0):
  self.stepinfo(self.stage,self.flight.info,iteration,0)
  self.solve(None,self.flight.context,self.flight.task,self.stage,self.range,self.flight.A,self.static,None,None)

SITES=[(0x80eded,14),(0x878b3a,16),(0x81ae17,15),(0x8c37f0,20),(0x854483,17),(0x8790b7,17)]
class Runtime:
 def __init__(self,n,library):
  self.lib=C.CDLL(str(library))
  self.lib.freeflight_test_setup.argtypes=[P];self.lib.freeflight_test_setup(n.base)
  self.lib.freeflight_test_dt.argtypes=[C.c_float]
  self.lib.freeflight_test_helper.argtypes=[C.c_uint];self.lib.freeflight_test_helper.restype=P
  self.helpers=[C.CFUNCTYPE(None,P)(self.lib.freeflight_test_helper(i)) for i in range(6)]
  self.lib.freeflight_test_contract.argtypes=[P,C.c_size_t];self.lib.freeflight_test_install.argtypes=[P,P]
  self.lib.freeflight_test_layout.argtypes=[C.c_uint,P,P]
  assert self.lib.freeflight_test_contract(n.base,n.size)==1
  layouts=[];address=C.c_size_t();length=C.c_size_t();i=0
  while self.lib.freeflight_test_layout(i,C.byref(address),C.byref(length)):
   layouts.append((address.value,length.value));i+=1
  self.signatures=SITES+layouts
  for site,length in self.signatures:
   for j in range(length):
    original=C.c_ubyte.from_address(n.base+site+j).value;C.c_ubyte.from_address(n.base+site+j).value=original^1
    assert self.lib.freeflight_test_contract(n.base,n.size)==0
    C.c_ubyte.from_address(n.base+site+j).value=original
  code=n.libc.mmap(None,3072+64,7,0x22,-1,0);assert code!=P(-1).value;C.memset(code,0xcc,3072+64)
  self.lib.freeflight_test_install(n.base,code)
  assert C.string_at(code+3072,64)==b'\xcc'*64
  self.code=code
  self.enabled=True
 def dt(self,dt):self.lib.freeflight_test_dt(F(dt) if self.enabled else F(1/30))
 def support(self,c,variant):
  c.solver_manager=buffer(0x600);c.objects=[buffer(8),buffer(8)];c.tables=[buffer(0x40),buffer(0x40)]
  for i,entry in enumerate([0x81ace0 if variant=='scalar' else 0x8c3620,0x859190]):
   ptr(c.tables[i],0x30,c.n.base+entry);ptr(c.objects[i],0,C.addressof(c.tables[i]));ptr(c.solver_manager,0x568+i*8,C.addressof(c.objects[i]))
  ptr(c.flight.world,0x150,C.addressof(c.solver_manager))
def configure(c,variant):
 c.variant=variant;c.target_offset=0x74
 if variant=='scalar':
  c.solve=C.CFUNCTYPE(P,P,P,P,P,P,P,P,P,P)(c.n.base+0x81ace0)
  a=c.n.base+0x81ad24;C.c_ubyte.from_address(a+7+C.c_int32.from_address(a+2).value).value=1
  stub=c.n.base+0x81aeb9+C.c_int32.from_address(c.n.base+0x81aeb5).value
  assert C.string_at(stub,2)==b'\xff\x25'
  C.c_void_p.from_address(stub+6+C.c_int32.from_address(stub+2).value).value=C.cast(c.n.libc.memset,P).value
  C.memset(c.jac,0,0x150);C.c_ubyte.from_address(c.jac).value=1;C.c_ubyte.from_address(c.jac+2).value=1
  C.c_float.from_address(c.jac+0x18).value=1e20;C.c_float.from_address(c.jac+0x4c).value=1
  u16(c.range,0x2a,2);C.c_int.from_address(C.addressof(c.range)+0x2c).value=2
  c.target_offset=0x5c
 if variant=='generic':
  c.flight.B=buffer(0x40);ptr(c.flight.task,0x88,C.addressof(c.flight.B));c.cells=[buffer(8),buffer(8)]
  for i,cell in enumerate(c.cells):
   C.c_int.from_address(C.addressof(cell)).value=i;C.c_int.from_address(C.addressof(cell)+4).value=1
  c.solve=C.CFUNCTYPE(P,P,P,P,P,P,P,P,P,P)(c.n.base+0x859190)
  a=c.n.base+0x8591b1;C.c_ubyte.from_address(a+7+C.c_int32.from_address(a+2).value).value=1
  stub=c.n.base+0x81aeb9+C.c_int32.from_address(c.n.base+0x81aeb5).value;C.c_void_p.from_address(stub+6+C.c_int32.from_address(stub+2).value).value=C.cast(c.n.libc.memset,P).value
  C.memset(c.jac,0,0x150);C.c_ubyte.from_address(c.jac+0xf).value=5;C.c_ubyte.from_address(c.jac+0x20).value=5
  C.c_float.from_address(c.jac+0x3c).value=1
  u16(c.range,0x2a,2);C.c_int.from_address(C.addressof(c.range)+0x2c).value=2
  C.c_ubyte.from_address(C.addressof(c.range)+0x18).value=1
  C.c_int.from_address(C.addressof(c.results)+0x20).value=1
  # Omit only post-solve cache export, which requires a complete world.
  C.memset(c.n.base+0x858182,0x90,5);c.target_offset=0x2c

def normal(c,v):
 for i,x in enumerate(v):C.c_float.from_address(c.jac+(0x20 if c.variant in ['scalar','generic'] else 0x50)+4*i).value=x

 if c.variant=='generic':C.c_ubyte.from_address(c.jac+0x20).value=5

def frame(c,dt,iterations=4,contact=False,target=0,impulse=None):
 f=c.flight;f.params(f.info,.6,1);f.setup(f.info,f.gravity,iterations,1,.1,F(dt));f.prepare(f.context,f.world,C.byref(f.index),1,f.A,f.B)
 for i in range(iterations):
  if contact or (impulse is not None and i==0):
   C.c_float.from_address(c.jac+c.target_offset).value=impulse if impulse is not None and i==0 else target
   if c.variant=='generic':
    c.stepinfo(c.stage,f.info,i,0);c.solve(None,f.context,f.task,c.stage,c.range,f.A,c.static,c.cells[0],c.cells[1])
   else:c.project(i)
  if i+1<iterations:f.accumulate(None,None,f.A,f.B,1,f.info,f.props)
 f.finish(None,f.A,f.B,1,f.motions,None,f.props,f.task,None,None,None,0,0)
 return dict(position=floats(f.motions,0x80)[:3],velocity=floats(f.motions,0xc0)[:3])

def matrix(n,rt):
 rows=[];worst=0
 for variant in ['scalar','optimized','generic']:
  c=Contact(n);configure(c,variant);rt.support(c,variant)
  for scale in [.125,.5,1,2]:
   for iterations in [1,2,4,8]:
    for scenario in ['freefall','grounded','wall','impulse','zero_impulse','constant','release']:
     results={}
     for enabled in [False,True]:
      rt.enabled=enabled
      for rate in [30,60,120,240,0]:
       normal(c,(1,0,0) if scenario=='wall' else (0,1,0))
       initial=(30,0,40) if scenario=='constant' else (0,0,0)
       gravity=(0,0,0) if scenario in ['impulse','zero_impulse','constant'] else (0,-980,0)
       c.flight.reset(initial,gravity)
       schedule=[F(1/rate)]*(rate//2) if rate else [F(1/240),F(1/80),F(1/60)]*15
       elapsed=0
       for i,dt in enumerate(schedule):
        rt.dt(dt)
        oncontact=scenario in ['grounded','wall','zero_impulse'] or (scenario=='release' and elapsed<1/6-1e-6)
        result=frame(c,dt*scale,iterations,contact=oncontact,impulse=200 if scenario=='impulse' and i==0 else None)
        elapsed+=dt
       assert rt.lib.freeflight_test_active()==0
       row=dict(variant=variant,scale=scale,iterations=iterations,scenario=scenario,enabled=enabled,rate=rate,**result);rows.append(row);results[enabled,rate]=row
     for rate in [30,60,120,240,0]:
      raw=results[False,rate];fixed=results[True,rate];ref=results[False,30]
      if rate==30 or scenario in ['grounded','wall','zero_impulse']:
       assert fixed['position']==raw['position'] and fixed['velocity']==raw['velocity'],(variant,scenario,scale,iterations,rate,raw,fixed)
      if scenario in ['freefall','constant','impulse','release']:
       gap=max(abs(a-b) for a,b in zip(fixed['position'],ref['position']));worst=max(worst,gap)
       assert gap<.005,(variant,scenario,scale,iterations,rate,gap,fixed,ref)
  print(variant,'done',len(rows),'worst',worst,flush=True)
 return len(rows),worst

def controls(n,rt):
 def body(variant='scalar'):
  c=Contact(n);configure(c,variant);rt.support(c,variant);normal(c,(0,1,0));return c
 c=body();checks=[]
 def run(c,raw=1/240,enabled=True,kind='free',count=30):
  rt.enabled=enabled;c.flight.reset((0,0,0),(0,-980,0))
  for _ in range(count):
   rt.dt(raw);result=frame(c,F(1/240),contact=kind=='contact')
  return result
 for raw in [0,-1,float('nan'),float('inf'),1/15,1/30,273/8192,274/8192]:
  assert run(c,raw)==run(c,raw,False)
  checks.append('native fallback raw_dt='+str(raw))
 for kind in ['linear_damping','angular_damping','solver_velocity_stabilization','unknown_solver','changed_solver']:
  if kind=='linear_damping':flt(c.flight.props,0x18,.5)
  if kind=='angular_damping':flt(c.flight.props,0x1c,.5)
  if kind=='solver_velocity_stabilization':flt(c.flight.props,0x24,.5)
  if kind=='unknown_solver':ptr(c.solver_manager,0x578,C.addressof(c.objects[0]))
  if kind=='changed_solver':ptr(c.tables[0],0x30,n.base+0x878840)
  assert run(c)==run(c,enabled=False),kind
  checks.append(kind+' fallback')
  flt(c.flight.props,0x18,0);flt(c.flight.props,0x1c,0);flt(c.flight.props,0x24,0)
  ptr(c.solver_manager,0x578,0);ptr(c.tables[0],0x30,n.base+0x81ace0)
 # Capture is bounded. Saturation must never reuse an active entry for another body.
 rt.lib.freeflight_test_setup(n.base);rt.dt(1/240);f=c.flight
 f.reset((0,0,0),(0,-980,0));f.params(f.info,.6,1);f.setup(f.info,f.gravity,4,1,.1,F(1/240))
 regs=buffer(648)
 ptr(regs,REG['rsi'],C.addressof(f.world));ptr(regs,REG['r15'],C.addressof(f.motions));ptr(regs,REG['rdi'],0x80)
 flt(regs,4,-980/240/4)
 for i in range(2048):
  ptr(regs,REG['r8'],0x10000000+i*32);rt.helpers[0](regs)
 assert rt.lib.freeflight_test_active()==2048
 assert run(c)==run(c,enabled=False)
 checks.append('table saturation fallback')
 rt.lib.freeflight_test_setup(n.base)
 # Motion ID 0 has no matching final call; never leave a reserved-body capture active.
 f=c.flight;f.index.value=0
 f.prepare(f.context,f.world,C.byref(f.index),1,f.A,f.B)
 assert rt.lib.freeflight_test_active()==0
 f.index.value=1;checks.append('reserved static motion0 has no active capture')
 # Reusing an A entry starts a fresh lifecycle, even after an unfinished touched capture.
 rt.dt(1/240);f.prepare(f.context,f.world,C.byref(f.index),1,f.A,f.B);c.project()
 assert rt.lib.freeflight_test_active()==1
 reused=run(c)
 rt.lib.freeflight_test_setup(n.base)
 assert reused==run(c);checks.append('unfinished touched capture reused cleanly')
 # Native aborts may leave old active keys. A reused pointer in a new world replaces them.
 f.prepare(f.context,f.world,C.byref(f.index),1,f.A,f.B)
 other=body();other.flight.A=f.A
 reused=run(other)
 rt.lib.freeflight_test_setup(n.base)
 assert reused==run(other);checks.append('same A reused by different world')
 # A solver-table change after capture must reject correction before native writes.
 def solver_changed(enabled):
  rt.enabled=enabled;rt.dt(1/240);f.reset((0,0,0),(0,-980,0))
  f.params(f.info,.6,1);f.setup(f.info,f.gravity,4,1,.1,F(1/240))
  f.prepare(f.context,f.world,C.byref(f.index),1,f.A,f.B)
  ptr(c.solver_manager,0x578,C.addressof(c.objects[0]))
  for _ in range(3):f.accumulate(None,None,f.A,f.B,1,f.info,f.props)
  f.finish(None,f.A,f.B,1,f.motions,None,f.props,f.task,None,None,None,0,0)
  ptr(c.solver_manager,0x578,0)
  return C.string_at(C.addressof(f.motions)+0x80,0x80)
 assert solver_changed(True)==solver_changed(False);checks.append('solver table changes after capture fallback')
 def microsteps(enabled,microsteps,change=False):
  rt.enabled=enabled;rt.dt(1/240);f.reset((0,0,0),(0,-980,0))
  f.params(f.info,.6,1);f.setup(f.info,f.gravity,4,1 if change else microsteps,.1,F(1/240))
  f.prepare(f.context,f.world,C.byref(f.index),1,f.A,f.B)
  if change:C.c_int.from_address(f.info+0xbc).value=microsteps
  for _ in range(3):f.accumulate(None,None,f.A,f.B,1,f.info,f.props)
  f.finish(None,f.A,f.B,1,f.motions,None,f.props,f.task,None,None,None,0,0)
  return C.string_at(C.addressof(f.motions)+0x80,0x80)
 for m in [2,4,8]:
  for change in [False,True]:
   assert microsteps(True,m,change)==microsteps(False,m,change)
   checks.append('microsteps %d change %s native fallback'%(m,change))
 # Sixteen occupied colliding buckets cannot overwrite a live capture.
 rt.lib.freeflight_test_setup(n.base);regs=buffer(648)
 ptr(regs,REG['rsi'],C.addressof(f.world));ptr(regs,REG['r15'],C.addressof(f.motions));ptr(regs,REG['rdi'],0x80)
 for i in range(20):
  ptr(regs,REG['r8'],0x20000000+i*2048*32);rt.helpers[0](regs)
 assert rt.lib.freeflight_test_active()==16
 ptr(regs,REG['r8'],0x20000000);ptr(regs,REG['rdi'],0);rt.helpers[0](regs)
 assert rt.lib.freeflight_test_active()==15
 checks.append('bounded hash collision fallback preserves other active entries')
 rt.lib.freeflight_test_setup(n.base)
 # Unsupported native header metadata disables the candidate, retaining original motion.
 row=buffer(0x150);regs=buffer(648)
 C.c_ubyte.from_address(C.addressof(row)).value=255;ptr(regs,REG['r13'],C.addressof(row));rt.helpers[2](regs)
 assert run(c)==run(c,enabled=False);checks.append('unknown scalar schema fallback')
 rt.lib.freeflight_test_setup(n.base)
 C.c_ubyte.from_address(C.addressof(row)+0x1b).value=2;ptr(regs,REG['r14'],C.addressof(row));rt.helpers[3](regs)
 assert run(c)==run(c,enabled=False);checks.append('unknown optimized cell metadata fallback')
 rt.lib.freeflight_test_setup(n.base)
 C.c_ubyte.from_address(C.addressof(row)+0xe).value=0;C.c_ubyte.from_address(C.addressof(row)+0xf).value=1;rt.helpers[4](regs)
 assert run(c)==run(c,enabled=False);checks.append('unknown generic header length fallback')
 rt.lib.freeflight_test_setup(n.base)
 # Detect gravity changed between actual native substeps.
 def changing(enabled):
  rt.enabled=enabled;rt.dt(1/240);f=c.flight;f.reset((0,0,0),(0,-980,0))
  f.params(f.info,.6,1);f.setup(f.info,f.gravity,4,1,.1,F(1/240));f.prepare(f.context,f.world,C.byref(f.index),1,f.A,f.B)
  C.c_float.from_address(f.info+0xd4).value=-.5
  for _ in range(3):f.accumulate(None,None,f.A,f.B,1,f.info,f.props)
  f.finish(None,f.A,f.B,1,f.motions,None,f.props,f.task,None,None,None,0,0)
  return C.string_at(C.addressof(f.motions)+0x80,0x80)
 assert changing(True)==changing(False);checks.append('changing substep gravity fallback')
 # A missing native intermediate call rejects correction even when every value is finite.
 def incomplete(enabled):
  rt.enabled=enabled;rt.dt(1/240);f=c.flight;f.reset((0,0,0),(0,-980,0))
  f.params(f.info,.6,1);f.setup(f.info,f.gravity,4,1,.1,F(1/240));f.prepare(f.context,f.world,C.byref(f.index),1,f.A,f.B)
  for _ in range(2):f.accumulate(None,None,f.A,f.B,1,f.info,f.props)
  f.finish(None,f.A,f.B,1,f.motions,None,f.props,f.task,None,None,None,0,0)
  return C.string_at(C.addressof(f.motions)+0x80,0x80)
 assert incomplete(True)==incomplete(False);checks.append('incomplete worker sequence fallback')
 # Actual solver/integrator calls from two concurrent CPU threads must match serial execution.
 bodies=[body('scalar'),body('generic')];barrier=None;errors=[]
 def worker(i,parallel=False):
  try:
   c=bodies[i];f=c.flight;f.reset((0,0,0),(0,-980,0));raw=F(1/240)
   rt.dt(raw)
   if parallel:barrier.wait()
   for _ in range(600):
    result=frame(c,raw,contact=bool(i))
   return result
  except BaseException as e:errors.append(repr(e));raise
 rt.enabled=True;rt.dt(1/240);expected=[worker(i) for i in range(2)];actual=[None,None];barrier=threading.Barrier(2)
 threads=[threading.Thread(target=lambda i=i:actual.__setitem__(i,worker(i,True))) for i in range(2)]
 for t in threads:t.start()
 for t in threads:t.join()
 assert not errors and expected==actual,(errors,expected,actual)
 assert rt.lib.freeflight_test_active()==0
 checks.append('two native workers, 2400 serial/concurrent frames, exact equality')
 return len(checks)

def scalar_dispatch(n,rt):
 c=Contact(n);configure(c,'scalar');rt.support(c,'scalar');normal(c,(0,1,0))
 for kind in [0,1,8,9]:
  for side in [0,1]:
   C.c_ubyte.from_address(c.jac).value=kind;C.c_ubyte.from_address(c.jac+1).value=side
   results=[]
   for enabled in [False,True]:
    rt.enabled=enabled;c.flight.reset((30,-10,40),(0,-980,0));rt.dt(1/240)
    for _ in range(15):result=frame(c,F(1/240),contact=True)
    results.append(result)
   assert results[0]==results[1],(kind,side,results)
 return 8

def quantized_clock(n,rt):
 c=Contact(n);configure(c,'scalar');rt.support(c,'scalar');worst=0;rows=[]
 for seconds in [1,10]:
  for scale in [.25,1,2]:
   results={}
   for rate in [30,60,120,240,0]:
    ticks=[i*8192//rate for i in range(rate*seconds+1)] if rate else [0]+[(i*8+j)*8192//240 for i in range(30*seconds) for j in [1,4,8]]
    for enabled in [False,True]:
     rt.enabled=enabled;c.flight.reset((0,0,0),(0,-980,0))
     for a,b in zip(ticks,ticks[1:]):
      raw=F((b-a)/8192);dt=F(raw*scale)
      old_velocity=floats(c.flight.motions,0xc0)[1]
      rt.dt(raw);result=frame(c,dt)
      if enabled:
       # A double oracle using each native starting velocity isolates the
       # corrected mean from native velocity/position accumulation roundoff.
       target=old_velocity-980*dt/2-980*(dt if rate==30 else scale/30)/8
       mean=floats(c.flight.motions,0xe0)[1]
       ulp=2**(math.frexp(max(1,abs(old_velocity),abs(mean)))[1]-24)
       assert abs(mean-target)<8*ulp,(seconds,scale,rate,mean,target)
     results[rate,enabled]=result
    if rate==30:assert results[rate,False]==results[rate,True]
   reference=results[30,False]['position'][1]
   for rate in [60,120,240,0]:
    gap=abs(results[rate,True]['position'][1]-reference)
    native_gap=abs(results[rate,False]['position'][1]-reference)
    worst=max(worst,gap)
    # Long runs retain native float accumulation, checked separately above.
    assert gap<(.005 if seconds==1 else native_gap*.1),(seconds,scale,rate,gap,native_gap)
    rows.append((seconds,scale,rate,gap,native_gap))
 print('quantized-clock',len(rows),'cases worst',worst,flush=True)
 return rows

@unittest.skipUnless(os.environ.get("BB_TEST_GAME"),"requires native USA/EU dump")
class NativeFreeflightTimestep(unittest.TestCase):
 def test_native_freeflight_and_contacts(self):
  from archive_reader import Archive
  from igz_file import Igz
  root=Path(__file__).resolve().parents[1]
  game=Path(os.environ["BB_TEST_GAME"])
  elf,*_=parse_self((game/"eboot.bin").read_bytes())
  archive=Archive(game/'archives/permanent.pak')
  try:
   asset=Igz(archive.extract(next(f for f in archive.files if f['path'].lower()=='loosedata/presets/physicsmotionpropertiespresets.igz')))
  finally:archive.data.close();archive._file.close()
  names=next(o for o,t in asset.objects() if t=='igNameList')
  objects=asset.u64(0x20);strings=asset.u64(names+0x20)
  preset=next(asset.u64(objects+8*i) for i in range(asset.u32(0xc)) if asset.string_at(strings+16*i)=='BaseKart')
  mode=asset.u32(preset+0x30)
  self.assertEqual(mode,0)
  self.assertEqual(struct.unpack_from('<ff',asset.data,preset+0x28),(0,0))
  with tempfile.TemporaryDirectory(prefix="octr-native-freeflight-") as tmp:
   library=Path(tmp)/"freeflight.so"
   subprocess.run(["cc","-std=c11","-O2","-Wall","-Wextra","-Werror","-ffp-contract=off","-ffunction-sections","-fdata-sections","-fvisibility=hidden","-shared","-fPIC","-I"+str(root/"src"),str(root/"tests/test_freeflight_timestep_native.c"),"-Wl,--gc-sections","-Wl,--no-undefined","-lm","-o",str(library)],check=True)
   native=Native(elf)
   try:
    properties=buffer(0x40)
    C.CFUNCTYPE(None,P,C.c_int,C.c_float)(native.base+0x6a7d20)(properties,mode,980)
    self.assertEqual(floats(properties,0x20)[:2],[0,0])
    runtime=Runtime(native,library)
    count,worst=matrix(native,runtime)
    self.assertEqual(count,3360)
    self.assertLess(worst,.005)
    self.assertEqual(controls(native,runtime),31)
    runtime.lib.freeflight_test_setup(native.base)
    self.assertEqual(scalar_dispatch(native,runtime),8)
    self.assertEqual(len(quantized_clock(native,runtime)),24)
   finally:native.close()

if __name__=="__main__":unittest.main()
