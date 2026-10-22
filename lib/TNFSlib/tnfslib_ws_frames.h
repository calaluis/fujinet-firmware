#ifndef _TNFSLIB_WS_FRAMES_H
#define _TNFSLIB_WS_FRAMES_H

// Turns each read of ESP-IDF's WebSocket transport (bytes, plus the frame's opcode, length
// and FIN) into one action for the TNFS byte stream, and resyncs after a timed-out read.

#include <cstdint>

enum class tnfsWsAction
{
    DATA,         // `n` payload bytes of a binary message belong to the TNFS stream
    IDLE,         // nothing for the stream: a timeout, an empty frame or a PONG
    CONTROL_PART, // `n` bytes of a control frame payload that continues in a later read
    PONG,         // a PING is complete (this read holds its last bytes); answer it
    CLOSE,        // the server started the closing handshake
    LOST,         // error or protocol violation; the connection must be dropped
};

class tnfsWsFrameTracker
{
public:
    // RFC 6455 opcodes, plus NO_FRAME when the read saw no frame header.
    static constexpr int OP_CONTINUATION = 0x0;
    static constexpr int OP_TEXT = 0x1;
    static constexpr int OP_BINARY = 0x2;
    static constexpr int OP_CLOSE = 0x8;
    static constexpr int OP_PING = 0x9;
    static constexpr int OP_PONG = 0xA;
    static constexpr int NO_FRAME = -1;
    static constexpr int MAX_CONTROL_PAYLOAD = 125;

    // `n`: the read's result (<0 error, 0 nothing, >0 bytes). `opcode`, `payload_len` and
    // `fin` describe the frame the transport is in after the read.
    tnfsWsAction on_read(int n, int opcode, int payload_len, bool fin);

    // A new connection starts outside any frame or message.
    void reset()
    {
        _frame_left = 0;
        _frame_opcode = NO_FRAME;
        _message_open = false;
    }

    int frame_left() const { return _frame_left; }
    bool message_open() const { return _message_open; }

private:
    tnfsWsAction lost()
    {
        reset();
        return tnfsWsAction::LOST;
    }
    tnfsWsAction first_fragment(int n, int opcode, int payload_len, bool fin);
    tnfsWsAction next_fragment(int n, int opcode, int payload_len, bool fin);
    tnfsWsAction data_frame(int n, int payload_len, bool fin);
    tnfsWsAction control_frame(int n, int opcode, int payload_len, bool fin);
    tnfsWsAction control_done(int opcode);

    int _frame_left = 0;          // payload bytes of the current frame not yet read
    int _frame_opcode = NO_FRAME; // opcode of that frame
    bool _message_open = false;   // a binary message continues in a later frame
};

#endif // _TNFSLIB_WS_FRAMES_H
