#!/usr/bin/env python3
"""Exercise the production framebuffer callback against a counted capture target."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'openmw/apps/openmw/mwgui/loadingscreen.cpp').read_text()
start = source.index('    class CopyFramebufferToTextureCallback')
end = source.index('    class DontComputeBoundCallback', start)
callback = source[start:end]
harness = r'''
#include <cassert>
namespace osg {
struct State {};
struct Viewport { double width() const { return 1920; } double height() const { return 1080; } };
struct RenderInfo;
struct Camera {
    struct DrawCallback { virtual ~DrawCallback() {} virtual void operator()(RenderInfo&) const = 0; };
    Viewport viewport;
    const Viewport* getViewport() const { return &viewport; }
};
struct RenderInfo {
    State state; Camera camera;
    Camera* getCurrentCamera() { return &camera; }
    State* getState() { return &state; }
};
struct Texture2D {
    unsigned copies = 0;
    void copyTexImage2D(State&, int x, int y, int w, int h) {
        assert(x == 0 && y == 0 && w == 1920 && h == 1080); ++copies;
    }
};
template<class T> struct ref_ptr {
    T* ptr;
    ref_ptr(T* p) : ptr(p) {}
    T* operator->() const { return ptr; }
};
}
'''
harness += callback
harness += r'''
int main() {
    osg::Texture2D target;
    osg::RenderInfo frame;
    CopyFramebufferToTextureCallback capture(&target);
    capture(frame);
    assert(target.copies == 1);
    for (unsigned i = 0; i < 600; ++i) capture(frame);
    assert(target.copies == 1);
    capture.reset();
    capture(frame);
    assert(target.copies == 2);
    capture(frame);
    assert(target.copies == 2);
}
'''
with tempfile.TemporaryDirectory(prefix='openmw-loading-snapshot-') as directory:
    test = Path(directory) / 'test.cpp'
    binary = Path(directory) / 'test'
    test.write_text(harness)
    subprocess.run(['c++', '-std=c++17', '-Wall', '-Wextra', '-Werror', str(test), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
print('PASS: first capture, 600 unchanged frames, reset, second capture')
