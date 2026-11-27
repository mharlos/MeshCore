package com.mharlos.radiofleet

import org.json.JSONArray
import org.json.JSONObject
import java.io.IOException
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.nio.charset.Charset
import java.security.MessageDigest
import java.util.Locale
import kotlin.math.roundToInt

/**
 * Benshi BLE command transport for BTECH UV-PRO / VR-N76 family.
 * Protocol structure independently implemented from the documented benlink
 * (Apache-2.0) RF channel layout and OpenHT (Apache-2.0) command reference.
 *
 * Firmware flashing is NOT implemented; memory programming only.
 */
class BenshiProgrammer(private val link: BluetoothLink) {
    data class Identity(
        val vendor: Int, val product: Int, val hardware: Int,
        val firmware: Int, val groups: Int, val perGroup: Int
    ) {
        val count: Int get() = groups * perGroup
        fun json(): JSONObject = JSONObject().apply {
            put("vendor", vendor); put("product", product); put("hardware", hardware)
            put("firmware", firmware); put("groups", groups); put("perGroup", perGroup)
        }
    }
    data class Memory(val group: Int, val position: Int, val raw: ByteArray) {
        fun json(): JSONObject = JSONObject().apply {
            put("group", group); put("position", position)
            put("rawHex", raw.joinToString("") { "%02x".format(it.toInt() and 255) })
        }
    }
    data class Snapshot(val identity: Identity, val memories: List<Memory>) {
        fun json(): String = JSONObject().apply {
            put("identity", identity.json())
            val data = JSONArray()
            memories.forEach { data.put(it.json()) }
            put("memories", data)
        }.toString(2)
        val fingerprint: String get() = sha256(json().toByteArray())
    }
    private fun sha256(input: ByteArray) =
        MessageDigest.getInstance("SHA-256").digest(input)
            .joinToString("") { "%02x".format(it.toInt() and 255) }

    private fun pack(command: Int, body: ByteArray = byteArrayOf()) =
        byteArrayOf(0, 2, ((command ushr 8) and 0x7f).toByte(), (command and 255).toByte()) + body

    /** Receives a specific basic-command reply. A malformed frame halts the transaction. */
    private fun response(command: Int, bodySize: Int, waitMs: Int = 3500): ByteArray {
        val header = link.read(4, waitMs)
        if (header.size != 4 || (header[0].toInt() and 255) != 0 ||
            (header[1].toInt() and 255) != 2 ||
            (header[2].toInt() and 255) != 0x80 ||
            (header[3].toInt() and 255) != command
        ) throw IOException("Benshi response header mismatch for command " + command +
            ": " + header.joinToString(" ") { "%02x".format(it.toInt() and 255) })
        return link.read(bodySize, waitMs).also {
            if (it.size != bodySize) throw IOException("Short Benshi reply for " + command)
            if (it[0].toInt() != 0) throw IOException("Benshi command " + command +
                " rejected with status " + (it[0].toInt() and 255))
        }
    }
    private fun query(command: Int, body: ByteArray, replyLen: Int): ByteArray {
        link.write(pack(command, body))
        return response(command, replyLen)
    }
    fun identity(): Identity {
        val payload = query(4, byteArrayOf(3), 11) // 1 status + 10-byte capabilities
        val bytes = payload.copyOfRange(1, payload.size)
        val vendor = bytes[0].toInt() and 255
        val product = ((bytes[1].toInt() and 255) shl 8) or (bytes[2].toInt() and 255)
        val hw = bytes[3].toInt() and 255
        val fw = ((bytes[4].toInt() and 255) shl 8) or (bytes[5].toInt() and 255)
        val groups = ((bytes[6].toInt() and 3) shl 4) or ((bytes[7].toInt() and 255) shr 4)
        val perGroup = bytes[8].toInt() and 255
        require(vendor > 0 && groups in 1..12 && perGroup in 1..64) {
            "Unexpected Benshi identity: vendor " + vendor + " groups " + groups +
                " channels " + perGroup
        }
        return Identity(vendor, product, hw, fw, groups, perGroup)
    }

    /** Explicitly sets the active radio channel group, then clears unsolicited data. */
    private fun region(group: Int) {
        link.clearInput()
        link.write(pack(60, byteArrayOf(group.toByte())))
        Thread.sleep(180)
        link.clearInput()
    }
    private fun getChannel(index: Int): ByteArray {
        val payload = query(13, byteArrayOf(index.toByte()), 26)
        val data = payload.copyOfRange(1, 26)
        if ((data[0].toInt() and 255) != index) throw IOException(
            "Benshi channel mismatch: requested " + index + " got " + (data[0].toInt() and 255)
        )
        return data
    }

    fun read(): Snapshot {
        if (!link.ready || link.family != BluetoothLink.Family.BENSHI_UVPRO)
            throw IOException("Connect directly to the UV-PRO Bluetooth control service first")
        link.clearInput()
        val id = identity()
        // Handshake: dev-info followed by settings + beacon settings, same order
        // described by the OpenHT protocol reference. Responses are not edited.
        link.write(pack(10)); Thread.sleep(100); link.clearInput()
        link.write(pack(33)); Thread.sleep(100); link.clearInput()
        val list = ArrayList<Memory>(id.count)
        for (g in 0 until id.groups) {
            region(g)
            for (slot in 0 until id.perGroup) {
                var bytes: ByteArray? = null
                var failure: Exception? = null
                for (attempt in 0..1) {
                    try {
                        link.clearInput()
                        bytes = getChannel(slot)
                        break
                    } catch (e: Exception) {
                        failure = e
                        if (attempt == 0) Thread.sleep(180)
                    }
                }
                list.add(Memory(g, slot, bytes ?: throw IOException(
                    "Channel read failed group " + g + ", slot " + slot, failure
                )))
            }
        }
        return Snapshot(id, list)
    }

    /** Convert global 1-based logical slot into Benshi's group/slot address. */
    fun convert(channel: Channel, original: Memory, groupSize: Int): Memory {
        require(channel.slot > 0 && groupSize > 0)
        val group = (channel.slot - 1) / groupSize
        val position = (channel.slot - 1) % groupSize
        require(original.group == group && original.position == position)
        val raw = original.raw.copyOf()
        require(raw.size == 25)
        val tx = channel.transmit?.let { (it * 1_000_000).roundToInt() } ?: 0
        val rx = (channel.receive * 1_000_000).roundToInt()
        val bb = ByteBuffer.wrap(raw).order(ByteOrder.BIG_ENDIAN)
        bb.put(0, position.toByte())
        bb.putInt(1, tx)
        bb.putInt(5, rx)
        bb.putShort(9, (channel.txCtcss?.times(100)?.roundToInt() ?: 0).toShort())
        bb.putShort(11, (channel.rxCtcss?.times(100)?.roundToInt() ?: 0).toShort())
        var f = raw[13].toInt() and 255
        f = if (channel.narrow) (f and 0xEF) else (f or 0x10)
        f = if (channel.transmit == null) (f or 0x01) else (f and 0xFE)
        f = if (channel.power.equals("High", true)) (f or 0x40) else (f and 0xBF)
        f = if (channel.power.equals("Medium", true)) (f or 0x02) else (f and 0xFD)
        raw[13] = f.toByte()
        for (i in 15 until 25) raw[i] = 0
        channel.name.toByteArray(Charsets.UTF_8).take(10).forEachIndexed { i, b ->
            raw[15+i] = b
        }
        return original.copy(raw = raw)
    }

    private fun putChannel(memory: Memory) {
        val res = query(14, memory.raw, 2)
        if ((res[1].toInt() and 255) != memory.position) throw IOException(
            "Write acknowledged a different channel"
        )
    }

    /**
     * Writes only selected memories after re-reading each existing target slot.
     * Entire original snapshot must have been saved by the caller before invoking.
     * Following each write, READ_RF_CH must match byte-for-byte.
     */
    fun write(snapshot: Snapshot, selected: List<Channel>, progress: (Int, Int) -> Unit) {
        require(selected.isNotEmpty() && selected.size == selected.map { it.slot }.distinct().size)
        val connected = identity()
        require(connected == snapshot.identity) {
            "Radio identity differs from captured backup; re-read the radio"
        }
        val original = snapshot.memories.associateBy { it.group to it.position }
        var currentGroup = -1
        selected.sortedBy { it.slot }.forEachIndexed { index, channel ->
            require(channel.slot in 1..connected.count) { "Channel outside radio capacity" }
            val group = (channel.slot - 1) / connected.perGroup
            val slot = (channel.slot - 1) % connected.perGroup
            if (group != currentGroup) { region(group); currentGroup = group }
            val old = original[group to slot] ?: throw IOException("Backup missing channel")
            link.clearInput()
            require(getChannel(slot).contentEquals(old.raw)) {
                "Radio channel " + channel.slot + " changed since backup; write cancelled"
            }
            val written = convert(channel, old, connected.perGroup)
            putChannel(written)
            link.clearInput()
            require(getChannel(slot).contentEquals(written.raw)) {
                "Radio did not read back the requested data at channel " + channel.slot
            }
            progress(index + 1, selected.size)
        }
    }

    /** Decode a channel from the Benshi wire format into the shared profile. */
    fun decode(memory: Memory, groupSize: Int): Channel {
        val bytes = memory.raw
        val buf = ByteBuffer.wrap(bytes).order(ByteOrder.BIG_ENDIAN)
        val tx = buf.getInt(1) and 0x3fff_ffff
        val rx = buf.getInt(5) and 0x3fff_ffff
        val txTone = buf.getShort(9).toInt() and 0xffff
        val rxTone = buf.getShort(11).toInt() and 0xffff
        val flags = bytes[13].toInt() and 255
        val name = bytes.copyOfRange(15, 25)
            .takeWhile { it != 0.toByte() }.toByteArray().toString(Charsets.UTF_8)
        return Channel(
            slot = memory.group * groupSize + memory.position + 1,
            name = name,
            receive = rx.toDouble() / 1e6,
            transmit = if (flags and 1 != 0) null else tx.toDouble() / 1e6,
            txCtcss = if (txTone >= 6700) txTone / 100.0 else null,
            rxCtcss = if (rxTone >= 6700) rxTone / 100.0 else null,
            narrow = flags and 0x10 == 0,
            power = if (flags and 0x40 != 0) "High" else if (flags and 0x02 != 0) "Medium" else "Low"
        )
    }
}
