# Reconstructing the Project Folder After Cloning

The git repo only contains source, build scripts, and vendored prebuilt developer libraries - see `WORKSPACE_GUIDE.md` for details. Toolchains, game assets, and build outputs are regenerated or obtained locally.

## Clone
```powershell
git clone <repo-url> "SnF PC Port"
cd "SnF PC Port"
```

## Get the Toolchain
Not committed to the repository:
```powershell
# Download portable w64devkit and portable CMake, unzip into:
tools/w64devkit/
tools/cmake/
```
(Pin GCC 16.2.0 and CMake 4.4.2 for reproducibility.)

Set the toolchain in your current PowerShell session:
```powershell
$env:PATH = "$PWD\tools\w64devkit\bin;$PWD\tools\cmake\bin;" + $env:PATH
```

## Verify Vendored third_party/ Paths
`third_party/` uses vendored headers and prebuilt libraries to eliminate Boost and Dynarmic build trees from everyday compilation:
```
third_party/SDL2/x86_64-w64-mingw32/include/
third_party/SDL2/x86_64-w64-mingw32/lib/libSDL2.dll.a
third_party/SDL2/x86_64-w64-mingw32/bin/SDL2.dll
third_party/openal/include/
third_party/openal/libs/Win64/libOpenAL32.dll.a
third_party/openal/bin/Win64/soft_oal.dll
third_party/dynarmic/include/
third_party/dynarmic/lib/windows-x64/libdynarmic.a
```

## Compile the Binaries

### Option 1: Using the Automated Build Scripts (Recommended)
```cmd
:: Production build (default, lean, silent, zero logging overhead):
.\build.bat

:: Or Debug build (with telemetry, profiler, and call-site leak tracking):
.\build.bat --debug
```
*Or in PowerShell:*
```powershell
.\build.ps1          # Production
.\build.ps1 -debug   # Debug
```

### Option 2: Manual CMake Commands
```powershell
# Production (default):
cmake -B build_app -G Ninja -DCMAKE_BUILD_TYPE=Release -DENABLE_DEBUG=OFF
cmake --build build_app

# Or Debug:
cmake -B build_app -G Ninja -DCMAKE_BUILD_TYPE=Debug -DENABLE_DEBUG=ON
cmake --build build_app
```
This compiles `src/`, links against the vendored libraries (Dynarmic, SDL2, OpenAL, OpenGL), and auto-deploys `PoPSnF_Reignited.exe`, `PoPSnF_Launcher.exe`, `SDL2.dll`, and `OpenAL32.dll` directly into `game/`.

## Provide the Original Game Data (1-Click APK Extraction)
A legally obtained copy of the original Android release (`.apk` file) is required:

### Option A: Via the Launcher (GUI)
1. Launch `.\game\PoPSnF_Launcher.exe`.
2. Click **"Extract Game Assets"** (or click "Launch the game!", which prompts automatically if assets are missing).
3. Select your `.apk` file in the Windows file dialog.
4. The launcher unpacks `libS3DClient.so`, `S3DMain.stk`, and `app_splash.png` directly into `game/` in **under 1 second**.

### Option B: Via Command Line (Headless)
```powershell
.\game\PoPSnF_Launcher.exe --extract "path\to\PrinceOfPersia_ShadowAndFlame.apk"
```

> [!NOTE]
> **Pristine STK-Only Runtime**: Unlike older builds, **no loose asset folders** (`Resources/`, `Models/`, `Scenes/`, `Games/`, etc.) are needed. The engine streams all 4,617 game assets directly from `S3DMain.stk` via our Virtual File System bridge.

## Run and Configure
Launch the configuration utility:
```powershell
.\game\PoPSnF_Launcher.exe
```
From here you can customize display resolutions (up to 4K), toggle Fullscreen/VSync, set MSAA antialiasing and Anisotropic filtering, remap keyboard/mouse/gamepad controls with conflict swapping and undo/redo, and launch the game.

Or start the game directly:
```powershell
.\game\PoPSnF_Reignited.exe
```

---

## What is NOT in the Repository, and Why

| Excluded | Reason |
| :--- | :--- |
| `game/` | Compiled output + Ubisoft's copyrighted game assets |
| `ref_assets/` | Original copyrighted platform dumps (APK/IPA/STK) |
| `tools/` | Large portable toolchain binaries (GCC/CMake) |
| `build_app/` | CMake build directory, fully reproducible from `src/` + `CMakeLists.txt` |
| `screenshots/`, `*.log`, `*.xkv` | Generated runtime and test output |

---

## Appendix: Rebuilding the Precompiled Dynarmic Fat Archive

Boost and Dynarmic sources are omitted from the repo to keep the directory lean. If you need to regenerate `third_party/dynarmic/lib/windows-x64/libdynarmic.a` from upstream source:

1. **Obtain Sources**:
   ```powershell
   git clone --recursive [https://github.com/merryhime/dynarmic.git](https://github.com/merryhime/dynarmic.git) temp_dynarmic
   # Provide Boost 1.84.0 headers at temp_boost
   ```

2. **Configure and Build Minimal ARM32 Target**:
   ```powershell
   cmake -S temp_dynarmic -B build_dynarmic -G Ninja -DCMAKE_BUILD_TYPE=Release `
     "-DBOOST_ROOT=$PWD/temp_boost" `
     "-DBoost_INCLUDE_DIR=$PWD/temp_boost" `
     "-DCMAKE_POLICY_VERSION_MINIMUM=3.5" `
     -DDYNARMIC_TESTS=OFF -DDYNARMIC_TARGET_ARM64=OFF -DDYNARMIC_ENABLE_DISASSEMBLER=OFF -DBUILD_SHARED_LIBS=OFF

   cmake --build build_dynarmic --config Release
   ```

3. **Merge Static Archives into a Fat Library**:
   ```powershell
   New-Item -ItemType Directory -Force -Path third_party/dynarmic/lib/windows-x64 | Out-Null

   @"
   CREATE third_party/dynarmic/lib/windows-x64/libdynarmic.a
   ADDLIB build_dynarmic/src/dynarmic/libdynarmic.a
   ADDLIB build_dynarmic/externals/fmt/libfmt.a
   ADDLIB build_dynarmic/externals/mcl/src/libmcl.a
   ADDLIB build_dynarmic/externals/zydis/libZydis.a
   ADDLIB build_dynarmic/externals/zydis/zycore/libZycore.a
   SAVE
   END
   "@ | Set-Content combine.mri -Encoding Ascii

   cmd /c "ar -M < combine.mri"
   Remove-Item combine.mri
   ```

4. **Stage Interface and Common Headers**:
   ```powershell
   New-Item -ItemType Directory -Force -Path third_party/dynarmic/include/dynarmic | Out-Null
   Copy-Item -Recurse -Force temp_dynarmic/src/dynarmic/interface third_party/dynarmic/include/dynarmic/
   Copy-Item -Recurse -Force temp_dynarmic/src/dynarmic/common third_party/dynarmic/include/dynarmic/
   Copy-Item -Recurse -Force temp_dynarmic/src/dynarmic/frontend third_party/dynarmic/include/dynarmic/
   Copy-Item -Recurse -Force temp_dynarmic/src/dynarmic/ir third_party/dynarmic/include/dynarmic/
   Copy-Item -Recurse -Force temp_dynarmic/externals/mcl/include/mcl third_party/dynarmic/include/

   # Strip implementation files from include directory
   Get-ChildItem -Path third_party/dynarmic/include -Recurse -Filter *.cpp | Remove-Item -Force
   Remove-Item -Recurse -Force temp_dynarmic, temp_boost, build_dynarmic
   ```
