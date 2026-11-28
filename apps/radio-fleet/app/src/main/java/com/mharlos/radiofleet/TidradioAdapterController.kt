package com.mharlos.radiofleet

import java.io.IOException
import java.util.concurrent.TimeUnit

/** TD-BL1 control protocol observed in the owner's ODmaster capture.
 * No opaque initialization/authentication payloads are replayed.
 */
internal object TidradioBaudProtocol {
    fun required(model: String): Int = when (model) {
        "UV-5R" -> 9600
        "UV-5RM" -> 115200
        else -> error("Unsupported TD-BL1 radio model")
    }

    fun setCommand(baud: Int): String = when (baud) {
        9600 -> "AT+BAUD=2]"
        115200 -> "AT+BAUD=7]"
        else -> error("Unsupported TD-BL1 baud")
    }

    fun acknowledged(reply: String): Boolean =
        reply.lineSequence().any { it.trim() == "OK" }

    fun reportedBaud(reply: String): Int? {
        if (!acknowledged(reply)) return null
        return Regex("""(?m)^\+BAUD:\s*(\d+)bps\r?$""")
            .find(reply)?.groupValues?.get(1)?.toIntOrNull()
    }
}

internal class TidradioAdapterController(private val link: BluetoothLink) {
    private fun exchange(command: String): String {
        link.sendAdapterCommand(command)
        val reply = StringBuilder()
        val deadline = System.nanoTime() + TimeUnit.SECONDS.toNanos(5)
        while (System.nanoTime() < deadline) {
            val left = TimeUnit.NANOSECONDS.toMillis(deadline - System.nanoTime()).coerceAtLeast(1)
            val next = link.adapterReply(left) ?: break
            reply.append(next.toString(Charsets.US_ASCII))
            if (reply.length > 512) throw IOException("TD-BL1 control response too long")
            val text = reply.toString()
            // Require a complete terminal line; notifications may be fragmented.
            if (text.endsWith("\n") && TidradioBaudProtocol.acknowledged(text)) return text
            if (text.lineSequence().any { it.trim() == "ERROR" })
                throw IOException("TD-BL1 rejected " + command)
        }
        throw IOException("TD-BL1 control timeout for " + command + "; no complete OK response")
    }

    fun queryBaud(): Int = TidradioBaudProtocol.reportedBaud(exchange("AT+BAUD?]"))
        ?: throw IOException("TD-BL1 returned no recognizable baud rate")

    fun ensureBaud(model: String, reconnect: () -> Unit, progress: (String) -> Unit): Int {
        val desired = TidradioBaudProtocol.required(model)
        progress("Querying TD-BL1 UART baud...")
        val current = queryBaud()
        if (current != desired) {
            progress("TD-BL1 baud " + current + " → " + desired + "; configuring adapter...")
            exchange(TidradioBaudProtocol.setCommand(desired))
            link.sendAdapterCommand("AT+RST]")
            // Reset deliberately terminates GATT; no ATT response is expected.
            val deadline = System.nanoTime() + TimeUnit.SECONDS.toNanos(8)
            while (link.ready && System.nanoTime() < deadline) Thread.sleep(100)
            if (link.ready) throw IOException("TD-BL1 reset did not disconnect; reconnect manually")
            progress("Adapter reset. Reconnecting...")
            reconnect()
            if (!link.hasTidradioControl) throw IOException("TD-BL1 control service missing after reset")
            val confirmed = queryBaud()
            if (confirmed != desired)
                throw IOException("TD-BL1 baud mismatch after reset: expected " + desired + ", got " + confirmed)
        }
        link.recordBaud(desired)
        return desired
    }
}
