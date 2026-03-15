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

#pragma once

#include <string>

#include "cas.h"
#include "wcl/result.h"

namespace cas {

enum class ResolvePathMode { HashOnly, StoreInCas };
enum class SpecialFilePolicy { Reject, LegacyExoticHash };

struct ResolvedPath {
  std::string type;
  std::string hash;
};

wcl::result<ResolvedPath, std::string> resolve_path(const std::string& path, ResolvePathMode mode,
                                                    SpecialFilePolicy special_policy,
                                                    Cas* store = nullptr);

}  // namespace cas
