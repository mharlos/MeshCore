package com.mharlos.radiofleet

import org.junit.Assert.*
import org.junit.Test

class TidradioBaudProtocolTest {
    @Test fun modelBaudMappingMatchesCapture() {
        assertEquals(9600, TidradioBaudProtocol.required("UV-5R"))
        assertEquals(115200, TidradioBaudProtocol.required("UV-5RM"))
        assertEquals("AT+BAUD=2]", TidradioBaudProtocol.setCommand(9600))
        assertEquals("AT+BAUD=7]", TidradioBaudProtocol.setCommand(115200))
    }

    @Test fun parseCapturedReplies() {
        assertEquals(9600, TidradioBaudProtocol.reportedBaud("\r\n+BAUD: 9600bps\r\nOK\r\n"))
        assertEquals(115200, TidradioBaudProtocol.reportedBaud("\r\n+BAUD: 115200bps\r\nOK\r\n"))
    }

    @Test fun incompleteAndInvalidRepliesFailClosed() {
        assertNull(TidradioBaudProtocol.reportedBaud("\r\n+BAUD: 9600bps\r\n"))
        assertNull(TidradioBaudProtocol.reportedBaud("\r\nOK\r\n"))
        assertNull(TidradioBaudProtocol.reportedBaud("\r\n+BAUD: wrongbps\r\nOK\r\n"))
        assertFalse(TidradioBaudProtocol.acknowledged("NOT OK"))
    }

    @Test fun rejectUnsupportedBaud() {
        assertThrows(IllegalStateException::class.java) { TidradioBaudProtocol.setCommand(19200) }
    }
}
