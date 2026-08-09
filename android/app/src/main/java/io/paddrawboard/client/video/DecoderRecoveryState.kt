package io.paddrawboard.client.video

/** Small platform-independent state machine used by the asynchronous decoder. */
internal enum class DecoderPhase { IDLE, RUNNING, AWAITING_IDR, CLOSED }

internal class DecoderRecoveryState {
    var phase: DecoderPhase = DecoderPhase.IDLE
        private set

    fun configured(awaitingIdr: Boolean) {
        if (phase != DecoderPhase.CLOSED) phase = if (awaitingIdr) DecoderPhase.AWAITING_IDR else DecoderPhase.RUNNING
    }

    fun acceptFrame(keyFrame: Boolean): Boolean {
        if (phase == DecoderPhase.CLOSED || phase == DecoderPhase.IDLE) return false
        if (phase == DecoderPhase.AWAITING_IDR && !keyFrame) return false
        return true
    }

    /**
     * A queued IDR is the recovery barrier. Until it reaches MediaCodec, an inter frame
     * must not replace it, otherwise recovery can start with a P frame and remain black.
     */
    fun canReplacePending(pendingKeyFrame: Boolean, incomingKeyFrame: Boolean): Boolean =
        !pendingKeyFrame || incomingKeyFrame

    /** Advances recovery only after MediaCodec accepted the input buffer. */
    fun onFrameQueued(keyFrame: Boolean): Boolean {
        if (phase == DecoderPhase.CLOSED || phase == DecoderPhase.IDLE) return false
        if (keyFrame) phase = DecoderPhase.RUNNING
        return true
    }

    fun codecError(): Boolean {
        if (phase == DecoderPhase.CLOSED) return false
        phase = DecoderPhase.AWAITING_IDR
        return true
    }

    fun close() { phase = DecoderPhase.CLOSED }
}
