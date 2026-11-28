package com.mharlos.radiofleet

import java.io.ByteArrayOutputStream
import java.util.Locale

/** KISS framing and AX.25 UI text packets (no HDLC FCS in KISS payload). */
internal object KissCodec {
    fun encode(payload: ByteArray): ByteArray {
        require(payload.size in 1..330)
        val out = ByteArrayOutputStream()
        out.write(0xc0)
        out.write(0) // port zero, data frame
        for (byte in payload) when (val b = byte.toInt() and 255) {
            0xc0 -> { out.write(0xdb); out.write(0xdc) }
            0xdb -> { out.write(0xdb); out.write(0xdd) }
            else -> out.write(b)
        }
        out.write(0xc0)
        return out.toByteArray()
    }

    class Decoder(private val onFrame: (ByteArray) -> Unit) {
        private val buffer = ByteArrayOutputStream()
        private var inside = false
        private var escaped = false
        private var invalid = false
        fun accept(byte: Int) {
            val b = byte and 255
            if (b == 0xc0) {
                if (inside && !invalid && !escaped && buffer.size() > 1) {
                    val frame = buffer.toByteArray()
                    if ((frame[0].toInt() and 15) == 0) onFrame(frame.copyOfRange(1, frame.size))
                }
                buffer.reset(); inside = true; escaped = false; invalid = false
                return
            }
            if (!inside || invalid) return
            if (escaped) {
                when (b) {
                    0xdc -> buffer.write(0xc0)
                    0xdd -> buffer.write(0xdb)
                    else -> invalid = true
                }
                escaped = false
            } else if (b == 0xdb) escaped = true else buffer.write(b)
            if (buffer.size() > 2048) invalid = true
        }
    }

    private fun address(raw: String, last: Boolean): ByteArray {
        val parts = raw.trim().uppercase(Locale.US).split("-")
        require(parts.size <= 2 && parts[0].matches(Regex("[A-Z0-9]{1,6}"))) { "Invalid AX.25 callsign" }
        val ssid = if (parts.size == 2) parts[1].toInt() else 0
        require(ssid in 0..15)
        val a = parts[0].padEnd(6).map { (it.code shl 1).toByte() }.toByteArray()
        return a + (0x60 or (ssid shl 1) or if (last) 1 else 0).toByte()
    }

    fun aprsMessage(source: String, recipient: String, text: String): ByteArray {
        require(source.trim().uppercase(Locale.US).matches(Regex("[A-Z0-9]{1,6}(-[0-9]{1,2})?")))
        val to = recipient.trim().uppercase(Locale.US)
        require(to.matches(Regex("[A-Z0-9]{1,6}(-[0-9]{1,2})?")) && to.length <= 9)
        require(text.isNotBlank() && text.length <= 67 && text.all { it.code in 32..126 && it != '{' })
        val info = ":" + to.padEnd(9) + ":" + text
        return address("APRS",false) + address(source,true) + byteArrayOf(0x03,0xf0.toByte()) +
            info.toByteArray(Charsets.US_ASCII)
    }

    fun describe(frame: ByteArray): String {
        if (frame.size < 16) return "Short AX.25 frame (" + frame.size + " bytes)"
        var index = 0
        val addresses = mutableListOf<String>()
        while (index+7 <= frame.size && addresses.size < 10) {
            val call = (0..5).map { ((frame[index+it].toInt() and 255) ushr 1).toChar() }.joinToString("").trim()
            val flags = frame[index+6].toInt() and 255
            val ssid = (flags ushr 1) and 15
            addresses.add(call + if (ssid != 0) "-" + ssid else "")
            index += 7
            if (flags and 1 != 0) {
                if (addresses.size < 2 || index+2 > frame.size) break
                val prefix = addresses[1] + " > " + addresses[0] +
                    (if (addresses.size > 2) " via " + addresses.drop(2).joinToString(",") else "")
                if ((frame[index].toInt() and 255) != 3 || (frame[index+1].toInt() and 255) != 0xf0)
                    return prefix + " · non-UI packet"
                return prefix + ": " + frame.copyOfRange(index+2,frame.size).toString(Charsets.US_ASCII)
            }
        }
        return "Unrecognized AX.25 packet (" + frame.size + " bytes)"
    }
}
