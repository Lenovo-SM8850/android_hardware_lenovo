// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "Graphics.h"
#include <memory>

namespace gamepolicy {
std::unique_ptr<GraphicsBackend> makeQspmClient();
} // namespace gamepolicy
