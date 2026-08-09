package io.paddrawboard.client.input

import android.view.KeyEvent
import android.view.MotionEvent
import io.paddrawboard.protocol.PdbProtocol

/** Maps only documented Android stylus controls to the three protocol button bits. */
object StylusButtonMapping {
    fun fromMotionEventButtonState(buttonState: Int, stylusTool: Boolean): Int {
        if (!stylusTool) return 0
        var buttons = 0
        if (buttonState and MotionEvent.BUTTON_STYLUS_PRIMARY != 0 ||
            buttonState and MotionEvent.BUTTON_PRIMARY != 0) buttons = buttons or PdbProtocol.BUTTON_1
        if (buttonState and MotionEvent.BUTTON_STYLUS_SECONDARY != 0 ||
            buttonState and MotionEvent.BUTTON_SECONDARY != 0) buttons = buttons or PdbProtocol.BUTTON_2
        // Android exposes the third generic button for stylus-class MotionEvents.
        if (buttonState and MotionEvent.BUTTON_TERTIARY != 0) buttons = buttons or PdbProtocol.BUTTON_3
        return buttons
    }

    fun fromKeyCode(keyCode: Int): Int = when (keyCode) {
        KeyEvent.KEYCODE_STYLUS_BUTTON_PRIMARY -> PdbProtocol.BUTTON_1
        KeyEvent.KEYCODE_STYLUS_BUTTON_SECONDARY -> PdbProtocol.BUTTON_2
        KeyEvent.KEYCODE_STYLUS_BUTTON_TERTIARY -> PdbProtocol.BUTTON_3
        else -> 0
    }

    // The Android framework key code is the evidence. Some framework devices report
    // these documented stylus codes with a keyboard-like source, so do not require a
    // vendor-specific source bit and do not accept unknown vendor codes.
    fun isSupportedKeyEvent(event: KeyEvent): Boolean = fromKeyCode(event.keyCode) != 0
}
