#include "tnfslib_ws_frames.h"

namespace
{
using T = tnfsWsFrameTracker;

// How a frame starts, by opcode. Text and reserved opcodes have no row: TNFS is binary, so
// such a frame means the peer is not a TNFS bridge.
struct tnfsWsFrameStart
{
    int opcode;
    tnfsWsAction (T::*start)(int n, int opcode, int payload_len, bool fin);
};

// What a complete control frame asks for; `reset` ends the connection's frame state.
struct tnfsWsControlEnd
{
    int opcode;
    tnfsWsAction action;
    bool reset;
};

constexpr tnfsWsControlEnd CONTROL_ENDS[] = {
    {T::OP_PING, tnfsWsAction::PONG, false},
    {T::OP_CLOSE, tnfsWsAction::CLOSE, true},
    {T::OP_PONG, tnfsWsAction::IDLE, false}, // unsolicited (we never ping); RFC 6455 allows it
};
} // namespace

tnfsWsAction tnfsWsFrameTracker::on_read(int n, int opcode, int payload_len, bool fin)
{
    static const tnfsWsFrameStart FRAME_STARTS[] = {
        {OP_BINARY, &T::first_fragment},     {OP_CONTINUATION, &T::next_fragment},
        {OP_CLOSE, &T::control_frame},       {OP_PING, &T::control_frame},
        {OP_PONG, &T::control_frame},
    };

    if (n < 0)
        return lost();

    if (_frame_left > 0)
    {
        // Still inside a frame: the read either continues its payload, or the transport
        // timed out mid-payload and has already dropped the frame's state.
        if (n == 0 || n > _frame_left)
            return lost();
        _frame_left -= n;
        if (_frame_opcode == OP_BINARY)
            return tnfsWsAction::DATA;
        return _frame_left > 0 ? tnfsWsAction::CONTROL_PART : control_done(_frame_opcode);
    }

    if (opcode == NO_FRAME)
        return n == 0 ? tnfsWsAction::IDLE : lost();
    for (const auto &row : FRAME_STARTS)
    {
        if (row.opcode == opcode)
            return (this->*row.start)(n, opcode, payload_len, fin);
    }
    return lost();
}

tnfsWsAction tnfsWsFrameTracker::first_fragment(int n, int, int payload_len, bool fin)
{
    if (_message_open) // a new message before the previous one finished
        return lost();
    return data_frame(n, payload_len, fin);
}

tnfsWsAction tnfsWsFrameTracker::next_fragment(int n, int, int payload_len, bool fin)
{
    if (!_message_open)
        return lost();
    return data_frame(n, payload_len, fin);
}

tnfsWsAction tnfsWsFrameTracker::data_frame(int n, int payload_len, bool fin)
{
    if (payload_len < 0 || n > payload_len)
        return lost();
    if (payload_len > 0 && n == 0) // the payload read timed out: the frame is gone
        return lost();
    _frame_left = payload_len - n;
    _frame_opcode = OP_BINARY;
    _message_open = !fin;
    return n > 0 ? tnfsWsAction::DATA : tnfsWsAction::IDLE;
}

tnfsWsAction tnfsWsFrameTracker::control_frame(int n, int opcode, int payload_len, bool fin)
{
    // Control frames are never fragmented and carry at most 125 bytes (RFC 6455 section
    // 5.5); the payload may still arrive over several reads, like any TCP data.
    if (!fin || payload_len < 0 || payload_len > MAX_CONTROL_PAYLOAD || n > payload_len)
        return lost();
    if (payload_len > 0 && n == 0) // the payload read timed out: the frame is gone
        return lost();
    _frame_left = payload_len - n;
    _frame_opcode = opcode;
    return _frame_left > 0 ? tnfsWsAction::CONTROL_PART : control_done(opcode);
}

tnfsWsAction tnfsWsFrameTracker::control_done(int opcode)
{
    for (const auto &row : CONTROL_ENDS)
    {
        if (row.opcode != opcode)
            continue;
        if (row.reset)
            reset();
        return row.action;
    }
    return tnfsWsAction::IDLE;
}
