#!/usr/bin/env python3
"""Verify actual PS5 preprocess/object output with detailed OSG profiling off/on."""
from pathlib import Path
import os,shlex,subprocess,tempfile
root=Path(__file__).resolve().parents[2]
build=root/'build/openscenegraph'
objects={
 'RenderLeaf':'src/osgUtil/CMakeFiles/osgUtil.dir/RenderLeaf.cpp.o',
 'Renderer':'src/osgViewer/CMakeFiles/osgViewer.dir/Renderer.cpp.o',
 'ViewerBase':'src/osgViewer/CMakeFiles/osgViewer.dir/ViewerBase.cpp.o',
}
def clean_command(command):
 result=[];i=0
 while i<len(command):
  arg=command[i]
  if arg in ('-o','-MF','-MT','-MQ'):i+=2;continue
  if arg in ('-c','-MD','-MMD'):i+=1;continue
  if arg.startswith('-DPS5_OSG_DETAILED_PROFILE'):i+=1;continue
  result.append(arg);i+=1
 return result
with tempfile.TemporaryDirectory(prefix='osg-profile-switch-') as directory:
 tmp=Path(directory)
 for name,obj in objects.items():
  commands=subprocess.check_output(['ninja','-C',str(build),'-t','commands',obj],text=True).splitlines()
  command=next(shlex.split(line) for line in commands if ' -c ' in line and line.endswith('/'+name+'.cpp'))
  base=clean_command(command)
  for enabled in (0,1):
   preprocessed=tmp/f'{name}-{enabled}.ii'
   subprocess.run([*base,f'-DPS5_OSG_DETAILED_PROFILE={enabled}','-E','-o',str(preprocessed)],cwd=build,check=True)
   text=preprocessed.read_text()
   marker={'RenderLeaf':'[ps5-osg-leaf]','Renderer':'[ps5-osg-renderer]','ViewerBase':'[ps5-osg-phase]'}[name]
   assert (marker in text)==bool(enabled),(name,enabled)
   if name=='RenderLeaf':
    binary=tmp/f'{name}-{enabled}.o'
    subprocess.run([*base,f'-DPS5_OSG_DETAILED_PROFILE={enabled}','-c','-o',str(binary)],cwd=build,check=True)
    symbols=subprocess.check_output(['llvm-nm','-C',str(binary)],text=True)
    assert ('osg::Timer::tick()' in symbols)==bool(enabled),symbols
    assert ('ps5_osg_leaf_report' in symbols)==bool(enabled),symbols
    print(f'RenderLeaf profile={enabled}: Timer::tick reference={bool(enabled)}, diagnostic helper={bool(enabled)}')
  print(f'{name}: preprocessor excludes production diagnostics and preserves opt-in diagnostics')
print('PASS OSG profiling switch; no production RenderLeaf timer references')
