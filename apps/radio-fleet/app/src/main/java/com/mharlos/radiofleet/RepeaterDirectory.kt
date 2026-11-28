package com.mharlos.radiofleet

import android.content.Context
import android.location.Geocoder
import org.json.JSONArray
import java.io.File
import java.net.HttpURLConnection
import java.net.URL
import java.util.Locale
import kotlin.math.*

internal object RepeaterDirectory {
    private const val ENDPOINT = "https://hearham.com/api/repeaters/v1"
    private const val MAX_BYTES = 24 * 1024 * 1024
    private const val CACHE_MS = 24L * 60 * 60 * 1000

    private fun distance(lat: Double, lon: Double, y: Double, x: Double): Double {
        val dlat = Math.toRadians(y - lat)
        val dlon = Math.toRadians(x - lon)
        val a = sin(dlat / 2).pow(2) + cos(Math.toRadians(lat)) *
            cos(Math.toRadians(y)) * sin(dlon / 2).pow(2)
        return 3958.76 * 2 * asin(sqrt(a.coerceIn(0.0, 1.0)))
    }

    private fun frequency(value: Double): Double =
        if (value > 1000000) value / 1000000.0 else value

    private fun fetch(context: Context): Pair<JSONArray, String> {
        val file = File(context.cacheDir, "hearham-repeaters.json")
        if (file.exists() && System.currentTimeMillis() - file.lastModified() < CACHE_MS) {
            try { return JSONArray(file.readText()) to "cached" } catch (_: Exception) { }
        }
        try {
            val connection = URL(ENDPOINT).openConnection() as HttpURLConnection
            connection.connectTimeout = 15000
            connection.readTimeout = 25000
            connection.setRequestProperty("User-Agent", "FarmRadioFleet/1.0")
            val data = try {
                require(connection.responseCode == 200) { "Directory HTTP " + connection.responseCode }
                connection.inputStream.use { stream ->
                    val out = java.io.ByteArrayOutputStream()
                    val bytes = ByteArray(8192)
                    while (true) {
                        val n = stream.read(bytes)
                        if (n < 0) break
                        require(out.size() + n <= MAX_BYTES) { "Directory exceeds cache limit" }
                        out.write(bytes, 0, n)
                    }
                    out.toString("UTF-8")
                }
            } finally { connection.disconnect() }
            val parsed = JSONArray(data)
            require(parsed.length() > 0) { "Empty directory" }
            val tmp = File(context.cacheDir, "hearham-repeaters.tmp")
            tmp.writeText(data)
            if (!tmp.renameTo(file)) tmp.delete()
            return parsed to "online"
        } catch (e: Exception) {
            if (file.exists()) return JSONArray(file.readText()) to "stale offline cache"
            throw e
        }
    }

    @Suppress("DEPRECATION")
    fun search(context: Context, city: String, radius: Double, query: String): Pair<String, List<Channel>> {
        val geocoded = Geocoder(context, Locale.US).getFromLocationName(city, 1)?.firstOrNull()
            ?: throw IllegalArgumentException("Location not found; try city, state or ZIP")
        val (rows, source) = fetch(context)
        val found = mutableListOf<Pair<Double, Channel>>()
        for (i in 0 until rows.length()) {
            val row = rows.optJSONObject(i) ?: continue
            val call = row.optString("callsign").trim().uppercase(Locale.US)
            if (call.isBlank() || !call.contains(query, true)) continue
            if (row.has("operational") && (row.optString("operational") in listOf("false","0"))) continue
            val y = row.optDouble("latitude", Double.NaN)
            val x = row.optDouble("longitude", Double.NaN)
            if (!y.isFinite() || !x.isFinite() || y !in -90.0..90.0 || x !in -180.0..180.0) continue
            val miles = distance(geocoded.latitude, geocoded.longitude, y, x)
            if (miles > radius) continue
            val rx = frequency(row.optDouble("frequency", Double.NaN))
            if (!rx.isFinite() || !(rx in 144.0..148.0 || rx in 420.0..450.0 || rx in 462.550..462.725)) continue
            val details = row.optString("description", "")
            val mode = row.optString("mode", "")
            val analog = row.optString("analog", "") in listOf("true","1") ||
                mode.equals("FM", true) || mode.equals("NFM", true) || mode.equals("Analog", true)
            // Directory field variants and access information are not assumed.
            // Import monitoring memories only; operator may enable amateur TX
            // after confirming the input frequency and access tone.
            val offset = row.optString("offset", "")
            val tone = row.optString("encode", row.optString("tone", ""))
            val decode = row.optString("decode", "")
            val summary = String.format(Locale.US, "%.1f mi | HearHam (%s) | mode %s | offset %s | encode %s | decode %s",
                miles, source, mode.ifBlank { "unknown" }, offset.ifBlank { "unknown" },
                tone.ifBlank { "unknown" }, decode.ifBlank { "unknown" })
            val c = Channel(0, call.filter { it.isLetterOrDigit() }.take(12), rx, null,
                narrow = mode.equals("NFM", true), power = "Low",
                service = if (rx >= 462) "GMRS repeater" else "Amateur repeater",
                description = summary + (if (analog) "" else " | Analog capability unconfirmed") +
                    (if (details.isBlank()) "" else " | " + details.take(120)))
            if (c.name.isNotBlank()) found.add(miles to c)
        }
        return "HearHam — " + source + " (RX-only imports)" to found.sortedBy { it.first }.take(150).map { it.second }
    }
}
