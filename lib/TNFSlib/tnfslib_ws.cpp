#include "tnfslib_ws.h"

#include <new>

#ifdef ESP_PLATFORM

#include <cstring>

#include "esp_crt_bundle.h"
#include "esp_transport.h"
#include "esp_transport_ssl.h"
#include "esp_transport_ws.h"
#include "sdkconfig.h"

#include "../../include/debug.h"
#include "tnfslib_ws_frames.h"

namespace
{

constexpr int WS_UPGRADE_OK = 101;
constexpr int WS_CLOSE_TIMEOUT_MS = 200; // best effort; never delays a teardown for long
// esp_transport_ws spends a write's timeout up to three times (writability, frame header,
// payload), so each step gets a third of the caller's budget.
constexpr int WS_WRITE_STEPS = 3;
const char WS_PATH[] = "/";
const char WS_USER_AGENT[] = "FujiNet";

class tnfsWsTransport : public tnfsStreamTransport
{
public:
    ~tnfsWsTransport() override { close(); }

    success_is_true connect(const char *host, uint16_t port, int timeout_ms) override;
    bool is_connected() const override { return _connected; }
    int write(const uint8_t *buf, int len, int timeout_ms) override;
    int read(uint8_t *buf, int max, int timeout_ms) override;
    void close() override;

private:
    bool create_handles();
    void destroy_handles();
    int drop(); // abort without a closing handshake and report -1
    int control(tnfsWsAction action, const uint8_t *buf, int n, int timeout_ms);

    esp_transport_handle_t _ssl = nullptr;
    esp_transport_handle_t _ws = nullptr;
    bool _connected = false;
    tnfsWsFrameTracker _frames;
    // Payload of the control frame being read; a PING is answered with all of it.
    uint8_t _ctrl[tnfsWsFrameTracker::MAX_CONTROL_PAYLOAD];
    int _ctrl_len = 0;
    // esp_transport_ws masks the payload in place, so requests are sent from this copy.
    uint8_t _tx[tnfsStreamLink::MAX_REQUEST_SIZE];
};

bool tnfsWsTransport::create_handles()
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
    // Control frames come back to read(), so PING, PONG and CLOSE are handled here
    // deterministically instead of looking like read timeouts.
    cfg.propagate_control_frames = true;
    return esp_transport_ws_set_config(_ws, &cfg) == ESP_OK;
}

void tnfsWsTransport::destroy_handles()
{
    // The WS handle borrows the SSL handle; destroying SSL also closes the socket.
    if (_ws != nullptr)
        esp_transport_destroy(_ws);
    if (_ssl != nullptr)
        esp_transport_destroy(_ssl);
    _ws = nullptr;
    _ssl = nullptr;
}

success_is_true tnfsWsTransport::connect(const char *host, uint16_t port, int timeout_ms)
{
    close();
    if (host == nullptr || host[0] == '\0' || !create_handles())
    {
        Debug_println("TNFS WSS: cannot create transport");
        close();
        RETURN_ERROR_AS_FALSE();
    }

    // TLS handshake, then the HTTP upgrade. Redirects are not followed: anything but
    // 101 Switching Protocols (with a valid Sec-WebSocket-Accept, checked by ESP-IDF)
    // fails the connection.
    if (esp_transport_connect(_ws, host, port, timeout_ms) < 0)
    {
        Debug_printf("TNFS WSS: connect to %s:%u failed\r\n", host, (unsigned)port);
        close();
        RETURN_ERROR_AS_FALSE();
    }
    int status = esp_transport_ws_get_upgrade_request_status(_ws);
    if (status != WS_UPGRADE_OK)
    {
        Debug_printf("TNFS WSS: upgrade refused, HTTP %d\r\n", status);
        close();
        RETURN_ERROR_AS_FALSE();
    }

    _frames.reset();
    _ctrl_len = 0;
    _connected = true;
    RETURN_SUCCESS_AS_TRUE();
}

int tnfsWsTransport::drop()
{
    // After an error the stream may sit mid-frame, where a CLOSE frame would be garbage.
    _connected = false;
    close();
    return -1;
}

int tnfsWsTransport::write(const uint8_t *buf, int len, int timeout_ms)
{
    if (!_connected || buf == nullptr || len <= 0 || len > (int)sizeof(_tx))
        return drop(); // len 0 would make esp_transport_ws send a PING

    // One request, one binary frame. The frame header goes out before the payload, so a
    // short or timed-out payload write leaves a broken frame: all or nothing.
    memcpy(_tx, buf, len);
    int step_ms = timeout_ms / WS_WRITE_STEPS > 0 ? timeout_ms / WS_WRITE_STEPS : 1;
    int n = esp_transport_write(_ws, reinterpret_cast<const char *>(_tx), len, step_ms);
    if (n != len)
    {
        Debug_printf("TNFS WSS: write failed (%d of %d)\r\n", n, len);
        return drop();
    }
    return len;
}

int tnfsWsTransport::read(uint8_t *buf, int max, int timeout_ms)
{
    if (!_connected || buf == nullptr || max <= 0)
        return drop();

    int n = esp_transport_read(_ws, reinterpret_cast<char *>(buf), max, timeout_ms);
    ws_transport_opcodes_t op = esp_transport_ws_get_read_opcode(_ws);
    int opcode = op == WS_TRANSPORT_OPCODES_NONE ? tnfsWsFrameTracker::NO_FRAME : (int)op;

    tnfsWsAction action =
        _frames.on_read(n, opcode, esp_transport_ws_get_read_payload_len(_ws),
                        esp_transport_ws_get_fin_flag(_ws));
    switch (action)
    {
    case tnfsWsAction::DATA:
        return n;
    case tnfsWsAction::IDLE:
        _ctrl_len = 0;
        return 0;
    case tnfsWsAction::LOST:
        Debug_printf("TNFS WSS: connection lost (read %d, opcode %d)\r\n", n, opcode);
        return drop();
    default:
        return control(action, buf, n, timeout_ms);
    }
}

int tnfsWsTransport::control(tnfsWsAction action, const uint8_t *buf, int n, int timeout_ms)
{
    // The tracker bounds a control payload to 125 bytes, the size of _ctrl.
    if (n > (int)sizeof(_ctrl) - _ctrl_len)
        return drop();
    memcpy(_ctrl + _ctrl_len, buf, n);
    _ctrl_len += n;

    switch (action)
    {
    case tnfsWsAction::CONTROL_PART:
        return 0;
    case tnfsWsAction::PONG: {
        int len = _ctrl_len;
        _ctrl_len = 0;
        // Masks _ctrl in place, which is fine: it is consumed.
        int sent = esp_transport_ws_send_raw(
            _ws,
            (ws_transport_opcodes_t)(WS_TRANSPORT_OPCODES_PONG | WS_TRANSPORT_OPCODES_FIN),
            reinterpret_cast<const char *>(_ctrl), len, timeout_ms);
        return sent == len ? 0 : drop();
    }
    case tnfsWsAction::CLOSE:
    default:
        Debug_println("TNFS WSS: server closed the connection");
        close(); // still connected, so this answers with our own CLOSE frame
        return -1;
    }
}

void tnfsWsTransport::close()
{
    if (_connected)
    {
        // Best-effort closing handshake: status 1000 (normal closure).
        static const char normal_closure[2] = {0x03, (char)0xE8};
        char payload[sizeof(normal_closure)];
        memcpy(payload, normal_closure, sizeof(payload)); // masked in place by ESP-IDF
        esp_transport_ws_send_raw(
            _ws,
            (ws_transport_opcodes_t)(WS_TRANSPORT_OPCODES_CLOSE | WS_TRANSPORT_OPCODES_FIN),
            payload, sizeof(payload), WS_CLOSE_TIMEOUT_MS);
        esp_transport_close(_ws);
    }
    _connected = false;
    _frames.reset();
    _ctrl_len = 0;
    destroy_handles();
}

} // namespace

std::unique_ptr<tnfsStreamTransport> tnfs_make_ws_transport()
{
#ifdef CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
    return std::unique_ptr<tnfsStreamTransport>(new (std::nothrow) tnfsWsTransport());
#else
    Debug_println("TNFS WSS: unavailable without CONFIG_MBEDTLS_CERTIFICATE_BUNDLE");
    return nullptr;
#endif
}

#else // !ESP_PLATFORM

std::unique_ptr<tnfsStreamTransport> tnfs_make_ws_transport() { return nullptr; }

#endif // ESP_PLATFORM
