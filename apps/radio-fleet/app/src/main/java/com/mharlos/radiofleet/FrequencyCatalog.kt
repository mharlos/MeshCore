package com.mharlos.radiofleet

import java.util.Locale

/** US channel plan. Catalog entries are templates, not assigned memory slots.
 * Non-amateur services are RX-only on the supported amateur handsets.
 */
internal object FrequencyCatalog {
    val groups: Map<String, List<Channel>> by lazy {
        fun rx(name: String, freq: Double, service: String, detail: String = "", mode: String = "FM") =
            Channel(0, name, freq, null, narrow = mode == "NFM", power = "Low",
                mode = mode, service = service, description = detail)
        fun num(n: Int) = String.format(Locale.US, "%02d", n)
        val shared = (1..22).map { n ->
            val f = when (n) {
                in 1..7 -> 462.5625 + (n - 1) * 0.025
                in 8..14 -> 467.5625 + (n - 8) * 0.025
                else -> 462.550 + (n - 15) * 0.025
            }
            rx("FRGM" + num(n), f, "FRS / GMRS",
                "Shared channel " + n + "; no universal privacy tone", "NFM")
        }
        val gmrs = (15..22).map { n ->
            val f = 462.550 + (n - 15) * 0.025
            rx("GMR" + num(n), f, "GMRS repeater", "Output; input " +
                String.format(Locale.US, "%.4f", f + 5.0) + " MHz; operator-specific tone")
        }
        val weather = listOf(162.550, 162.400, 162.475, 162.425, 162.450, 162.500, 162.525)
            .mapIndexed { i, f -> rx("WX" + (i + 1), f, "NOAA weather", "NOAA Weather Radio") }
        val marine = listOf(
            Triple("MAR06", 156.300, "Intership safety"),
            Triple("MAR09", 156.450, "Calling"),
            Triple("MAR13", 156.650, "Bridge-to-bridge navigation"),
            Triple("MAR16", 156.800, "Distress, safety and calling"),
            Triple("MAR22A", 157.100, "USCG maritime safety"),
            Triple("MAR68", 156.425, "Noncommercial"),
            Triple("MAR69", 156.475, "Noncommercial"),
            Triple("MAR71", 156.575, "Noncommercial"),
            Triple("MAR72", 156.625, "Intership noncommercial"),
            Triple("MAR77", 156.875, "Intership")
        ).map { (n, f, d) -> rx(n, f, "Marine VHF", d) }
        val aviation = listOf(
            Triple("AIRGUAR", 121.500, "Civil aviation emergency guard"),
            Triple("AIR1228", 122.800, "Common CTAF / UNICOM; verify airport"),
            Triple("AIR1227", 122.700, "Common UNICOM; verify airport"),
            Triple("AIR1230", 123.000, "Common UNICOM; verify airport"),
            Triple("AIR12305", 123.050, "Common UNICOM; verify airport"),
            Triple("AIR123075", 123.075, "Common UNICOM; verify airport"),
            Triple("AIR12345", 123.450, "Air-to-air communications where designated")
        ).map { (n, f, d) -> rx(n, f, "Aviation AM", d, "AM") }
        val murs = listOf(151.820, 151.880, 151.940, 154.570, 154.600)
            .mapIndexed { i, f -> rx("MURS" + (i + 1), f, "MURS", "MURS channel " + (i + 1),
                if (i < 3) "NFM" else "FM") }
        val cbFreq = listOf(26.965,26.975,26.985,27.005,27.015,27.025,27.035,27.055,
            27.065,27.075,27.085,27.105,27.115,27.125,27.135,27.155,27.165,27.175,
            27.185,27.205,27.215,27.225,27.255,27.235,27.245,27.265,27.275,27.285,
            27.295,27.305,27.315,27.325,27.335,27.345,27.355,27.365,27.375,27.385,
            27.395,27.405)
        val cb = cbFreq.mapIndexed { i, f -> rx("CB" + num(i+1), f, "CB reference",
            "Reference only; neither handset covers 27 MHz", "AM") }
        val amateur = listOf(
            Channel(0, "CALL2M", 146.520, 146.520, service="Amateur simplex", description="National 2m calling"),
            Channel(0, "CALL70", 446.000, 446.000, service="Amateur simplex", description="National 70cm calling")
        )
        linkedMapOf("FRS / GMRS (22)" to shared, "GMRS repeater outputs (8)" to gmrs,
            "NOAA weather (7)" to weather, "Marine VHF (10)" to marine,
            "Aviation AM (7; UV-5RM only)" to aviation, "MURS (5)" to murs,
            "Amateur simplex (2)" to amateur, "CB (40; reference only)" to cb)
    }

    // Use conservative verified shared receive ranges; AM belongs only to 5RM.
    fun supports(c: Channel, model: String): Boolean {
        if (c.mode == "AM") return model == "UV-5RM" && c.receive in 108.0..136.975 && c.transmit == null
        if (c.mode !in listOf("FM", "NFM")) return false
        return c.receive in 136.0..174.0 || c.receive in 400.0..480.0
    }

    fun forModel(channels: List<Channel>, model: String): List<Channel> =
        channels.filter { supports(it, model) && it.slot in 1..127 }.map {
            it.copy(name = it.name.take(if (model == "UV-5R") 7 else 12),
                power = if (model == "UV-5R" && it.power.equals("Medium", true)) "Low" else it.power)
        }

    fun append(existing: List<Channel>, candidates: List<Channel>): List<Channel> {
        val result = existing.toMutableList()
        val free = (1..127).filter { n -> existing.none { it.slot == n } }.iterator()
        for (c in candidates) {
            if (!supports(c, "UV-5RM")) continue
            if (result.any { kotlin.math.abs(it.receive - c.receive) < 0.000001 &&
                    it.transmit == c.transmit && it.mode == c.mode &&
                    it.txCtcss == c.txCtcss && it.rxCtcss == c.rxCtcss &&
                    it.txDcs == c.txDcs && it.rxDcs == c.rxDcs && it.dcsPolarity == c.dcsPolarity }) continue
            if (!free.hasNext()) break
            result.add(c.copy(slot = free.next()))
        }
        return result
    }
}
