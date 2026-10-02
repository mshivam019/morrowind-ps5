#!/usr/bin/env python3
"""Exercise actual OSG allocation guard with fresh and pooled immutable names."""
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[2]
s=(root/'deps/openscenegraph/src/osg/Texture2D.cpp').read_text()
start=s.index('             if (!textureObject->isAllocated())',s.index('// A matching pooled texture'))
end=s.index('\n         }\n         else',start)
code=s[start:end]
h='''#include <cassert>
namespace osg { template<class T> T maximum(T a,T b){return a>b?a:b;} }
const int GL_TEXTURE_2D=3553;
struct TextureObject { bool allocated=false; bool isAllocated()const{return allocated;} void setAllocated(bool value){allocated=value;} };
struct Extensions { unsigned calls=0; void glTexStorage2D(int target,int levels,int format,int width,int height){assert(target==GL_TEXTURE_2D&&levels==1&&format==32856&&width==1920&&height==1080);++calls;} };
void allocate(TextureObject* textureObject,Extensions* extensions){
 int _numMipmapLevels=0,texStorageSizedInternalFormat=32856,_textureWidth=1920,_textureHeight=1080;
'''+code+'''
}
int main(){
 Extensions gl;TextureObject fresh;
 allocate(&fresh,&gl);assert(gl.calls==1&&fresh.isAllocated());
 // Same-profile pooled name retains its immutable allocation across owners.
 for(unsigned owner=0;owner<20;++owner){allocate(&fresh,&gl);}assert(gl.calls==1);
 // Already allocated orphan from another texture bypasses storage definition.
 TextureObject pooled;pooled.allocated=true;allocate(&pooled,&gl);assert(gl.calls==1);
 // Different profile requires another fresh object, which allocates once.
 TextureObject other;allocate(&other,&gl);assert(gl.calls==2&&other.isAllocated());
}
'''
with tempfile.TemporaryDirectory(prefix='osg-immutable-reuse-') as directory:
 p=Path(directory);(p/'test.cpp').write_text(h)
 subprocess.run(['c++','-std=c++17','-Wall','-Wextra','-Werror',str(p/'test.cpp'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
print('PASS fresh allocation,20 pooled reuses,allocated orphan,new profile')
