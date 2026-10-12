#include "tnfslib_ws_frames.h"

tnfsWsAction tnfsWsFrameTracker::on_read(int n, int opcode, int payload_len, bool fin)
{
    if (n < 0)
        return lost();

    if (_frame_left > 0)
    {
        // Still inside a frame: the read either continues its payload, or the transport
        // timed out mid-payload and has already dropped the frame's state.
        if (n == 0 || n > _frame_left)
            return lost();
        _frame_left -= n;
        if (_frame_opcode == OP_BINARY || _frame_opcode == OP_CONTINUATION)
            return tnfsWsAction::DATA;
        return _frame_left > 0 ? tnfsWsAction::CONTROL_PART : control_done(_frame_opcode);
    }

    switch (opcode)
    {
    case NO_FRAME:
        return n == 0 ? tnfsWsAction::IDLE : lost();
    case OP_BINARY:
        if (_message_open) // a new message before the previous one finished
            return lost();
        return data_frame(n, payload_len, fin);
    case OP_CONTINUATION:
        if (!_message_open)
            return lost();
        return data_frame(n, payload_len, fin);
    case OP_CLOSE:
    case OP_PING:
    case OP_PONG:
        return control_frame(n, opcode, payload_len, fin);
    case OP_TEXT: // TNFS is binary; text means the peer is not a TNFS bridge
    default:      // reserved opcodes
        return lost();
    }
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
    switch (opcode)
    {
    case OP_PING:
        return tnfsWsAction::PONG;
    case OP_CLOSE:
        reset();
        return tnfsWsAction::CLOSE;
    case OP_PONG: // unsolicited (we never ping); ignored as RFC 6455 allows
    default:
        return tnfsWsAction::IDLE;
    }
}
