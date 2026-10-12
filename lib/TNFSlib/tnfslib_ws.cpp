#include "tnfslib_ws.h"

#include <new>
#include <utility>

#include "tnfslib_ws_transport.h"

#ifdef ESP_PLATFORM

#include "esp_crt_bundle.h"
#include "esp_transport.h"
#include "esp_transport_ssl.h"
#include "esp_transport_ws.h"
#include "sdkconfig.h"

#include "../../include/debug.h"

namespace
{

constexpr int WS_UPGRADE_OK = 101;
const char WS_PATH[] = "/";
const char WS_USER_AGENT[] = "FujiNet";

// tnfsWsIo on ESP-IDF 5.4 esp_transport_ws over esp_transport_ssl.
class tnfsEspWsIo : public tnfsWsIo
{
public:
    ~tnfsEspWsIo() override { shutdown(); }

    bool open(const char *host, uint16_t port, int timeout_ms) override;
    int read(uint8_t *buf, int max, int timeout_ms) override;
    int opcode() const override;
    int payload_len() const override { return esp_transport_ws_get_read_payload_len(_ws); }
    bool fin() const override { return esp_transport_ws_get_fin_flag(_ws); }
    int send(int opcode, uint8_t *payload, int len, int timeout_ms) override;
    void shutdown() override;

private:
    bool create_handles();

    esp_transport_handle_t _ssl = nullptr;
    esp_transport_handle_t _ws = nullptr;
};

bool tnfsEspWsIo::create_handles()
{
    // Fresh handles for every connection: the WS layer keeps frame state and buffered
    // handshake bytes that must never leak into the next connection.
    _ssl = esp_transport_ssl_init();
    if (_ssl == nullptr)
        return false;
    // Verification is mandatory: chain against the bundle, name against the host
    // (esp-tls defaults common_name to the host; skip_common_name is never set).
    esp_transport_ssl_crt_bundle_attach(_ssl, esp_crt_bundle_attach);

    _ws = esp_transport_ws_init(_ssl);
    if (_ws == nullptr)
        return false;

    esp_transport_ws_config_t cfg = {};
    cfg.ws_path = WS_PATH;
    cfg.user_agent = WS_USER_AGENT;
    // Control frames come back to read(), so PING, PONG and CLOSE are handled by
    // tnfsWsTransport deterministically instead of looking like read timeouts.
    cfg.propagate_control_frames = true;
    return esp_transport_ws_set_config(_ws, &cfg) == ESP_OK;
}

bool tnfsEspWsIo::open(const char *host, uint16_t port, int timeout_ms)
{
    shutdown();
    if (!create_handles())
        return false;
    // TLS handshake (verified), then the HTTP upgrade. esp_transport_ws never follows
    // redirects; it reports the HTTP status, which must be 101.
    if (esp_transport_connect(_ws, host, port, timeout_ms) < 0)
        return false;
    int status = esp_transport_ws_get_upgrade_request_status(_ws);
    if (status != WS_UPGRADE_OK)
    {
        Debug_printf("TNFS WSS: upgrade refused, HTTP %d\r\n", status);
        return false;
    }
    return true;
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

std::unique_ptr<tnfsStreamTransport> tnfs_make_ws_transport()
{
#ifdef CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
    std::unique_ptr<tnfsWsIo> io(new (std::nothrow) tnfsEspWsIo());
    if (!io)
        return nullptr;
    return std::unique_ptr<tnfsStreamTransport>(new (std::nothrow)
                                                    tnfsWsTransport(std::move(io)));
#else
    Debug_println("TNFS WSS: unavailable without CONFIG_MBEDTLS_CERTIFICATE_BUNDLE");
    return nullptr;
#endif
}

#else // !ESP_PLATFORM

std::unique_ptr<tnfsStreamTransport> tnfs_make_ws_transport() { return nullptr; }

#endif // ESP_PLATFORM
