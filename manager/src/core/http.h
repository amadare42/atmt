// http.h - HTTPS GETs for the update check (docs/MANAGER.md).
//
// WinHTTP on Windows. On Linux the system's libcurl, loaded at run time (dlopen libcurl.so.4):
// SteamOS ships it with its CA bundle, the AppImage then needs no TLS stack of its own, and on a
// system without it the update check says so instead of the app failing to start. Only GETs, no
// identifiers, no cookies.
#pragma once

#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace atmt {

struct HttpResponse {
    int status = 0;          // 0: no response at all (see error)
    std::string body;
    std::string etag;
    std::string error;
};

// Called with (bytes so far, total or 0); returning false cancels.
using ProgressFn = std::function<bool(uint64_t, uint64_t)>;

class Http {
public:
    virtual ~Http() = default;
    virtual bool Get(const std::string& url, const std::vector<std::pair<std::string, std::string>>& headers,
                     HttpResponse* out, const ProgressFn& progress = nullptr) = 0;
};

// The platform's client; null (with `error`) when there is none (no libcurl on this system).
std::unique_ptr<Http> MakeHttp(std::string* error = nullptr);

}  // namespace atmt
