package io.paddrawboard.client.input

import org.junit.Assert.*
import org.junit.Test

class PalmRejectionPolicyTest {
    @Test fun touchIsSuppressedDuringStylusAndFor150msAfterwards() { val policy = PalmRejectionPolicy({ true }); policy.stylusObserved(100); assertTrue(policy.suppressTouch(100)); policy.stylusLeftRange(200); assertTrue(policy.suppressTouch(350)); assertFalse(policy.suppressTouch(351)) }
    @Test fun settingCanDisableSuppression() { val policy = PalmRejectionPolicy({ false }); policy.stylusObserved(1); assertFalse(policy.suppressTouch(1)) }
}
