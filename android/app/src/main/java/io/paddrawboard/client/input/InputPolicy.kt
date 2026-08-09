package io.paddrawboard.client.input

/** Pure time policy, independently testable without Android event objects. */
class PalmRejectionPolicy(private val enabled: () -> Boolean, private val releaseDelayMs: Long = 150) {
    private var stylusActiveUntil = Long.MIN_VALUE
    fun stylusObserved(timeMs: Long) { stylusActiveUntil = Long.MAX_VALUE }
    fun stylusLeftRange(timeMs: Long) { stylusActiveUntil = timeMs + releaseDelayMs }
    fun reset() { stylusActiveUntil = Long.MIN_VALUE }
    fun suppressTouch(timeMs: Long): Boolean = enabled() && timeMs <= stylusActiveUntil
}
