#include "tnfslib_wss.h"

#ifdef ESP_PLATFORM

#include <ctime>
#include <new>
#include <utility>

#include "esp_crt_bundle.h"
#include "esp_transport.h"
#include "esp_transport_ssl.h"
#include "esp_transport_ws.h"
#include "mbedtls/ssl.h"
#include "mbedtls/x509_crt.h"
#include "sdkconfig.h"

#include "compat_string.h"
#include "fnSystem.h"
#include "tnfslib_cert_time.h"
#include "tnfslib_ws_stream.h"
#include "tnfslib_wss_url.h"

#include "../../include/debug.h"

// ESP-IDF's certificate bundle check (esp_crt_bundle.c): exported, and installed by
// esp_crt_bundle_attach(), but not declared in esp_crt_bundle.h.
extern "C" int esp_crt_verify_callback(void *buf, mbedtls_x509_crt *crt, int depth,
                                       uint32_t *flags);

namespace
{
constexpr int WS_UPGRADE_OK = 101;
const char WS_USER_AGENT[] = "FujiNet";

struct tnfsCertRejection
{
    tnfsCertValidity validity;
    uint32_t flag;
    const char *reason;
};

constexpr tnfsCertRejection REJECTIONS[] = {
    {tnfsCertValidity::EXPIRED, MBEDTLS_X509_BADCERT_EXPIRED, "expired"},
    {tnfsCertValidity::NOT_YET_VALID, MBEDTLS_X509_BADCERT_FUTURE, "not yet valid"},
    {tnfsCertValidity::NO_CLOCK, MBEDTLS_X509_BADCERT_OTHER, "no SNTP time to check it against"},
};

tnfsCertTime cert_time(const mbedtls_x509_time &t)
{ return {t.year, t.mon, t.day, t.hour, t.min, t.sec}; }

// Called by mbedTLS for every certificate in the server's chain: the bundle's chain check,
// then the validity dates that mbedTLS skips without MBEDTLS_HAVE_TIME_DATE. A flag left set
// fails the handshake (the authmode is MBEDTLS_SSL_VERIFY_REQUIRED with a bundle).
int verify_with_dates(void *, mbedtls_x509_crt *crt, int depth, uint32_t *flags)
{
    int ret = esp_crt_verify_callback(nullptr, crt, depth, flags);
    if (ret != 0)
        return ret;
    tnfsCertValidity validity =
        tnfs_cert_validity(cert_time(crt->valid_from), cert_time(crt->valid_to),
                           fnSystem.Net.get_sntp_lastsync() != 0, time(nullptr));
    for (const auto &row : REJECTIONS)
    {
        if (row.validity == validity)
        {
            Debug_printf("TNFS WSS: certificate at depth %d rejected: %s\r\n", depth, row.reason);
            *flags |= row.flag;
        }
    }
    return 0;
}

esp_err_t attach_bundle_with_dates(void *conf)
{
    esp_err_t err = esp_crt_bundle_attach(conf);
    if (err != ESP_OK || conf == nullptr)
        return err;
    mbedtls_ssl_conf_verify(static_cast<mbedtls_ssl_config *>(conf), verify_with_dates, nullptr);
    return ESP_OK;
}

// tnfsWsIo on ESP-IDF 5.4 esp_transport_ws over esp_transport_ssl.
class tnfsEspWsIo : public tnfsWsIo
{
public:
    explicit tnfsEspWsIo(const char *path) { strlcpy(_path, path, sizeof(_path)); }
    ~tnfsEspWsIo() override { shutdown(); }

    success_is_true open(const char *host, uint16_t port, int timeout_ms) override;
    int read(uint8_t *buf, int max, int timeout_ms) override;
    int opcode() const override;
    int payload_len() const override { return esp_transport_ws_get_read_payload_len(_ws); }
    bool fin() const override { return esp_transport_ws_get_fin_flag(_ws); }
    int send(int opcode, uint8_t *payload, int len, int timeout_ms) override;
    void shutdown() override;

private:
    success_is_true create_handles();

    char _path[TNFS_WSS_PATH_SIZE];
    esp_transport_handle_t _ssl = nullptr;
    esp_transport_handle_t _ws = nullptr;
};

success_is_true tnfsEspWsIo::create_handles()
{
    // Fresh handles for every connection: the WS layer keeps frame state and buffered
    // handshake bytes that must never leak into the next connection.
    _ssl = esp_transport_ssl_init();
    if (_ssl == nullptr)
        RETURN_ERROR_AS_FALSE();
    // Verification is mandatory: chain against the bundle, name against the host (esp-tls
    // defaults common_name to the host; skip_common_name is never set), dates as above.
    esp_transport_ssl_crt_bundle_attach(_ssl, attach_bundle_with_dates);

    _ws = esp_transport_ws_init(_ssl);
    if (_ws == nullptr)
        RETURN_ERROR_AS_FALSE();

    esp_transport_ws_config_t cfg = {};
    cfg.ws_path = _path;
    cfg.user_agent = WS_USER_AGENT;
    // Control frames come back to read(), so PING, PONG and CLOSE are handled by
    // tnfsWsStream deterministically instead of looking like read timeouts.
    cfg.propagate_control_frames = true;
    RETURN_SUCCESS_IF(esp_transport_ws_set_config(_ws, &cfg) == ESP_OK);
}

success_is_true tnfsEspWsIo::open(const char *host, uint16_t port, int timeout_ms)
{
    shutdown();
    if (create_handles().is_error())
        RETURN_ERROR_AS_FALSE();
    // TLS handshake (verified), then the HTTP upgrade. esp_transport_ws never follows
    // redirects; it reports the HTTP status, which must be 101.
    if (esp_transport_connect(_ws, host, port, timeout_ms) < 0)
        RETURN_ERROR_AS_FALSE();
    int status = esp_transport_ws_get_upgrade_request_status(_ws);
    if (status != WS_UPGRADE_OK)
    {
        Debug_printf("TNFS WSS: upgrade refused, HTTP %d\r\n", status);
        RETURN_ERROR_AS_FALSE();
    }
    RETURN_SUCCESS_AS_TRUE();
}

int tnfsEspWsIo::read(uint8_t *buf, int max, int timeout_ms)
{ return esp_transport_read(_ws, reinterpret_cast<char *>(buf), max, timeout_ms); }

int tnfsEspWsIo::opcode() const
{
    ws_transport_opcodes_t op = esp_transport_ws_get_read_opcode(_ws);
    return op == WS_TRANSPORT_OPCODES_NONE ? tnfsWsFrameTracker::NO_FRAME : (int)op;
}

int tnfsEspWsIo::send(int opcode, uint8_t *payload, int len, int timeout_ms)
{
    // esp_transport_ws_send_raw masks the payload in place and writes header + payload.
    return esp_transport_ws_send_raw(
        _ws, (ws_transport_opcodes_t)(opcode | WS_TRANSPORT_OPCODES_FIN),
        reinterpret_cast<const char *>(payload), len, timeout_ms);
}

void tnfsEspWsIo::shutdown()
{
    // The WS handle borrows the SSL handle; destroying SSL also closes the socket.
    if (_ws != nullptr)
        esp_transport_destroy(_ws);
    if (_ssl != nullptr)
        esp_transport_destroy(_ssl);
    _ws = nullptr;
    _ssl = nullptr;
}
} // namespace

tnfsStreamTransportPtr tnfs_new_wss_stream(const char *path)
{
#ifdef CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
    std::unique_ptr<tnfsWsIo> io(new (std::nothrow) tnfsEspWsIo(path));
    if (!io)
        return nullptr;
    void *memory = tnfs_stream_transport_memory(sizeof(tnfsWsStream));
    if (memory == nullptr)
        return nullptr;
    return tnfsStreamTransportPtr(new (memory) tnfsWsStream(std::move(io)));
#else
    Debug_println("TNFS WSS: unavailable without CONFIG_MBEDTLS_CERTIFICATE_BUNDLE");
    return nullptr;
#endif
}

#else // !ESP_PLATFORM

tnfsStreamTransportPtr tnfs_new_wss_stream(const char *) { return nullptr; }

#endif // ESP_PLATFORM
