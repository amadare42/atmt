// http.cpp - see http.h.
#include "http.h"

#include "payload.h"
#include "util.h"

#ifdef _WIN32
#include <windows.h>
#include <winhttp.h>
#else
#include <dlfcn.h>
#endif

namespace atmt {

namespace {

std::string UserAgent() { return std::string("atmt-manager/") + AppVersion(); }

}  // namespace

#ifdef _WIN32

namespace {

std::wstring Wide(const std::string& s) {
    if (s.empty()) return std::wstring();
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), &w[0], n);
    return w;
}

class WinHttp : public Http {
public:
    bool Get(const std::string& url, const std::vector<std::pair<std::string, std::string>>& headers, HttpResponse* out,
             const ProgressFn& progress) override {
        *out = HttpResponse();
        const std::wstring wurl = Wide(url);
        URL_COMPONENTS parts;
        ZeroMemory(&parts, sizeof(parts));
        parts.dwStructSize = sizeof(parts);
        wchar_t host[256], path[4096];
        parts.lpszHostName = host;
        parts.dwHostNameLength = 256;
        parts.lpszUrlPath = path;
        parts.dwUrlPathLength = 4096;
        wchar_t extra[4096];
        parts.lpszExtraInfo = extra;
        parts.dwExtraInfoLength = 4096;
        if (!WinHttpCrackUrl(wurl.c_str(), 0, 0, &parts)) {
            out->error = "bad url: " + url;
            return false;
        }
        if (parts.nScheme != INTERNET_SCHEME_HTTPS) {
            out->error = "only https is used: " + url;
            return false;
        }
        HINTERNET session = WinHttpOpen(Wide(UserAgent()).c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        if (session == nullptr) {
            session = WinHttpOpen(Wide(UserAgent()).c_str(), WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
                                  WINHTTP_NO_PROXY_BYPASS, 0);
        }
        if (session == nullptr) {
            out->error = "WinHttpOpen failed";
            return false;
        }
        WinHttpSetTimeouts(session, 15000, 15000, 30000, 60000);
        bool ok = false;
        HINTERNET connect = WinHttpConnect(session, std::wstring(host, parts.dwHostNameLength).c_str(), parts.nPort, 0);
        HINTERNET request = nullptr;
        if (connect != nullptr) {
            const std::wstring object = std::wstring(path, parts.dwUrlPathLength) + std::wstring(extra, parts.dwExtraInfoLength);
            request = WinHttpOpenRequest(connect, L"GET", object.c_str(), nullptr, WINHTTP_NO_REFERER,
                                         WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
        }
        if (request != nullptr) {
            std::wstring hdrs;
            for (const auto& h : headers) hdrs += Wide(h.first) + L": " + Wide(h.second) + L"\r\n";
            if (!hdrs.empty()) {
                WinHttpAddRequestHeaders(request, hdrs.c_str(), static_cast<DWORD>(-1L), WINHTTP_ADDREQ_FLAG_ADD);
            }
            if (WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0)
                && WinHttpReceiveResponse(request, nullptr)) {
                DWORD status = 0, size = sizeof(status);
                WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                                    &status, &size, WINHTTP_NO_HEADER_INDEX);
                out->status = static_cast<int>(status);
                wchar_t etag[512];
                DWORD etag_size = sizeof(etag);
                if (WinHttpQueryHeaders(request, WINHTTP_QUERY_ETAG, WINHTTP_HEADER_NAME_BY_INDEX, etag, &etag_size,
                                        WINHTTP_NO_HEADER_INDEX)) {
                    const std::wstring w(etag, etag_size / sizeof(wchar_t));
                    out->etag = std::string(w.begin(), w.end());
                }
                uint64_t total = 0;
                DWORD length = 0;
                size = sizeof(length);
                if (WinHttpQueryHeaders(request, WINHTTP_QUERY_CONTENT_LENGTH | WINHTTP_QUERY_FLAG_NUMBER,
                                        WINHTTP_HEADER_NAME_BY_INDEX, &length, &size, WINHTTP_NO_HEADER_INDEX)) {
                    total = length;
                }
                ok = true;
                for (;;) {
                    DWORD avail = 0;
                    if (!WinHttpQueryDataAvailable(request, &avail)) {
                        ok = false;
                        out->error = "connection lost";
                        break;
                    }
                    if (avail == 0) break;
                    std::string chunk(avail, '\0');
                    DWORD got = 0;
                    if (!WinHttpReadData(request, &chunk[0], avail, &got)) {
                        ok = false;
                        out->error = "connection lost";
                        break;
                    }
                    out->body.append(chunk, 0, got);
                    if (progress && !progress(out->body.size(), total)) {
                        ok = false;
                        out->error = "cancelled";
                        break;
                    }
                }
            } else {
                out->error = "request failed (error " + std::to_string(GetLastError()) + ") - offline?";
            }
        } else if (out->error.empty()) {
            out->error = "cannot connect";
        }
        if (request != nullptr) WinHttpCloseHandle(request);
        if (connect != nullptr) WinHttpCloseHandle(connect);
        WinHttpCloseHandle(session);
        return ok;
    }
};

}  // namespace

std::unique_ptr<Http> MakeHttp(std::string*) { return std::unique_ptr<Http>(new WinHttp()); }

#else  // Linux: libcurl through dlopen

namespace {

// The few libcurl entry points and option numbers used here; these are part of libcurl's stable
// ABI (curl/curl.h: CURLOPTTYPE_* + n).
typedef void CURL;
struct curl_slist;
enum : int {
    kOptWriteData = 10001,
    kOptUrl = 10002,
    kOptTimeout = 13,
    kOptLowSpeedLimit = 19,
    kOptLowSpeedTime = 20,
    kOptUserAgent = 10018,
    kOptHttpHeader = 10023,
    kOptHeaderData = 10029,
    kOptNoProgress = 43,
    kOptFollowLocation = 52,
    kOptXferInfoData = 10057,
    kOptMaxRedirs = 68,
    kOptConnectTimeout = 78,
    kOptNoSignal = 99,
    kOptWriteFunction = 20011,
    kOptHeaderFunction = 20079,
    kOptXferInfoFunction = 20219,
    kInfoResponseCode = 0x200000 + 2,
};

struct Curl {
    void* lib = nullptr;
    CURL* (*easy_init)() = nullptr;
    int (*easy_setopt)(CURL*, int, ...) = nullptr;
    int (*easy_perform)(CURL*) = nullptr;
    int (*easy_getinfo)(CURL*, int, ...) = nullptr;
    void (*easy_cleanup)(CURL*) = nullptr;
    const char* (*easy_strerror)(int) = nullptr;
    curl_slist* (*slist_append)(curl_slist*, const char*) = nullptr;
    void (*slist_free_all)(curl_slist*) = nullptr;

    bool Load(std::string* error) {
        for (const char* name : {"libcurl.so.4", "libcurl-gnutls.so.4", "libcurl.so"}) {
            lib = dlopen(name, RTLD_NOW | RTLD_LOCAL);
            if (lib != nullptr) break;
        }
        if (lib == nullptr) {
            if (error != nullptr) *error = "libcurl is not installed, so updates cannot be checked";
            return false;
        }
        easy_init = reinterpret_cast<CURL* (*)()>(dlsym(lib, "curl_easy_init"));
        easy_setopt = reinterpret_cast<int (*)(CURL*, int, ...)>(dlsym(lib, "curl_easy_setopt"));
        easy_perform = reinterpret_cast<int (*)(CURL*)>(dlsym(lib, "curl_easy_perform"));
        easy_getinfo = reinterpret_cast<int (*)(CURL*, int, ...)>(dlsym(lib, "curl_easy_getinfo"));
        easy_cleanup = reinterpret_cast<void (*)(CURL*)>(dlsym(lib, "curl_easy_cleanup"));
        easy_strerror = reinterpret_cast<const char* (*)(int)>(dlsym(lib, "curl_easy_strerror"));
        slist_append = reinterpret_cast<curl_slist* (*)(curl_slist*, const char*)>(dlsym(lib, "curl_slist_append"));
        slist_free_all = reinterpret_cast<void (*)(curl_slist*)>(dlsym(lib, "curl_slist_free_all"));
        if (!easy_init || !easy_setopt || !easy_perform || !easy_getinfo || !easy_cleanup || !easy_strerror
            || !slist_append || !slist_free_all) {
            if (error != nullptr) *error = "the system's libcurl lacks functions the update check needs";
            return false;
        }
        return true;
    }
};

struct Transfer {
    HttpResponse* out;
    const ProgressFn* progress;
};

size_t OnWrite(char* data, size_t size, size_t n, void* user) {
    static_cast<Transfer*>(user)->out->body.append(data, size * n);
    return size * n;
}

size_t OnHeader(char* data, size_t size, size_t n, void* user) {
    const std::string line(data, size * n);
    if (Lower(line.substr(0, 5)) == "etag:") static_cast<Transfer*>(user)->out->etag = Trim(line.substr(5));
    return size * n;
}

int OnProgress(void* user, int64_t total, int64_t now, int64_t, int64_t) {
    const Transfer* t = static_cast<Transfer*>(user);
    if (t->progress != nullptr && *t->progress) {
        return (*t->progress)(static_cast<uint64_t>(now), static_cast<uint64_t>(total)) ? 0 : 1;
    }
    return 0;
}

class CurlHttp : public Http {
public:
    explicit CurlHttp(Curl c) : c_(c) {}

    bool Get(const std::string& url, const std::vector<std::pair<std::string, std::string>>& headers, HttpResponse* out,
             const ProgressFn& progress) override {
        *out = HttpResponse();
        if (!StartsWith(url, "https://")) {
            out->error = "only https is used: " + url;
            return false;
        }
        CURL* h = c_.easy_init();
        if (h == nullptr) {
            out->error = "curl_easy_init failed";
            return false;
        }
        Transfer t{out, &progress};
        curl_slist* list = nullptr;
        for (const auto& kv : headers) list = c_.slist_append(list, (kv.first + ": " + kv.second).c_str());
        const std::string agent = UserAgent();
        c_.easy_setopt(h, kOptUrl, url.c_str());
        c_.easy_setopt(h, kOptUserAgent, agent.c_str());
        c_.easy_setopt(h, kOptHttpHeader, list);
        c_.easy_setopt(h, kOptFollowLocation, 1L);
        c_.easy_setopt(h, kOptMaxRedirs, 5L);
        c_.easy_setopt(h, kOptNoSignal, 1L);
        c_.easy_setopt(h, kOptConnectTimeout, 15L);
        c_.easy_setopt(h, kOptLowSpeedLimit, 1L);   // abort when stalled for a minute
        c_.easy_setopt(h, kOptLowSpeedTime, 60L);
        c_.easy_setopt(h, kOptTimeout, 0L);
        c_.easy_setopt(h, kOptWriteFunction, &OnWrite);
        c_.easy_setopt(h, kOptWriteData, &t);
        c_.easy_setopt(h, kOptHeaderFunction, &OnHeader);
        c_.easy_setopt(h, kOptHeaderData, &t);
        c_.easy_setopt(h, kOptXferInfoFunction, &OnProgress);
        c_.easy_setopt(h, kOptXferInfoData, &t);
        c_.easy_setopt(h, kOptNoProgress, 0L);
        const int rc = c_.easy_perform(h);
        long status = 0;
        c_.easy_getinfo(h, kInfoResponseCode, &status);
        out->status = static_cast<int>(status);
        if (rc != 0) out->error = std::string(c_.easy_strerror(rc)) + " - offline?";
        c_.easy_cleanup(h);
        if (list != nullptr) c_.slist_free_all(list);
        return rc == 0;
    }

private:
    Curl c_;
};

}  // namespace

std::unique_ptr<Http> MakeHttp(std::string* error) {
    struct Loaded {
        Curl curl;
        std::string why;
        Loaded() {
            if (!curl.Load(&why)) curl.lib = nullptr;
        }
    };
    static const Loaded loaded;   // once, thread-safe
    if (loaded.curl.lib == nullptr) {
        if (error != nullptr) *error = loaded.why;
        return nullptr;
    }
    return std::unique_ptr<Http>(new CurlHttp(loaded.curl));
}

#endif

}  // namespace atmt
