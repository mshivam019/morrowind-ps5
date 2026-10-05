#!/usr/bin/env python3
"""Package the compiled OpenMW PS5 objects using the proven native-title pipeline."""
import argparse
import hashlib
import json
import os
import re
import shlex
import shutil
import subprocess
from pathlib import Path
from texture_pack import config_data_line, load_manifest

REPO = Path(__file__).resolve().parent.parent
ROOT = Path(os.environ.get("PS5SDK_ROOT", str(Path.home() / "ps5sdk")))
TEMPLATE = Path(os.environ.get("PS5_NATIVE_APP_TEMPLATE", ROOT / "native-app-boilerplate"))
GL_ROOT = Path(os.environ.get("PS5_OPENGL_ROOT", ROOT / "ps5-opengl-030/ps5-opengl"))
GL_PREFIX = Path(os.environ.get("PS5_OPENGL_SDK", ROOT / "extracted/ps5-opengl-sdk-0.3.0/sdk"))
SDL_PREFIX = Path(os.environ.get("PS5_SDL2_PREFIX", REPO / "prefix"))
BUILD = REPO / "build/openmw"
SOURCE = REPO / "openmw"
ART = TEMPLATE / "sce_sys"

TITLE_ID = "PPSA99630"
TITLE_NAME = "Morrowind - OpenMW"
CONTENT_ID = f"UP9000-{TITLE_ID}_00-OPENMWMORROWIND0"
SYSTEM_IMPORTS = ("libScePad.so", "libSceUserService.so", "libSceSystemService.so",
                  "libSceAudioOut.so", "libSceVideoOut.so")


def run(*args, **kwargs):
    return subprocess.run([str(a) for a in args], check=True, **kwargs)


def replace_once(path, old, new):
    text = path.read_text()
    if text.count(old) != 1:
        raise SystemExit(f"template contract changed: {path}: {old!r}")
    path.write_text(text.replace(old, new))


def openmw_link_inputs():
    """Objects and libraries from the CMake link statement for the OpenMW executable."""
    ninja = (BUILD / "build.ninja").read_text()
    match = re.search(r"^build (openmw): CXX_EXECUTABLE_LINKER__openmw_\w+ (.*?)(?: \|\| .*)?$"
                      r"((?:\n  .*)*)", ninja, re.M)
    if not match:
        raise SystemExit("openmw link statement not found in build.ninja")
    explicit = match.group(2).split(" | ")[0]
    objects = [BUILD / p.replace("$ ", " ") for p in explicit.split()]
    libs_line = re.search(r"^  LINK_LIBRARIES = (.*)$", match.group(3), re.M).group(1)
    # CMake emits static OSG plugins in LINK_FLAGS to preserve their registrars.
    flags = re.search(r"^  LINK_FLAGS = (.*)$", match.group(3), re.M).group(1)
    whole = re.search(r"-Wl,--whole-archive (.*?) -Wl,--no-whole-archive", flags)
    if whole:
        plugins = shlex.split(whole.group(1))
        plugin_object = BUILD / "ps5-osg-plugins.o"
        run("ld.lld", "-r", "--whole-archive", *plugins, "--no-whole-archive", "-o", plugin_object)
        objects.append(plugin_object)
    libraries = []
    for token in shlex.split(libs_line):
        path = Path(token) if token.startswith("/") else BUILD / token
        # The GL runtime is linked explicitly from GL_PREFIX below.
        if path.name.startswith("libPS5OpenGL") or path.name == "libSDL2.a":
            continue
        if token.endswith((".a", ".o")):
            libraries.append(path)
    return objects, libraries


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--demo", action="store_true", help="start directly in Seyda Neen")
    parser.add_argument("--probe", action="store_true", help="build only the compatibility graphics probe")
    parser.add_argument("--hold-on-assert", action="store_true",
                        help="keep a failed diagnostic launch alive so its logs remain readable")
    parser.add_argument("--hd-textures", action="store_true",
                        help="enable separately transferred MET 6.1 textures (no texture assets bundled)")
    parser.add_argument("--hd-texture-profile", choices=("MET6.1-2K", "MET6.1"),
                        default="MET6.1-2K", help="HD profile; 2K is the default to limit memory use")
    parser.add_argument("--out", type=Path, default=REPO / "build/package")
    parser.add_argument("--heap-mib", type=int, default=2048,
                        help="app heap reserved from direct memory (default 2048)")
    args = parser.parse_args()
    texture_manifest = None
    if args.hd_textures:
        if args.probe:
            parser.error("--hd-textures is not applicable to the graphics probe")
        texture_manifest = load_manifest(REPO / "game-mods" / args.hd_texture_profile)
    driver_receipt = None
    driver_archive = REPO / "driver/lib/libps5_opengl_core33.a"
    receipt_path = driver_archive.with_suffix(".build.json")
    if driver_archive.is_file() and receipt_path.is_file():
        driver_receipt = json.loads(receipt_path.read_text())
        if driver_receipt.get("archive_sha256") != hashlib.sha256(driver_archive.read_bytes()).hexdigest():
            raise SystemExit("driver archive changed since build receipt; rebuild the driver")
    out = args.out.resolve()
    if args.probe and out == REPO / "build/package":
        out = REPO / "build/graphics-probe-package"
    if out.exists():
        shutil.rmtree(out)
    out.mkdir(parents=True)

    for directory in ("runtime", "sce_sys", "tooling", "tools", ".deps/native"):
        shutil.copytree(TEMPLATE / directory, out / directory)
    sdk = out / ".deps/native/ps5-payload-sdk"
    for directory in ("src", "vendor", "assets"):
        (out / directory).mkdir(parents=True, exist_ok=True)

    # Runtime shims and a 2 GiB direct-memory heap for the engine.
    shutil.copy2(GL_ROOT / "native-app/runtime_shims.c", out / "src/runtime_shims.c")
    if args.probe or args.hold_on_assert:
        runtime = out / "src/runtime_shims.c"
        runtime.write_text(runtime.read_text().replace("  abort();", "  fflush(NULL); for (;;) sceKernelUsleep(100000);"))
    shutil.copy2(REPO / "platform/ps5_libc_shims.c", out / "src/ps5_libc_shims.c")
    for name in ("ps5_tls_dtors.c", "ps5_emutls.c"):
        shutil.copy2(REPO / "platform" / name, out / "src" / name)
    # The graphics probe holds after main for inspection; the game should exit.
    if not args.probe:
        replace_once(out / "src/runtime_shims.c",
                     "  for (;;)\n    sceKernelUsleep(100000);",
                     '  extern int sceSystemServiceLoadExec(const char *, const char **);\n'
                     '  int result = sceSystemServiceLoadExec("exit", NULL);\n'
                     '  fprintf(stderr, "PS5 exit request returned: 0x%x\\n", result);\n'
                     '  for (;;)\n    sceKernelUsleep(100000);')
    shutil.copy2(REPO / "platform/ps5_extra_shims.c", out / "src/ps5_extra_shims.c")
    heap = (GL_ROOT / "native-app/app_heap.c").read_text()
    old = "#define PS5_OPENGL_HEAP_SIZE (128u * 1024u * 1024u)"
    if heap.count(old) != 1:
        raise SystemExit("app_heap.c contract changed")
    heap = heap.replace(old, f"#define PS5_OPENGL_HEAP_SIZE ((size_t){args.heap_mib}u * 1024u * 1024u)")
    # Game titles get little flexible (mmap) memory; back the heap with direct memory,
    # the same way the GL runtime maps its own buffers (type 12, CPU+GPU RW).
    old_map = ("  void *base = mmap(NULL, PS5_OPENGL_HEAP_SIZE, PROT_READ | PROT_WRITE,\n"
               "                    MAP_PRIVATE | MAP_ANON, -1, 0);\n"
               "  if (base == MAP_FAILED) {\n")
    new_map = ("  void *base = ps5_heap_map_direct(PS5_OPENGL_HEAP_SIZE);\n"
               "  if (base == NULL) {\n")
    helper = ("int64_t sceKernelGetDirectMemorySize(void);\n"
              "int32_t sceKernelAllocateDirectMemory(int64_t, int64_t, size_t, size_t, int, int64_t *);\n"
              "int32_t sceKernelMapDirectMemory(void **, size_t, int, int, int64_t, size_t);\n\n"
              "static void *ps5_heap_map_direct(size_t size) {\n"
              "  int64_t start = 0;\n"
              "  void *base = NULL;\n"
              "  if (sceKernelAllocateDirectMemory(0, sceKernelGetDirectMemorySize(), size, 0x200000,\n"
              "                                    12, &start) != 0)\n"
              "    return NULL;\n"
              "  if (sceKernelMapDirectMemory(&base, size, 0x33, 0, start, 0x200000) != 0)\n"
              "    return NULL;\n"
              "  return base;\n"
              "}\n\n"
              "static int ps5_heap_ready(void) {\n")
    if heap.count(old_map) != 1 or heap.count("static int ps5_heap_ready(void) {\n") != 1:
        raise SystemExit("app_heap.c mapping contract changed")
    heap = heap.replace(old_map, new_map).replace("static int ps5_heap_ready(void) {\n", helper)
    (out / "src/app_heap.c").write_text(heap)
    shutil.copy2(out / "tooling/native/ps5-pie.ld", out / "tooling/native/ps5-pie-base.ld")
    for name in ("ps5-pie.ld", "app-symbols.map"):
        shutil.copy2(GL_ROOT / "native-app" / name, out / "tooling/native" / name)

    # Title metadata and launcher art.
    param = json.loads((GL_ROOT / "native-app/param.json").read_text())
    param.update(titleId=TITLE_ID, conceptId=TITLE_ID[4:], contentId=CONTENT_ID,
                 downloadDataSize=1024, contentVersion="01.000.001")
    if args.probe:
        param.update(titleId="PPSA99631", conceptId="99631", contentId="UP9000-PPSA99631_00-OPENMWGFXTEST000")
    language = param["localizedParameters"]["defaultLanguage"]
    param["localizedParameters"][language]["titleName"] = "Morrowind Graphics Test" if args.probe else TITLE_NAME
    (out / "sce_sys/param.json").write_text(json.dumps(param, indent=2) + "\n")
    for name in ("icon0.png", "pic0.dds", "pic1.dds"):
        art = REPO / "art" if not args.probe else ART
        shutil.copy2(art / name, out / "sce_sys" / name)
    # The template's home-screen preview sound is a test tone, not game audio.
    (out / "sce_sys/snd0.at9").unlink(missing_ok=True)

    # Game content is transferred separately to M.2. Package engine resources only.
    resources = BUILD / "resources"
    if not args.probe and not resources.is_dir():
        raise SystemExit("generated OpenMW resources are missing")
    if not args.probe:
        shutil.copytree(resources, out / "assets/resources")
    config_text = (SOURCE / "files/openmw.cfg").read_text().replace(
        "${OPENMW_RESOURCE_FILES}", "/app0/assets/resources")
    # Use the original game movies rather than OpenMW's example-suite defaults.
    config_text = "\n".join(line for line in config_text.splitlines()
                            if not line.startswith("fallback=Movies_")) + "\n"
    for key, value in {
        "Movies_Company_Logo": "bethesda logo.bik",
        "Movies_Morrowind_Logo": "mw_logo.bik",
        "Movies_New_Game": "mw_intro.bik",
        "Movies_Loading": "mw_load.bik",
        "Movies_Options_Menu": "mw_menu.bik",
    }.items():
        config_text += f"fallback={key},{value}\n"
    config_text += ('\ndata="/app0/assets/Data Files"\n'
                    'content=Morrowind.esm\ncontent=Tribunal.esm\ncontent=Bloodmoon.esm\n'
                    'fallback-archive=Morrowind.bsa\nfallback-archive=Tribunal.bsa\n'
                    'fallback-archive=Bloodmoon.bsa\n')
    if texture_manifest is not None:
        config_text += config_data_line(texture_manifest["pack"])
        # Content remains separate: this receipt lets an offline package be audited.
        (out / "assets/hd-textures.json").write_text(json.dumps(dict(
            pack=texture_manifest["pack"], archive_sha256=texture_manifest["archive_sha256"],
            files=len(texture_manifest["files"]), bytes=texture_manifest["total_bytes"]), indent=2) + "\n")
    if args.demo:
        config_text += "skip-menu=1\nnew-game=0\nstart=Seyda Neen\n"
    else:
        config_text += "skip-menu=0\nnew-game=0\n"
    (out / "assets/openmw.cfg").write_text(config_text)
    if driver_receipt is not None:
        (out / "assets/driver-build.json").write_text(json.dumps(driver_receipt, indent=2) + "\n")
    if not args.probe:
        shutil.copy2(BUILD / "defaults.bin", out / "assets/defaults.bin")
    shutil.copy2(REPO / "config/alsoft.conf", out / "assets/alsoft.conf")
    shutil.copy2(SOURCE / "files/gamecontrollerdb.txt", out / "assets/gamecontrollerdb.txt")
    shutil.copy2(REPO / "config/settings.cfg", out / "assets/settings.cfg")

    replace_once(out / "tooling/native/sce_module_writer.cpp",
                 "write_u64(result.data, result.heap_size, std::numeric_limits<std::uint64_t>::max());",
                 "write_u64(result.data, result.heap_size, 0x10000000ULL);")
    script = out / "tools/build.sh"
    replace_once(script, 'bash "$root/tools/setup-native-dependencies.sh" >/dev/null',
                 'test -x "$root/.deps/native/ps5-payload-sdk/bin/prospero-lld"')
    replace_once(script, '[[ -f $root/runtime/libc.prx ]] || bash "$root/tools/rebuild-libc.sh"',
                 'test -f "$root/runtime/libc.prx"')
    replace_once(script, "--eh-frame-hdr \\",
                 "--eh-frame-hdr --error-limit=0 --wrap=malloc --wrap=calloc --wrap=realloc --wrap=free "
                 "--wrap=posix_memalign --wrap=malloc_usable_size "
                 # OpenAL's constant-initialized TLS has no dynamic initializer.
                 # Resolve the optional weak hook to zero before SELF conversion.
                 "--defsym=_ZTHN10ALCcontext13sLocalContextE=0 "
                 "\\")

    for name in ("libSceAgc.so", "libSceAgcDriver.so"):
        shutil.copy2(GL_PREFIX / "lib" / name, sdk / "target/lib" / name)
    # build.sh links every stub with --as-needed; the WebKit POSIX module is not loaded
    # for game titles, so anything bound to it would be null at runtime.
    (sdk / "target/lib/libScePosixForWebKit.so").unlink()
    compiler_rt_override = os.environ.get("PS5_COMPILER_RT")
    compiler_rt = (Path(compiler_rt_override) if compiler_rt_override else
                   Path(run("clang", "--print-resource-dir", capture_output=True,
                            text=True).stdout.strip()) / "lib/linux/libclang_rt.builtins-x86_64.a")

    if args.probe:
        probe = out / "graphics_probe.o"
        run(TEMPLATE / "tooling/prospero-clang18", "-O2", "-ffunction-sections", "-fdata-sections",
            "-I" + str(SDL_PREFIX / "include/SDL2"), "-I" + str(GL_PREFIX / "include"),
            "-c", REPO / "platform/graphics_probe.c", "-o", probe)
        objects, libraries = [probe], []
    else:
        objects, libraries = openmw_link_inputs()
    inputs = objects + libraries
    gl_group = out / "vendor/libps5gl.a"
    gl_script = (GL_PREFIX / "lib/libPS5OpenGLCore33.a").read_text()
    for archive in ("libpsbc.ps5.a", "libps5_opengl_core33.a"):
        patched = REPO / "driver/lib" / archive
        if not patched.is_file():
            raise SystemExit(f"missing patched graphics library: {patched}")
        gl_script = gl_script.replace(archive, '"' + str(patched) + '"')
    gl_group.write_text(gl_script)
    inputs += [SDL_PREFIX / "lib/libSDL2.a", gl_group]
    inputs += [sdk / "target/lib" / name for name in SYSTEM_IMPORTS]
    inputs += [sdk / f"target/lib/{name}" for name in ("libunwind.a", "libc++abi.a", "libc++.a")]
    inputs += [compiler_rt]
    missing = [p for p in inputs if not p.is_file()]
    if missing:
        raise SystemExit("missing link inputs:\n" + "\n".join(map(str, missing[:20])))
    print(f"link: {len(objects)} objects, {len(inputs) - len(objects)} libraries")
    group = (f'SEARCH_DIR("{REPO}/driver/lib")\nSEARCH_DIR("{sdk}/target/lib")\nSEARCH_DIR("{GL_PREFIX}/lib")\n'
             'EXTERN(ps5_agc_gate2_run)\nGROUP (\n' +
             "".join(f'  "{p}"\n' for p in inputs) + ')\n')
    (out / "vendor/libopenmw.a").write_text(group)

    env = os.environ.copy()
    env["PS5_PAYLOAD_SDK"] = str(sdk)
    env["APP_STATIC_ARCHIVES"] = "vendor/libopenmw.a"
    for key in ("APP_DEFINITIONS", "APP_INCLUDE_PATHS", "PACBREW_PACKAGES",
                "PACBREW_INCLUDE_PATHS", "PACBREW_STATIC_ARCHIVES", "APP_RUNTIME_MODULES"):
        env[key] = ""
    run("bash", script, "Folder", env=env, cwd=out)
    needed = run(shutil.which("llvm-readelf-18") or "llvm-readelf", "-d", out / "build/llvm-pie.elf",
                 capture_output=True, text=True).stdout
    if "PosixForWebKit" in needed:
        raise SystemExit("eboot still imports libScePosixForWebKit")
    app = out / "dist" / param["titleId"]
    print(f"OpenMW title folder: {app}")


if __name__ == "__main__":
    main()
