package com.mharlos.radiofleet

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class ChirpCsvTest {
    @Test fun roundTripRepeaterAndRxOnly() {
        val original = listOf(
            Channel(1, "PRIMARY", 146.880, 146.280, 114.8),
            Channel(3, "BACKUP", 440.375, 445.375, 123.0),
            Channel(5, "RT22RX", 462.6625, null, narrow = true)
        )
        val readback = ChirpCsv.parse(ChirpCsv.export(original))
        assertTrue(ChirpCsv.diff(original, readback).isEmpty())
        assertEquals(null, readback.first { it.slot == 5 }.transmit)
        assertEquals(445.375, readback.first { it.slot == 3 }.transmit!!, 0.000001)
    }
    @Test fun mismatchedToneIsFlagged() {
        val expected = listOf(Channel(1, "PRIMARY", 146.880, 146.280, 114.8))
        val actual = listOf(Channel(1, "PRIMARY", 146.880, 146.280, 123.0))
        assertTrue(ChirpCsv.diff(expected, actual).single().contains("TX tone"))
    }
    @Test fun nonmatchingTransmitInhibitIsFlagged() {
        val expected = listOf(Channel(5, "RXONLY", 462.6625, null))
        val actual = listOf(Channel(5, "RXONLY", 462.6625, 462.6625))
        assertFalse(ChirpCsv.diff(expected, actual).isEmpty())
    }
}
