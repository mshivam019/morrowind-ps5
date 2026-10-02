"""Focused DDS cropping, path safety, manifest and inactive-title checks."""
import importlib.util
from pathlib import Path
import struct
import sys
import unittest
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'tools'))
from texture_pack import cap_dds,inspect_dds,safe_relative,config_data_line,memory_estimate,add_memory_accounting
spec=importlib.util.spec_from_file_location('transfer',Path(__file__).resolve().parents[1]/'tools/transfer-hd-textures.py')
transfer=importlib.util.module_from_spec(spec);spec.loader.exec_module(transfer)


def make_dds(width,height,mips,fmt='DXT1',padding=False):
 f=[0]*31;f[0]=124;f[1]=0x21007;f[2]=height;f[3]=width;f[6]=mips
 f[18]=32;f[26]=0x401008
 chunks=[]
 if fmt.startswith('DXT'):
  f[19]=4;f[20]=int.from_bytes(fmt.encode(),'little');f[1]|=0x80000
  sizes=[max(1,((width>>i)+3)//4)*max(1,((height>>i)+3)//4)*(8 if fmt=='DXT1' else 16) for i in range(mips)]
  f[4]=sizes[0];chunks=[bytes([i+1])*n for i,n in enumerate(sizes)]
 else:
  bits=int(fmt[3:]);b=bits//8;f[19]=0x40;f[21]=bits;f[1]|=8
  f[22:26]=[0xff0000,0xff00,0xff,0xff000000 if bits==32 else 0]
  f[4]=((width*b+3)//4)*4 if padding else width*b
  for i in range(mips):
   w,h=max(1,width>>i),max(1,height>>i);row=bytes([i+1])*(w*b)
   if padding:row+=bytes([255])*((4-len(row)%4)%4)
   chunks.append(row*h)
 return b'DDS '+struct.pack('<31I',*f)+b''.join(chunks),chunks


class TextureTests(unittest.TestCase):
 def test_compressed_non_square_partial_chain(self):
  for fmt in ('DXT1','DXT3','DXT5'):
   blob,chunks=make_dds(8192,4096,6,fmt);result,skip=cap_dds(blob)
   self.assertEqual(skip,2);self.assertEqual(result[128:],b''.join(chunks[2:]))
   d=inspect_dds(result[:128],len(result));self.assertEqual((d['width'],d['height'],d['mip_count']),(2048,1024,4))
   f=struct.unpack_from('<31I',result,4);self.assertEqual(f[4],len(chunks[2]));self.assertTrue(f[1]&0x80000)
 def test_rgb_stride_and_single_mip_flags(self):
  for padding in (False,True):
   blob,_=make_dds(7,3,3,'RGB24',padding);result,skip=cap_dds(blob,maximum=2)
   self.assertEqual(skip,2);self.assertEqual(result[128:],b'\x03'*3)
   f=struct.unpack_from('<31I',result,4)
   self.assertEqual((f[2],f[3],f[4],f[6]),(1,1,3,1))
   self.assertFalse(f[1]&0x20000);self.assertFalse(f[26]&(8|0x400000));self.assertTrue(f[1]&8)
 def test_rgb32_preserved(self):
  blob,chunks=make_dds(4096,2048,4,'RGB32');result,skip=cap_dds(blob)
  self.assertEqual(skip,1);self.assertEqual(result[128:],b''.join(chunks[1:]))
 def test_invalid_payload_or_missing_level(self):
  blob,_=make_dds(4096,4096,1)
  with self.assertRaises(ValueError):cap_dds(blob)
  with self.assertRaises(ValueError):inspect_dds(blob[:128],len(blob)-1)
 def test_rgb_masks(self):
  for fmt in ('RGB24','RGB32'):
   blob,_=make_dds(4,4,3,fmt)
   inspect_dds(blob[:128],len(blob))
   invalid=bytearray(blob);struct.pack_into('<I',invalid,92,0x123456)
   with self.assertRaisesRegex(ValueError,'channel masks'):
    inspect_dds(invalid[:128],len(invalid))
  # OSG also accepts swapped byte order and packed 10-bit RGB32.
  blob,_=make_dds(4,4,3,'RGB32')
  for masks in ((0xff,0xff00,0xff0000,0xff000000),
                (0x3ff,0xffc00,0x3ff00000,0xc0000000)):
   header=bytearray(blob[:128]);struct.pack_into('<4I',header,92,*masks)
   inspect_dds(header,len(blob))
 def test_path_escape(self):
  for name in ('../textures/a.dds','textures/../../a.dds','/textures/a.dds','textures\\a.dds','textures//a.dds','textures/a.txt','textures/a\r\n.dds'):
   with self.assertRaises(ValueError):safe_relative(name)
  self.assertEqual(safe_relative('textures/sub/a.dds'),'textures/sub/a.dds')
 def test_remote_state_fail_closed(self):
  for state in ({},{'procs':None},{'procs':[None]},{'procs':[{'title_id':'PPSA99630'}]},{'procs':[{'title_id':'CUSA00001'}]}):
   with self.assertRaises(RuntimeError):transfer.require_inactive(state)
  transfer.require_inactive({'procs':[{'title_id':''}]})
 def test_residency_accounts_cpu_copies_and_gpu_padding(self):
  blob,_=make_dds(4,4,3,'DXT1');d=inspect_dds(blob[:128],len(blob));m=memory_estimate(d,len(blob))
  self.assertEqual(m['osg_image_payload_bytes'],24)
  self.assertEqual(m['mesa_compressed_cpu_bytes'],24)
  self.assertEqual(m['rgba_texel_bytes'],84)
  self.assertEqual(m['rgba_gpu_allocation_bytes'],16384)
  self.assertEqual(m['modeled_resident_bytes'],16432)
  blob,_=make_dds(4,4,3,'RGB32');d=inspect_dds(blob[:128],len(blob));m=memory_estimate(d,len(blob))
  self.assertEqual(m['mesa_compressed_cpu_bytes'],0)
  self.assertEqual(m['modeled_resident_bytes'],84+16384)
 def test_memory_manifest_totals(self):
  blob,_=make_dds(4,4,3);d=inspect_dds(blob[:128],len(blob))
  m=add_memory_accounting({'files':[{'path':'textures/a.dds','bytes':len(blob),'dds':d},
                                  {'path':'textures/b.dds','bytes':len(blob),'dds':d}]})
  self.assertEqual(m['memory_model']['totals']['modeled_resident_bytes'],32864)
 def test_profile_path(self):
  self.assertEqual(config_data_line(),'data="/app0/assets/Mods/MET6.1-2K"\n')
  with self.assertRaises(ValueError):config_data_line('../Data Files')

if __name__=='__main__':unittest.main()
