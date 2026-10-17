// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <filesystem>
#include <optional>
#include <string>

namespace Platform::Bachata {

// Android 0.2.5 "direct" games live behind a SAF broker (abstract LocalSocket). The broker
// hands out read-only fds; we open the eboot once, read the fd's real path and map it back to
// a /storage path the runtime can read directly (requires all-files access for the app).
std::optional<std::filesystem::path> ResolveSafGamePath(const std::string& socket_name,
                                                        const std::string& token,
                                                        const std::string& relative_path,
                                                        std::string& error);

} // namespace Platform::Bachata
