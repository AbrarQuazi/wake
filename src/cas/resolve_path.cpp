/*
 * Copyright 2026 SiFive, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You should have received a copy of LICENSE.Apache2 along with
 * this software. If not, you may obtain a copy at
 *
 *    https://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "resolve_path.h"

#include <errno.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <optional>
#include <sstream>
#include <vector>

namespace cas {

namespace {

constexpr const char* kDirectoryHash =
    "0000000000000000000000000000000000000000000000000000000000000000";

std::string legacy_exotic_hash() {
  ContentHash out;
  out.data[0] = 1;
  return out.to_hex();
}

wcl::result<std::string, std::string> read_symlink_target(const std::string& path) {
  std::vector<char> buffer(8192, 0);

  while (true) {
    ssize_t bytes_read = readlink(path.c_str(), buffer.data(), buffer.size());
    if (bytes_read < 0) {
      std::stringstream str;
      str << "readlink(" << path << "): " << strerror(errno);
      return wcl::make_error<std::string, std::string>(str.str());
    }
    if (static_cast<size_t>(bytes_read) != buffer.size()) {
      return wcl::make_result<std::string, std::string>(std::string(buffer.data(), bytes_read));
    }
    buffer.resize(2 * buffer.size(), 0);
  }
}

Cas* resolve_store(ResolvePathMode mode, Cas* store, std::optional<Cas>& opened_store,
                   std::string* error) {
  if (mode != ResolvePathMode::StoreInCas) return nullptr;
  if (store != nullptr) return store;

  auto store_result = Cas::open(".cas");
  if (!store_result) {
    *error = "failed to open CAS store at .cas";
    return nullptr;
  }

  opened_store.emplace(std::move(*store_result));
  return &*opened_store;
}

}  // namespace

wcl::result<ResolvedPath, std::string> resolve_path(const std::string& path, ResolvePathMode mode,
                                                    SpecialFilePolicy special_policy,
                                                    Cas* store) {
  struct stat statbuf;
  if (lstat(path.c_str(), &statbuf) != 0) {
    std::stringstream str;
    str << "lstat(" << path << "): " << strerror(errno);
    return wcl::make_error<ResolvedPath, std::string>(str.str());
  }

  std::optional<Cas> opened_store;
  std::string store_error;
  Cas* cas_store = resolve_store(mode, store, opened_store, &store_error);
  if (mode == ResolvePathMode::StoreInCas && cas_store == nullptr) {
    return wcl::make_error<ResolvedPath, std::string>(store_error);
  }

  if (S_ISDIR(statbuf.st_mode)) {
    return wcl::make_result<ResolvedPath, std::string>(ResolvedPath{"directory", kDirectoryHash});
  }

  if (S_ISLNK(statbuf.st_mode)) {
    auto target_result = read_symlink_target(path);
    if (!target_result) {
      return wcl::make_error<ResolvedPath, std::string>(target_result.error());
    }

    std::string hash;
    if (mode == ResolvePathMode::StoreInCas) {
      auto hash_result = cas_store->store_blob(*target_result);
      if (!hash_result) {
        std::stringstream str;
        str << "failed to store symlink in CAS: " << path;
        return wcl::make_error<ResolvedPath, std::string>(str.str());
      }
      hash = hash_result->to_hex();
    } else {
      hash = ContentHash::from_string(*target_result).to_hex();
    }

    return wcl::make_result<ResolvedPath, std::string>(ResolvedPath{"symlink", hash});
  }

  if (S_ISREG(statbuf.st_mode)) {
    std::string hash;
    if (mode == ResolvePathMode::StoreInCas) {
      auto hash_result = cas_store->store_blob_from_file(path);
      if (!hash_result) {
        std::stringstream str;
        str << "failed to store file in CAS: " << path;
        return wcl::make_error<ResolvedPath, std::string>(str.str());
      }
      hash = hash_result->to_hex();
    } else {
      auto hash_result = ContentHash::from_file(path);
      if (!hash_result) {
        std::stringstream str;
        str << "read(" << path << "): " << strerror(hash_result.error());
        return wcl::make_error<ResolvedPath, std::string>(str.str());
      }
      hash = hash_result->to_hex();
    }

    return wcl::make_result<ResolvedPath, std::string>(ResolvedPath{"file", hash});
  }

  if (special_policy == SpecialFilePolicy::LegacyExoticHash) {
    return wcl::make_result<ResolvedPath, std::string>(ResolvedPath{"file", legacy_exotic_hash()});
  }

  std::stringstream str;
  str << "unsupported file type for " << path;
  return wcl::make_error<ResolvedPath, std::string>(str.str());
}

}  // namespace cas
