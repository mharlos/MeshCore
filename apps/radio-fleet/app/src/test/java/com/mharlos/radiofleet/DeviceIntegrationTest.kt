package com.mharlos.radiofleet

import org.junit.Assert.*
import org.junit.Test

class DeviceIntegrationTest {
    @Test fun defaultServicePlanHasAllFiftyTwoNamedMemories() {
        val service = StarterProfile.serviceChannels
        assertEquals(52, service.size)
        assertEquals(52, service.map { it.slot }.distinct().size)
        assertEquals("FRS01", service.first { it.slot == 50 }.name)
        assertEquals("GMRS01", service.first { it.slot == 70 }.name)
        assertEquals("GMR15", service.first { it.slot == 90 }.name)
        assertEquals("FRS21", service.first { it.slot == 98 }.name)
        assertEquals("GMRS22", service.first { it.slot == 101 }.name)
        assertTrue(service.all { it.transmit != null })
    }

    @Test fun mergeDoesNotOverwriteExistingSlots() {
        val custom = Channel(50,"CUSTOM",146.52,146.52)
        val result = StarterProfile.mergeMissing(listOf(custom))
        assertEquals(custom,result.first { it.slot == 50 })
        assertEquals(82,result.size)
        assertEquals(result,StarterProfile.mergeMissing(result))
    }

    @Test fun regionalDefaultsAreUniqueReceiveOnlyAndModelAware() {
        val all = StarterProfile.channels
        assertEquals(88, all.size)
        assertEquals(all.size, all.map { it.slot }.distinct().size)
        val regional = RegionalCatalog.defaults
        assertEquals((20..49).toList(), regional.map { it.slot })
        assertTrue(regional.all { it.transmit == null && it.rxCtcss == null && it.rxDcs == null })
        assertEquals(124.200, regional.first().receive, 0.000001)
        assertEquals(18, FrequencyCatalog.forModel(regional, "UV-5R").size)
        assertEquals(30, FrequencyCatalog.forModel(regional, "UV-5RM").size)
        assertTrue(RegionalCatalog.digitalReference.none { FrequencyCatalog.supports(it, "UV-5RM") })
        val custom = Channel(20, "CUSTOM", 146.52, 146.52)
        assertEquals(custom, StarterProfile.mergeMissing(listOf(custom)).first { it.slot == 20 })
    }

    @Test fun kissEscapingRoundtripAcrossStreamChunks() {
        val payload = byteArrayOf(1,0xc0.toByte(),0xdb.toByte(),2)
        val seen = mutableListOf<ByteArray>()
        val d = KissCodec.Decoder { seen.add(it) }
        KissCodec.encode(payload).forEach { d.accept(it.toInt()) }
        assertEquals(1,seen.size)
        assertArrayEquals(payload,seen.single())
    }

    @Test fun aprsPacketContainsSourceAndRecipient() {
        val raw = KissCodec.aprsMessage("N0CALL-1","N0CALL-2","TEST")
        assertEquals("N0CALL-1 > APRS: :N0CALL-2 :TEST",KissCodec.describe(raw))
    }
}
