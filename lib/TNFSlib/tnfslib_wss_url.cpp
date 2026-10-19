#include "tnfslib_wss_url.h"

#include <cstring>
#include <strings.h>

#include "global_types.h"

namespace
{
const char SCHEME[] = "tnfsw://";
constexpr size_t SCHEME_LEN = sizeof(SCHEME) - 1;

// Characters that end the host, or are refused anywhere in the URL.
bool is_host_end(char c) { return c == '\0' || c == ':' || c == '/'; }
bool is_refused(char c) { return c == '@' || c == '[' || c == ']' || c == '?' || c == '#'; }

success_is_true parse_port(const char *p, const char *end, uint16_t &port)
{
    if (p == end)
        RETURN_ERROR_AS_FALSE();
    unsigned long value = 0;
    for (; p < end; p++)
    {
        if (*p < '0' || *p > '9')
            RETURN_ERROR_AS_FALSE();
        value = value * 10 + (*p - '0');
        if (value > 65535)
            RETURN_ERROR_AS_FALSE();
    }
    if (value == 0)
        RETURN_ERROR_AS_FALSE();
    port = static_cast<uint16_t>(value);
    RETURN_SUCCESS_AS_TRUE();
}
} // namespace

tnfsWssUrlResult tnfs_parse_wss_url(const char *text, tnfsWssUrl &out)
{
    if (text == nullptr || strncasecmp(text, SCHEME, SCHEME_LEN) != 0)
        return tnfsWssUrlResult::NOT_WSS;

    const char *host = text + SCHEME_LEN;
    for (const char *p = host; *p != '\0'; p++)
    {
        if (is_refused(*p))
            return tnfsWssUrlResult::INVALID;
    }

    const char *host_end = host;
    while (!is_host_end(*host_end))
        host_end++;
    size_t host_len = host_end - host;
    if (host_len == 0 || host_len >= sizeof(out.host))
        return tnfsWssUrlResult::INVALID;

    uint16_t port = TNFS_WSS_DEFAULT_PORT;
    const char *path = host_end;
    if (*host_end == ':')
    {
        path = strchr(host_end, '/');
        if (path == nullptr)
            path = host_end + strlen(host_end);
        if (parse_port(host_end + 1, path, port).is_error())
            return tnfsWssUrlResult::INVALID;
    }
    if (*path == '\0')
        path = "/";
    size_t path_len = strlen(path);
    if (path_len >= sizeof(out.path))
        return tnfsWssUrlResult::INVALID;

    memcpy(out.host, host, host_len);
    out.host[host_len] = '\0';
    out.port = port;
    memcpy(out.path, path, path_len + 1);
    return tnfsWssUrlResult::VALID;
}
