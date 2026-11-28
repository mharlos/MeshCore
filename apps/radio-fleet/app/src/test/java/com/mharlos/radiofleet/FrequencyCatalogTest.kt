package com.mharlos.radiofleet

import org.junit.Assert.*
import org.junit.Test

class FrequencyCatalogTest {
    @Test fun standardServiceFrequenciesAndRxInhibit() {
        val shared = FrequencyCatalog.groups.getValue("FRS / GMRS (22)")
        assertEquals(22, shared.size)
        assertEquals(462.6625, shared[4].receive, 0.000001)
        assertEquals(467.5625, shared[7].receive, 0.000001)
        assertEquals(462.725, shared[21].receive, 0.000001)
        assertTrue(shared.all { it.transmit == null && it.txCtcss == null && it.rxCtcss == null })
        assertEquals(7, FrequencyCatalog.groups.getValue("NOAA weather (7)").size)
        assertEquals(40, FrequencyCatalog.groups.getValue("CB (40; reference only)").size)
    }

    @Test fun modelProjectionPreservesSlotsAndNames() {
        val input = listOf(
            Channel(5, "LONGNAME1234", 146.52, 146.52, power = "Medium"),
            Channel(8, "GUARD", 121.5, null, mode = "AM"),
            Channel(9, "CB19", 27.185, null, mode = "AM"))
        val r = FrequencyCatalog.forModel(input, "UV-5R")
        val rm = FrequencyCatalog.forModel(input, "UV-5RM")
        assertEquals(listOf(5), r.map { it.slot })
        assertEquals("LONGNAM", r.single().name)
        assertEquals("Low", r.single().power)
        assertEquals(listOf(5,8), rm.map { it.slot })
        assertEquals("LONGNAME1234", rm.first().name)
    }

    @Test fun importKeepsExistingSlotsAndSkipsDuplicates() {
        val existing = listOf(Channel(1, "WX1", 162.550, null))
        val weather = FrequencyCatalog.groups.getValue("NOAA weather (7)")
        val merged = FrequencyCatalog.append(existing, weather)
        assertEquals(7, merged.size)
        assertEquals(existing.first(), merged.first())
        assertEquals(7, merged.map { it.slot }.distinct().size)
        assertEquals(merged, FrequencyCatalog.append(merged, weather))
    }

    @Test fun csvPreservesMixedToneAndAm() {
        val channels = listOf(
            Channel(1, "MIXED", 146.88, 146.28, txCtcss = 114.8, rxDcs = 23,
                dcsPolarity = "NR"),
            Channel(2, "GUARD", 121.5, null, mode = "AM"))
        val roundtrip = ChirpCsv.parse(ChirpCsv.export(channels))
        assertTrue(ChirpCsv.diff(channels, roundtrip).isEmpty())
    }
}
