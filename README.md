Modified version of ReXGlue SDK that can run Metal Slug XX.

> [!CAUTION]
> This project is in early development. Expect things to not work quite right and there to be significant changes and breaking public API updates as development progresses. Contributions and feedback are welcome, but please be aware that the codebase is still evolving rapidly.

<h1 align="center">
  <br>
  <a href="https://github.com/rexglue/rexglue-sdk">
    <img src="https://github.com/rexglue/rexglue-media/blob/main/ReX_Banner.png" alt="ReXGlue banner">
  </a>
  <br>
  <br>
  <a href="https://discord.gg/CNTxwSNZfT">
    <img src="https://img.shields.io/badge/Discord-Join%20Server-5865F2?logo=discord&logoColor=white" alt="Discord">
  </a>
  <a href="https://github.com/rexglue/rexglue-sdk/stargazers">
    <img src="https://img.shields.io/github/stars/rexglue/rexglue-sdk" alt="rexglue-sdk stargazers">
  </a>
</h1>

ReXGlue converts Xbox 360 PowerPC code into portable C++ that runs natively on modern platforms.

ReXGlue is heavily rooted on the foundations of [Xenia](https://github.com/xenia-project), the Xbox 360 emulator. Rather than interpreting or JIT-compiling PPC instructions at runtime, ReXGlue takes a different path: it generates C++ source code ahead of time, an approach inspired by [XenonRecomp](https://github.com/hedge-dev/XenonRecomp) and [rexdex's recompiler](https://github.com/rexdex/recompiler).

Latest SDK builds and releases are published on [GitHub Releases](https://github.com/rexglue/rexglue-sdk/releases). Join the [Discord server](https://discord.gg/CNTxwSNZfT) for updates and share what you have created.

## Builds

| Channel | CI | Download |
| --- | --- | --- |
| Release | [![win-amd64](https://github.com/rexglue/rexglue-sdk/actions/workflows/build-win-amd64.yaml/badge.svg)](https://github.com/rexglue/rexglue-sdk/actions/workflows/build-win-amd64.yaml) [![linux-amd64](https://github.com/rexglue/rexglue-sdk/actions/workflows/build-linux-amd64.yaml/badge.svg)](https://github.com/rexglue/rexglue-sdk/actions/workflows/build-linux-amd64.yaml) [![linux-arm64](https://github.com/rexglue/rexglue-sdk/actions/workflows/build-linux-aarch64.yaml/badge.svg)](https://github.com/rexglue/rexglue-sdk/actions/workflows/build-linux-aarch64.yaml) | [Latest stable](https://github.com/rexglue/rexglue-sdk/releases/latest) |
| Nightly | [![nightly](https://github.com/rexglue/rexglue-sdk/actions/workflows/nightly.yaml/badge.svg)](https://github.com/rexglue/rexglue-sdk/actions/workflows/nightly.yaml) | [Latest pre-release](https://github.com/rexglue/rexglue-sdk/releases?q=prerelease%3Atrue) |

## Quickstart

For quick start guide, full CLI reference, and config file options, see the [wiki](https://github.com/rexglue/rexglue-sdk/wiki).

## Building Metal Slug XX from source

These steps compile the recompiled C++ into the two binaries the game needs:

- **`rexruntime.dll` / `rexruntimerd.dll`** — the SDK runtime (everything under `src/`). Built from the SDK source tree.
- **`metalslugxx.exe`** — the recompiled game host (everything under `metalslugxx/`).

The game consumes the SDK via `add_subdirectory` (`-DREXSDK_DIR=../`), so a single
build configures and compiles **both** at once. Editing anything in the SDK's
`src/` rebuilds the runtime DLL; editing `metalslugxx/src/` rebuilds the exe.

### Prerequisites

- **Visual Studio 2022** (the Build Tools workload is enough). The presets use
  bare `clang`/`clang++` + `lld-link` against the MSVC toolchain and UCRT, so a VS
  Developer environment (`vcvars64.bat`) must populate `INCLUDE`/`LIB`/`PATH`
  before CMake runs.
- **Clang/LLVM 18 or newer** with `clang` and `clang++` on `PATH`.
- **CMake 3.25+** and **Ninja**.

### Build (the easy way)

From a normal terminal, run the helper script for the configuration you want.
Each one calls `vcvars64.bat`, configures the CMake preset with `-DREXSDK_DIR=../`,
and builds:

```bat
metalslugxx\scripts\build-rlsdebug.bat   :: RelWithDebInfo -> metalslugxx.exe + rexruntimerd.dll
metalslugxx\scripts\build-release.bat    :: Release        -> metalslugxx.exe + rexruntime.dll
```

> The scripts hardcode `C:\Program Files\Microsoft Visual Studio\2022\Community\...`.
> If VS is installed elsewhere (e.g. Professional/Enterprise/BuildTools), edit the
> `vcvars64.bat` path at the top of the script.

### Build (manual steps)

Equivalent to what the scripts do, from a **VS Developer (x64) prompt** with the
working directory at the `metalslugxx/` folder:

```bat
:: Configure (point the build at the SDK source tree one level up)
cmake --preset win-amd64-relwithdebinfo -DREXSDK_DIR=../

:: Compile both the runtime DLL and the game exe
cmake --build --preset win-amd64-relwithdebinfo
```

Swap `relwithdebinfo` for `release` (or `debug`) to change configuration.

### Output

Both binaries land side-by-side in the preset's build directory:

| Configuration | Path | Runtime DLL |
| --- | --- | --- |
| RelWithDebInfo | `metalslugxx/out/build/win-amd64-relwithdebinfo/metalslugxx.exe` | `rexruntimerd.dll` |
| Release | `metalslugxx/out/build/win-amd64-release/metalslugxx.exe` | `rexruntime.dll` |
| Debug | `metalslugxx/out/build/win-amd64-debug/metalslugxx.exe` | `rexruntimed.dll` |

The `.exe` loads the matching `.dll` from its own directory, so keep them together.

### Regenerating the recompiled C++ (optional)

The lifted PPC→C++ sources are already committed under `metalslugxx/generated/`.
You only need to re-run codegen if you change the lifting hints (the
`metalslugxx_*.toml` files) or the source XEX. To regenerate, then rebuild:

```bat
cmake --build --preset win-amd64-relwithdebinfo --target metalslugxx_codegen
cmake --build --preset win-amd64-relwithdebinfo
```

### Building only the SDK runtime DLL (optional)

To build the runtime DLL without the game, configure and build the SDK root
directly from a VS Developer prompt at the repository root:

```bat
cmake --preset win-amd64
cmake --build --preset win-amd64-relwithdebinfo   :: or win-amd64-release
```

The DLL is written to `out/win-amd64/<Config>/rexruntime[rd].dll`.

### Running

Running needs no VS environment. Point the host at your extracted game data:

```bat
cd metalslugxx\out\build\win-amd64-relwithdebinfo
metalslugxx.exe --game_data_root "C:\path\to\MSXX"
```

# **Disclaimer**
ReXGlue is not affiliated with nor endorsed by Microsoft or Xbox. It is an independent project created for educational and development purposes. All trademarks and copyrights belong to their respective owners. 

This project is not intended to promote piracy nor unauthorized use of copyrighted material. Any misuse of this software to endorse or enable this type of activity is strictly prohibited.


# Credits

## ReXGlue
- [Tom (crack)](https://github.com/tomcl7) - Project Founder
- [Loreaxe](https://github.com/Loreaxe) - Linux Contributor
- [mystixor](https://github.com/Mystixor) - Windows Contributor
- [Graine25](https://github.com/Graine25) - Project Support
- [Carlos Estrague (mrcmunir)](https://github.com/mrcmunir) - Linux / ARM64 Contributor
- [sanjay900](https://github.com/sanjay900) - Linux / SDL Contributor
- [Toby](https://github.com/TbyDtch) - Project Support
- [Roxxsen](https://github.com/Roxxsen) - CI/CD Contributor

The list above is not exhaustive. Thanks to everyone in the ReXGlue community who contributes code, files issues, tests builds, and keeps the project moving.

## Very Special Thank You:
- [Project Xenia](https://github.com/xenia-project/xenia/tree/master/src/xenia) - Their invaluable work on Xbox 360 emulation laid the groundwork for ReXGlue's development. This project (and numerous others) would not exist without their hard work and dedication.
- [XenonRecomp](https://github.com/hedge-dev/XenonRecomp) - For pioneering the modern static recompilation approach for Xbox 360. A lot of the codegen analysis logic and instruction translations are based on their work. Thank you!
- [rexdex's recompiler](https://github.com/rexdex/recompiler) - The OG static recompiler for Xbox 360. 
- Many others in the Xbox 360 homebrew and modding communities whose work and research have contributed to the collective knowledge that makes projects like this possible.
