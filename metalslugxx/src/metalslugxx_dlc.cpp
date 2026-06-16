// metalslugxx - ReXGlue Recompiled Project
//
// See metalslugxx_dlc.h for the contract and the unlock mechanism.

#include "metalslugxx_dlc.h"

#include <fstream>
#include <system_error>

#include <rex/logging/api.h>
#include <rex/logging/macros.h>

namespace metalslugxx {

namespace {

// MSXX title id (PE XBLA_MS7_ReleaseOuter, base 0x82000000). The SDK content
// manager keys packages by the running title id, formatted as 8 uppercase hex
// digits -- mirror that exactly so our package lands where it enumerates.
constexpr const char* kTitleIdDir = "58410972";

// Marketplace content lives under the common (xuid 0) tree; content type
// 0x00000002 = XContentType::kMarketplaceContent.
constexpr const char* kCommonXuidDir = "0000000000000000";
constexpr const char* kMarketplaceTypeDir = "00000002";

// Our synthetic package's folder name (= the content file_name). Shared with the
// app via kLeonaPackageName so the marketplace_content_redirect cvar points here.
constexpr const char* kPackageDirName = kLeonaPackageName;

// The file the game opens (DLC0:\0.id) and its expected 5-byte contents.
constexpr const char* kIdFileName = "0.id";
constexpr const char* kIdFileContents = "leona";

// <user_data_root>/0000000000000000/58410972/00000002/rexleona
std::filesystem::path PackageDir(const std::filesystem::path& user_data_root) {
  return user_data_root / kCommonXuidDir / kTitleIdDir / kMarketplaceTypeDir / kPackageDirName;
}

}  // namespace

void ApplyLeonaUnlock(const std::filesystem::path& user_data_root, bool unlock) {
  if (user_data_root.empty()) {
    REXLOG_WARN("leona: user data root is empty; cannot {} the DLC",
                unlock ? "create" : "remove");
    return;
  }

  const std::filesystem::path package_dir = PackageDir(user_data_root);
  const std::filesystem::path id_path = package_dir / kIdFileName;

  if (!unlock) {
    // Remove only our synthetic package, never the whole marketplace tree.
    std::error_code ec;
    if (std::filesystem::exists(package_dir, ec)) {
      std::filesystem::remove_all(package_dir, ec);
      if (ec) {
        REXLOG_WARN("leona: UnlockLeona = False but could not remove {} ({})",
                    package_dir.string(), ec.message());
      } else {
        REXLOG_INFO("leona: UnlockLeona = False -> removed synthetic DLC {}", package_dir.string());
      }
    } else {
      REXLOG_INFO("leona: UnlockLeona = False -> no synthetic DLC present");
    }
    return;
  }

  std::error_code ec;
  std::filesystem::create_directories(package_dir, ec);
  if (ec) {
    REXLOG_WARN("leona: could not create DLC directory {} ({})", package_dir.string(),
                ec.message());
    return;
  }

  // Write 0.id ("leona"). Overwrite unconditionally so a truncated/garbled file
  // from a previous run is always corrected; it is only 5 bytes.
  std::ofstream out(id_path, std::ios::binary | std::ios::trunc);
  if (!out) {
    REXLOG_WARN("leona: could not write {}", id_path.string());
    return;
  }
  out.write(kIdFileContents, 5);
  out.close();
  if (!out) {
    REXLOG_WARN("leona: failed while writing {}", id_path.string());
    return;
  }

  REXLOG_INFO("leona: UnlockLeona = True -> synthetic DLC ready at {}", id_path.string());
}

}  // namespace metalslugxx
