#pragma once
#include "Support.h"

namespace aot::driver {
using PackageFiles = std::map<std::string, std::string>;
llvm::Error writePackage(const fs::path &output, const PackageFiles &files);
llvm::Expected<PackageFiles> readPackage(const fs::path &input);
llvm::Expected<llvm::json::Value> validatePackage(const PackageFiles &files);
bool validName(llvm::StringRef name);
}
