package com.mharlos.radiofleet

import android.content.Context
import org.json.JSONArray
import org.json.JSONObject
import java.security.MessageDigest
import java.util.Locale
import kotlin.math.abs

/** Cross-model channel intent; never copy model-specific EEPROM images between radio models. */
data class Channel(
    val slot: Int,
    val name: String,
    val receive: Double,
    val transmit: Double?,
    val txCtcss: Double? = null,
    val rxCtcss: Double? = null,
    val narrow: Boolean = false,
    val power: String = "High"
) {
    fun json(): JSONObject = JSONObject().apply {
        put("slot", slot); put("name", name); put("receive", receive)
        put("transmit", transmit ?: JSONObject.NULL)
        put("txCtcss", txCtcss ?: JSONObject.NULL)
        put("rxCtcss", rxCtcss ?: JSONObject.NULL)
        put("narrow", narrow); put("power", power)
    }
    companion object {
        fun parse(j: JSONObject) = Channel(
            j.getInt("slot"), j.getString("name"), j.getDouble("receive"),
            if (j.isNull("transmit")) null else j.getDouble("transmit"),
            if (j.isNull("txCtcss")) null else j.getDouble("txCtcss"),
            if (j.isNull("rxCtcss")) null else j.getDouble("rxCtcss"),
            j.optBoolean("narrow", false), j.optString("power", "High")
        )
    }
}
data class FleetRadio(val id: String, val label: String, val model: String, val verifiedHash: String = "") {
    fun json() = JSONObject().apply {
        put("id", id); put("label", label); put("model", model); put("verifiedHash", verifiedHash)
    }
    companion object {
        fun parse(j: JSONObject) = FleetRadio(
            j.getString("id"), j.getString("label"), j.getString("model"), j.optString("verifiedHash", "")
        )
    }
}
object StarterProfile {
    // Example values from farm planning; verify each repeater with its operator.
    val channels = listOf(
        Channel(1, "MEISS", 146.880, 146.280, 114.8),
        Channel(2, "WARREN", 146.680, 146.080, 114.8),
        Channel(3, "LONGVW", 440.375, 445.375, 123.0),
        Channel(4, "COLSMP", 146.500, 146.500),
        // Standard UV-5R lacks FRS/GMRS transmitter certification: receive only.
        Channel(5, "RT22RX", 462.6625, null, narrow = true, power = "Low"),
        Channel(6, "CALL2M", 146.520, 146.520)
    )
}
/** CHIRP CSV is portable between distinct CHIRP radio drivers, unlike raw radio images. */
object ChirpCsv {
    private val headers = listOf(
        "Location","Name","Frequency","Duplex","Offset","Tone","rToneFreq","cToneFreq",
        "DtcsCode","DtcsPolarity","RxDtcsCode","CrossMode","Mode","TStep","Skip",
        "Power","Comment","URCALL","RPT1CALL","RPT2CALL","DVCODE"
    )
    private fun num(value: Double) = String.format(Locale.US, "%.6f", value)
    private fun escape(value: String): String =
        if (value.contains(',') || value.contains('"') || value.contains('\n'))
            "\"" + value.replace("\"", "\"\"") + "\"" else value

    fun export(channels: List<Channel>): String = buildString {
        append(headers.joinToString(",")).append('\n')
        for (c in channels.sortedBy { it.slot }) {
            require(c.slot in 0..127 && c.receive > 0) { "Invalid channel slot or frequency" }
            val duplex = when {
                c.transmit == null -> "off"
                abs(c.receive - c.transmit) < 0.0000005 -> ""
                c.transmit > c.receive -> "+"
                else -> "-"
            }
            val offset = if (c.transmit == null) 0.0 else abs(c.transmit - c.receive)
            val toneMode = when {
                c.rxCtcss != null -> "TSQL"
                c.txCtcss != null -> "Tone"
                else -> ""
            }
            val row = listOf(
                c.slot.toString(), c.name.uppercase(Locale.US).take(7), num(c.receive),
                duplex, num(offset), toneMode, num(c.txCtcss ?: 88.5),
                num(c.rxCtcss ?: c.txCtcss ?: 88.5), "023", "NN", "023",
                "Tone->Tone", if (c.narrow) "NFM" else "FM", "5.00", "",
                c.power, "", "", "", "", ""
            )
            append(row.joinToString(",") { escape(it) }).append('\n')
        }
    }
    private fun records(source: String): List<List<String>> {
        val output = mutableListOf<List<String>>()
        var row = mutableListOf<String>()
        val cell = StringBuilder()
        var quoted = false
        var i = 0
        while (i < source.length) {
            val c = source[i]
            when {
                c == '"' && quoted && i + 1 < source.length && source[i + 1] == '"' ->
                    { cell.append('"'); i++ }
                c == '"' -> quoted = !quoted
                c == ',' && !quoted -> { row.add(cell.toString()); cell.clear() }
                (c == '\r' || c == '\n') && !quoted -> {
                    if (c == '\r' && i + 1 < source.length && source[i + 1] == '\n') i++
                    row.add(cell.toString()); cell.clear()
                    if (row.any { it.isNotBlank() }) output.add(row)
                    row = mutableListOf()
                }
                else -> cell.append(c)
            }
            i++
        }
        require(!quoted) { "Unterminated CSV quote" }
        row.add(cell.toString())
        if (row.any { it.isNotBlank() }) output.add(row)
        return output
    }
    fun parse(csv: String): List<Channel> {
        val rows = records(csv)
        require(rows.isNotEmpty()) { "Empty file" }
        val header = rows.first().map { it.trim().removePrefix("\uFEFF") }
        require(header.containsAll(listOf("Location", "Frequency", "Duplex", "Offset"))) {
            "Choose a CHIRP CSV with Location, Frequency, Duplex and Offset fields"
        }
        fun List<String>.field(key: String): String = getOrNull(header.indexOf(key))?.trim() ?: ""
        return rows.drop(1).filter { it.field("Location").isNotBlank() }.map { row ->
            val rx = row.field("Frequency").toDouble()
            val offset = row.field("Offset").toDoubleOrNull() ?: 0.0
            val tx = when (row.field("Duplex").lowercase(Locale.US)) {
                "off" -> null
                "+" -> rx + offset
                "-" -> rx - offset
                "split" -> offset
                else -> rx
            }
            val tone = row.field("Tone")
            val txTone = if (tone == "Tone" || tone == "TSQL" || tone == "Cross")
                row.field("rToneFreq").toDoubleOrNull() else null
            val rxTone = if (tone == "TSQL") row.field("cToneFreq").toDoubleOrNull() else null
            Channel(
                row.field("Location").toInt(), row.field("Name"), rx, tx,
                txTone, rxTone, row.field("Mode").uppercase(Locale.US) == "NFM",
                row.field("Power").ifBlank { "High" }
            )
        }.also { list -> require(list.map { it.slot }.distinct().size == list.size) {
            "Duplicate memory locations in CSV"
        } }
    }
    fun diff(expected: List<Channel>, actual: List<Channel>): List<String> {
        val bySlot = actual.associateBy { it.slot }
        fun equal(a: Double?, b: Double?): Boolean =
            if (a == null || b == null) a == b else abs(a - b) < 0.000011
        return expected.sortedBy { it.slot }.mapNotNull { want ->
            val got = bySlot[want.slot] ?: return@mapNotNull "CH " + want.slot + ": missing"
            val wrong = mutableListOf<String>()
            if (!equal(want.receive, got.receive)) wrong.add("receive")
            if (!equal(want.transmit, got.transmit)) wrong.add("transmit / inhibit")
            if (!equal(want.txCtcss, got.txCtcss)) wrong.add("TX tone")
            if (!equal(want.rxCtcss, got.rxCtcss)) wrong.add("RX tone")
            if (want.narrow != got.narrow) wrong.add("bandwidth")
            if (!want.name.equals(got.name, ignoreCase = true)) wrong.add("name")
            if (wrong.isEmpty()) null else "CH " + want.slot + ": " + wrong.joinToString(", ")
        }
    }
    fun fingerprint(channels: List<Channel>): String {
        val normalized = channels.sortedBy { it.slot }.joinToString("\n") { it.json().toString() }
        return MessageDigest.getInstance("SHA-256").digest(normalized.toByteArray())
            .joinToString("") { "%02x".format(it) }.take(16)
    }
}
class FleetStore(context: Context) {
    private val prefs = context.getSharedPreferences("radio_fleet", Context.MODE_PRIVATE)
    fun channels(): List<Channel> {
        val json = prefs.getString("channels", null) ?: return StarterProfile.channels
        val array = JSONArray(json)
        return (0 until array.length()).map { Channel.parse(array.getJSONObject(it)) }
    }
    fun saveChannels(channels: List<Channel>) {
        val arr = JSONArray()
        channels.sortedBy { it.slot }.forEach { arr.put(it.json()) }
        prefs.edit().putString("channels", arr.toString()).apply()
    }
    fun radios(): List<FleetRadio> {
        val arr = JSONArray(prefs.getString("radios", "[]")!!)
        return (0 until arr.length()).map { FleetRadio.parse(arr.getJSONObject(it)) }
    }
    fun saveRadios(radios: List<FleetRadio>) {
        val arr = JSONArray()
        radios.forEach { arr.put(it.json()) }
        prefs.edit().putString("radios", arr.toString()).apply()
    }
}
