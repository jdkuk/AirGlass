"""AirGlass build: shaders (fxc) + C/C++ (llvm-mingw clang), parallel and incremental.

usage: python build.py [--clean] [--debug] [--pro]

--pro builds a personal copy with AirGlass Pro always unlocked (no license key needed).
Releases are built without it.
"""
import concurrent.futures as cf
import glob
import os
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.abspath(__file__))
TC = glob.glob(os.path.join(ROOT, ".toolchain", "llvm-mingw-*", "bin"))[0]
FFMPEG = glob.glob(os.path.join(ROOT, "third_party", "ffmpeg-*"))[0]
FXC = os.path.join(os.environ["ProgramFiles(x86)"], r"Windows Kits\10\bin\10.0.22621.0\x64\fxc.exe")
OUT = os.path.join(ROOT, "build")
DEBUG = "--debug" in sys.argv
PRO = "--pro" in sys.argv
OBJ = os.path.join(OUT, "obj-pro" if PRO else "obj")
SHADERS = os.path.join(OUT, "shaders")

CXXFLAGS = [
    "-std=c++20", "-O0" if DEBUG else "-O2", "-g" if DEBUG else "-g0",
    "-Wall", "-Wextra", "-Wno-unused-parameter", "-Wno-missing-field-initializers",
    "-DUNICODE", "-D_UNICODE", "-DWIN32_LEAN_AND_MEAN", "-DNOMINMAX",
    "-D_WIN32_WINNT=0x0A00", "-DWINVER=0x0A00",
    "-I" + os.path.join(ROOT, "src"),
    "-I" + os.path.join(ROOT, "third_party", "ed25519"),
    "-I" + os.path.join(ROOT, "third_party", "playfair"),
    "-I" + os.path.join(FFMPEG, "include"),
    "-I" + SHADERS,
] + (["-DAIRGLASS_ALWAYS_PRO"] if PRO else [])
CFLAGS = ["-std=gnu11", "-O2", "-w"]

LIBS = [
    os.path.join(FFMPEG, "lib", "libavcodec.dll.a"),
    os.path.join(FFMPEG, "lib", "libavutil.dll.a"),
    "-ld3d11", "-ldxgi", "-ldcomp", "-ld2d1", "-ldwrite", "-ldwmapi", "-lshcore",
    "-lws2_32", "-lmswsock", "-liphlpapi", "-lbcrypt", "-ladvapi32", "-lole32", "-loleaut32",
    "-luuid", "-lshell32", "-luser32", "-lgdi32", "-lavrt", "-lwinmm", "-lwinhttp",
]

SHADER_JOBS = [
    ("vs_5_0", "VSFull", "compose.hlsl", "vs_full.h", "g_VSFull"),
    ("ps_5_0", "PSCompose", "compose.hlsl", "ps_compose.h", "g_PSCompose"),
    ("ps_5_0", "PSConvert", "convert.hlsl", "ps_convert.h", "g_PSConvert"),
]

THIRD_PARTY_C = sorted(glob.glob(os.path.join(ROOT, "third_party", "ed25519", "*.c")) +
                       glob.glob(os.path.join(ROOT, "third_party", "playfair", "*.c")))
APP_CPP = sorted(p for p in glob.glob(os.path.join(ROOT, "src", "**", "*.cpp"), recursive=True))
TEST_CPP = [os.path.join(ROOT, "tools", "airglass_test.cpp")]
# Sources shared with the test tool (everything except the UI / app shell).
SHARED_FOR_TEST = [p for p in APP_CPP if os.sep + "ui" + os.sep not in p and not p.endswith("main.cpp")]


def newest_header():
    hs = glob.glob(os.path.join(ROOT, "src", "**", "*.h"), recursive=True)
    hs += glob.glob(os.path.join(SHADERS, "*.h"))
    return max(os.path.getmtime(h) for h in hs) if hs else 0


def obj_path(src):
    rel = os.path.relpath(src, ROOT).replace(os.sep, "_").replace(".", "_")
    return os.path.join(OBJ, rel + ".o")


def run(cmd):
    p = subprocess.run(cmd, capture_output=True, text=True, env=ENV)
    return p.returncode, (p.stdout + p.stderr).strip()


def build_shaders():
    os.makedirs(SHADERS, exist_ok=True)
    ok = True
    for profile, entry, src, out, var in SHADER_JOBS:
        srcp = os.path.join(ROOT, "src", "ui", src)
        outp = os.path.join(SHADERS, out)
        if os.path.exists(outp) and os.path.getmtime(outp) >= os.path.getmtime(srcp):
            continue
        rc, text = run([FXC, "/nologo", "/O3", "/T", profile, "/E", entry, "/Vn", var, "/Fh", outp, srcp])
        lines = [l for l in text.splitlines() if "compilation header save succeeded" not in l]
        if lines:
            print("\n".join(lines))
        if rc != 0:
            ok = False
        else:
            print(f"  shader {entry}")
    return ok


def compile_one(src):
    is_c = src.endswith(".c")
    compiler = os.path.join(TC, "clang.exe" if is_c else "clang++.exe")
    flags = CFLAGS if is_c else CXXFLAGS
    obj = obj_path(src)
    rc, text = run([compiler, "-c", src, "-o", obj] + flags)
    return src, rc, text


def main():
    global ENV
    ENV = dict(os.environ)
    ENV["PATH"] = TC + os.pathsep + ENV["PATH"]
    if "--clean" in sys.argv and os.path.isdir(OUT):
        shutil.rmtree(OUT)
    os.makedirs(OBJ, exist_ok=True)

    if not build_shaders():
        print("shader compilation failed")
        return 1

    hdr_time = newest_header()
    sources = THIRD_PARTY_C + APP_CPP + TEST_CPP
    todo = []
    for s in sources:
        if not os.path.exists(s):
            continue
        o = obj_path(s)
        if not os.path.exists(o):
            todo.append(s)
            continue
        t = os.path.getmtime(o)
        if os.path.getmtime(s) > t or (not s.endswith(".c") and hdr_time > t):
            todo.append(s)

    failed = False
    with cf.ThreadPoolExecutor(max_workers=os.cpu_count() or 8) as ex:
        for src, rc, text in ex.map(compile_one, todo):
            name = os.path.relpath(src, ROOT)
            if text:
                print(f"--- {name}\n{text}")
            if rc != 0:
                failed = True
            else:
                print(f"  compiled {name}")
    if failed:
        print("BUILD FAILED")
        return 1

    # Resources: icon, version info, DPI-aware manifest.
    rc = os.path.join(ROOT, "assets", "AirGlass.rc")
    res_obj = os.path.join(OBJ, "AirGlass_res.o")
    deps = [rc, os.path.join(ROOT, "assets", "AirGlass.ico"), os.path.join(ROOT, "assets", "AirGlass.manifest")]
    if not os.path.exists(res_obj) or max(os.path.getmtime(d) for d in deps) > os.path.getmtime(res_obj):
        code, text = run([os.path.join(TC, "x86_64-w64-mingw32-windres.exe"), "-I", os.path.join(ROOT, "assets"),
                          "-i", rc, "-o", res_obj, "--output-format=coff"])
        if text:
            print(text)
        if code != 0:
            print("RESOURCE COMPILE FAILED")
            return 1
        print("  compiled assets/AirGlass.rc")

    clangxx = os.path.join(TC, "clang++.exe")
    common_objs = [obj_path(s) for s in THIRD_PARTY_C]
    app_objs = common_objs + [obj_path(s) for s in APP_CPP] + [res_obj]
    rc, text = run([clangxx, "-O2", "-static", "-mwindows", "-municode", "-o", os.path.join(OUT, "AirGlass.exe")] +
                   app_objs + LIBS)
    if text:
        print(text)
    if rc != 0:
        print("LINK FAILED (AirGlass.exe)")
        return 1
    print("  linked build/AirGlass.exe" + (" (Pro always unlocked)" if PRO else ""))

    if os.path.exists(TEST_CPP[0]):
        test_objs = common_objs + [obj_path(s) for s in SHARED_FOR_TEST] + [obj_path(TEST_CPP[0])]
        ff_extra = [os.path.join(FFMPEG, "lib", "libavformat.dll.a")]
        rc, text = run([clangxx, "-O2", "-static", "-o", os.path.join(OUT, "airglass_test.exe")] + test_objs +
                       ff_extra + LIBS)
        if text:
            print(text)
        if rc != 0:
            print("LINK FAILED (airglass_test.exe)")
            return 1
        print("  linked build/airglass_test.exe")

    for dll in glob.glob(os.path.join(FFMPEG, "bin", "*.dll")):
        base = os.path.basename(dll)
        if base.split("-")[0] in ("avcodec", "avutil", "swresample", "avformat"):
            dst = os.path.join(OUT, base)
            if not os.path.exists(dst) or os.path.getmtime(dst) < os.path.getmtime(dll):
                shutil.copy2(dll, dst)
    print("BUILD OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
