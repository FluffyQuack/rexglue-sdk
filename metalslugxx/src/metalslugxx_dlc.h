// metalslugxx - ReXGlue Recompiled Project
//
// Leona DLC unlock.
//
// On Xbox 360, the playable character Leona was gated behind a downloadable
// marketplace add-on. The DLC package contained a single 5-byte file, `0.id`,
// whose contents were the literal text "leona". At boot the game enumerates its
// marketplace content, mounts each package under the root name `DLC0`, opens
// `DLC0:\0.id`, and unlocks Leona when it reads back "leona" (strings
// `DLC0`, `DLC0:\0.id` are present in the executable).
//
// We reproduce that add-on entirely on the host filesystem rather than touching
// guest code: when enabled we materialize a synthetic marketplace content
// package (a directory holding a correct `0.id`) inside the SDK content root,
// using the exact on-disk layout the SDK's ContentManager enumerates
//   <user_data_root>/0000000000000000/<title>/00000002/<name>/0.id
// The game's own XamContentCreateEnumerator -> XamContentCreateEx -> file-read
// path then finds it through the SDK's existing, tested content machinery and
// unlocks Leona. No header file is needed: ContentManager::ListContent falls
// back to synthesising the content metadata from the package directory name.

#pragma once

#include <filesystem>

namespace metalslugxx {

// The synthetic Leona package's content file_name. It is the directory name of
// the package on disk and the value the SDK's marketplace_content_redirect cvar
// is pointed at so the game's DLC open resolves to it. The game does not care
// about the name (it identifies the add-on by reading 0.id); a real installed
// Leona DLC carries a different STFS-derived name, so this stays distinct.
inline constexpr const char* kLeonaPackageName = "rexleona";

// Ensures the synthetic Leona marketplace DLC exists (when `unlock` is true) or
// is removed (when `unlock` is false) under `user_data_root` (the SDK content
// root). Safe to call once at startup, before the runtime mounts content. Logs
// what it did. A no-op (with a warning) if `user_data_root` is empty.
void ApplyLeonaUnlock(const std::filesystem::path& user_data_root, bool unlock);

}  // namespace metalslugxx
