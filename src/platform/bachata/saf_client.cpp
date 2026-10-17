// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "platform/bachata/saf_client.h"

#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string_view>
#include <system_error>
#include <vector>

#ifndef _WIN32
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#endif

namespace Platform::Bachata {
namespace {

constexpr std::uint32_t BrokerMagic = 0x42534631; // "BSF1"
constexpr std::uint8_t BrokerVersion = 1;
constexpr std::uint8_t BrokerOpOpen = 3;
constexpr std::size_t BrokerHeaderSize = 10;

void PutU16(std::vector<char>& out, std::uint16_t value) {
    out.push_back(static_cast<char>(value >> 8));
    out.push_back(static_cast<char>(value));
}

void PutU32(std::vector<char>& out, std::uint32_t value) {
    for (int shift = 24; shift >= 0; shift -= 8) {
        out.push_back(static_cast<char>(value >> shift));
    }
}

std::uint32_t GetU32(const unsigned char* data) {
    return (std::uint32_t{data[0]} << 24) | (std::uint32_t{data[1]} << 16) |
           (std::uint32_t{data[2]} << 8) | std::uint32_t{data[3]};
}

bool IsReadableFile(const std::filesystem::path& path) {
    std::error_code ec;
    return std::filesystem::is_regular_file(path, ec) && ::access(path.c_str(), R_OK) == 0;
}

// The provider opens files on the lower filesystem or a FUSE mount; the app process only
// sees them through /storage. Map the known Android mount layouts back onto it.
std::vector<std::filesystem::path> StorageCandidates(const std::string& real) {
    std::vector<std::filesystem::path> candidates{real};
    const auto strip_user = [&](std::string_view prefix) -> std::optional<std::string> {
        if (!real.starts_with(prefix)) {
            return std::nullopt;
        }
        auto rest = std::string_view{real}.substr(prefix.size());
        const auto slash = rest.find('/');
        if (slash == std::string_view::npos) {
            return std::nullopt;
        }
        return std::string{rest.substr(slash + 1)};
    };
    if (real.starts_with("/mnt/media_rw/")) {
        candidates.emplace_back("/storage/" + real.substr(14));
    }
    if (real.starts_with("/data/media/")) {
        candidates.emplace_back("/storage/emulated/" + real.substr(12));
    }
    for (const auto prefix : {std::string_view{"/mnt/user/"}, std::string_view{"/mnt/pass_through/"},
                              std::string_view{"/mnt/runtime/"}}) {
        if (const auto rest = strip_user(prefix)) {
            candidates.emplace_back("/storage/" + *rest);
        }
    }
    return candidates;
}

} // namespace

std::optional<std::filesystem::path> ResolveSafGamePath(const std::string& socket_name,
                                                        const std::string& token,
                                                        const std::string& relative_path,
                                                        std::string& error) {
#ifdef _WIN32
    error = "SAF broker is Android-only";
    return std::nullopt;
#else
    error.clear();
    if (socket_name.empty() || socket_name.size() + 1 > sizeof(sockaddr_un::sun_path) ||
        token.empty() || token.size() > 128 || relative_path.size() > 4096) {
        error = "invalid SAF socket, token or path";
        return std::nullopt;
    }

    const int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        error = std::string("socket: ") + std::strerror(errno);
        return std::nullopt;
    }
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::memcpy(addr.sun_path + 1, socket_name.data(), socket_name.size()); // abstract
    const auto addr_len =
        static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + 1 + socket_name.size());
    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), addr_len) != 0) {
        error = std::string("connect: ") + std::strerror(errno);
        ::close(fd);
        return std::nullopt;
    }

    std::vector<char> request;
    PutU32(request, BrokerMagic);
    request.push_back(static_cast<char>(BrokerVersion));
    request.push_back(static_cast<char>(BrokerOpOpen));
    PutU16(request, static_cast<std::uint16_t>(token.size()));
    PutU32(request, static_cast<std::uint32_t>(relative_path.size()));
    request.insert(request.end(), token.begin(), token.end());
    request.insert(request.end(), relative_path.begin(), relative_path.end());
    for (std::size_t sent = 0; sent < request.size();) {
        const ssize_t n = ::send(fd, request.data() + sent, request.size() - sent, MSG_NOSIGNAL);
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n <= 0) {
            error = std::string("send: ") + std::strerror(errno);
            ::close(fd);
            return std::nullopt;
        }
        sent += static_cast<std::size_t>(n);
    }

    std::array<unsigned char, BrokerHeaderSize> header{};
    int file_fd = -1;
    for (std::size_t got = 0; got < header.size();) {
        iovec iov{header.data() + got, header.size() - got};
        alignas(cmsghdr) char control[CMSG_SPACE(sizeof(int))]{};
        msghdr msg{};
        msg.msg_iov = &iov;
        msg.msg_iovlen = 1;
        msg.msg_control = control;
        msg.msg_controllen = sizeof(control);
        const ssize_t n = ::recvmsg(fd, &msg, MSG_CMSG_CLOEXEC);
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n <= 0) {
            error = n == 0 ? "broker closed the connection" : std::string("recvmsg: ") +
                                                                   std::strerror(errno);
            break;
        }
        for (cmsghdr* c = CMSG_FIRSTHDR(&msg); c != nullptr; c = CMSG_NXTHDR(&msg, c)) {
            if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_RIGHTS && file_fd < 0) {
                std::memcpy(&file_fd, CMSG_DATA(c), sizeof(int));
            }
        }
        got += static_cast<std::size_t>(n);
    }
    ::close(fd);
    if (!error.empty()) {
        if (file_fd >= 0) {
            ::close(file_fd);
        }
        return std::nullopt;
    }

    const auto status = header[5];
    if (GetU32(header.data()) != BrokerMagic || header[4] != BrokerVersion || status != 0 ||
        file_fd < 0) {
        static constexpr std::array<const char*, 5> StatusNames = {
            "ok without fd", "not found", "provider error", "security", "bad token"};
        error = std::string("broker open failed: ") +
                (status < StatusNames.size() ? StatusNames[status] : "bad response");
        if (file_fd >= 0) {
            ::close(file_fd);
        }
        return std::nullopt;
    }

    std::array<char, 4096> link{};
    const auto proc_path = "/proc/self/fd/" + std::to_string(file_fd);
    const ssize_t link_len = ::readlink(proc_path.c_str(), link.data(), link.size() - 1);
    ::close(file_fd);
    if (link_len <= 0) {
        error = std::string("readlink: ") + std::strerror(errno);
        return std::nullopt;
    }

    const std::string real(link.data(), static_cast<std::size_t>(link_len));
    for (const auto& candidate : StorageCandidates(real)) {
        if (IsReadableFile(candidate)) {
            return candidate;
        }
    }
    error = "SAF file " + real + " is not readable directly (grant all-files access)";
    return std::nullopt;
#endif
}

} // namespace Platform::Bachata
