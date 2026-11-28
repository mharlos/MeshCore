package com.mharlos.radiofleet

import java.io.IOException
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.util.concurrent.TimeUnit

/** Shared session contract; commands retain their individual wire formats. */
internal interface DeviceSession {
    fun identity(): String
    fun status(): String
}

internal class BenshiSession(private val link: BluetoothLink) : DeviceSession {
    private fun u(b: Byte) = b.toInt() and 255
    private fun hex(b: ByteArray) = b.joinToString(" ") { "%02x".format(u(it)) }
    private fun command(id: Int, data: ByteArray = byteArrayOf(), response: Boolean = true): ByteArray {
        link.writePacket(byteArrayOf(0, 2, (id ushr 8).toByte(), id.toByte()) + data)
        if (!response) return byteArrayOf()
        val until = System.nanoTime() + TimeUnit.SECONDS.toNanos(5)
        while (System.nanoTime() < until) {
            val frame = link.readPacket(200) ?: continue
            if (frame.size < 5 || u(frame[0]) != 0 || u(frame[1]) != 2) continue
            if (u(frame[2]) != (0x80 or (id ushr 8)) || u(frame[3]) != (id and 255)) continue
            if (u(frame[4]) != 0) throw IOException("Benshi command " + id + " rejected: " + u(frame[4]))
            return frame.copyOfRange(5, frame.size)
        }
        throw IOException("Benshi command " + id + " timed out")
    }
    data class Info(val vendor: Int, val product: Int, val firmware: Int, val regions: Int, val channels: Int, val gmrs: Boolean)
    private var cached: Info? = null
    fun info(): Info {
        val b = command(4, byteArrayOf(3))
        require(b.size == 10) { "Unexpected device info length " + b.size }
        val result = Info(u(b[0]), u(b[1])*256+u(b[2]), u(b[4])*256+u(b[5]),
            ((u(b[6]) and 3) shl 4) or (u(b[7]) ushr 4), u(b[8]), u(b[7]) and 4 != 0)
        require(result.regions in 1..63 && result.channels in 1..255)
        cached = result
        return result
    }
    override fun identity(): String {
        val i = info()
        return "Vendor " + i.vendor + " / product " + i.product + "\nFirmware " + i.firmware +
            "\n" + i.regions + " banks × " + i.channels + " memories\nGMRS firmware flag: " + i.gmrs
    }
    override fun status(): String {
        val b = command(20)
        require(b.size == 2 || b.size == 4) { "Unexpected status length " + b.size }
        val a = u(b[0]); val second = u(b[1])
        val slot = (second ushr 4) or (if (b.size == 4) (((u(b[3]) ushr 2) and 15) shl 4) else 0)
        val region = if (b.size == 4) (((u(b[2]) and 15) shl 2) or (u(b[3]) ushr 6)) else null
        return "TX " + (a and 64 != 0) + " · RX " + (a and 16 != 0) + " · Squelch " + (a and 32 != 0) +
            "\nChannel index " + slot + (region?.let { " · bank " + it } ?: "") +
            "\nGPS lock " + (second and 8 != 0) + " · Scanning " + (a and 2 != 0) +
            (if (b.size == 4) "\nRSSI scale " + (u(b[2]) ushr 4)*100/15 + "%" else "")
    }
    fun battery(): String {
        val b = command(5, byteArrayOf(0,2))
        require(b.size == 4 && u(b[0]) == 0 && u(b[1]) == 2)
        return "Battery: " + String.format(java.util.Locale.US, "%.3f V", (u(b[2])*256+u(b[3]))/1000.0)
    }
    fun position(): String {
        val b = command(76)
        require(b.size in listOf(6,16,18)) { "Unknown GPS reply size " + b.size }
        fun signed24(i: Int): Int {
            val n = (u(b[i]) shl 16) or (u(b[i+1]) shl 8) or u(b[i+2])
            return if (n and 0x800000 != 0) n - 0x1000000 else n
        }
        return String.format(java.util.Locale.US, "Radio GPS: %.6f, %.6f\nCheck GPS lock in Status.", signed24(0)/30000.0,signed24(3)/30000.0)
    }
    fun settings(): ByteArray = command(10).also { require(it.size >= 12) }
    fun settingsSummary(): String {
        val s = settings()
        return "Squelch " + (u(s[1]) and 15) + "\nScan " + (u(s[1]) and 128 != 0) +
            "\nRaw settings (" + s.size + " bytes): " + hex(s)
    }
    fun setSquelch(level: Int, beforeWrite: (ByteArray) -> Unit): String {
        require(level in 0..9)
        val before = settings()
        beforeWrite(before)
        val after = before.copyOf()
        after[1] = ((u(after[1]) and 0xf0) or level).toByte()
        command(11, after)
        val readback = settings()
        require(u(readback[1]) and 15 == level) { "Squelch read-back mismatch" }
        return "Verified squelch " + level
    }
    fun activeBankMemories(): List<String> {
        val count = (cached ?: info()).channels
        return (0 until count).map { index ->
            val b = command(13, byteArrayOf(index.toByte()))
            require(b.size in listOf(25,27) && u(b[0]) == index)
            val bb = ByteBuffer.wrap(b).order(ByteOrder.BIG_ENDIAN)
            val tx = bb.getInt(1) and 0x3fffffff
            val rx = bb.getInt(5) and 0x3fffffff
            val name = b.copyOfRange(15,25).takeWhile { it != 0.toByte() }.toByteArray().toString(Charsets.UTF_8)
            val inhibit = u(b[13]) and 1 != 0
            index.toString() + " " + name + " · RX " + rx/1e6 + " · " + (if (inhibit) "RX ONLY" else "TX " + tx/1e6)
        }
    }
    private fun currentRegion(): Int {
        val b = command(20)
        require(b.size == 4) { "Extended status required to verify active bank" }
        require(u(b[0]) and 64 == 0) { "Radio is transmitting; stop before programming" }
        return ((u(b[2]) and 15) shl 2) or (u(b[3]) ushr 6)
    }
    private fun channel(index: Int): ByteArray = command(13, byteArrayOf(index.toByte())).also {
        require(it.size == 25 && u(it[0]) == index) { "Unsupported channel record or mismatched index" }
    }
    data class BankPlan(val region: Int, val perBank: Int, val channels: List<Channel>)
    fun planBank(master: List<Channel>): BankPlan {
        val info = cached ?: info()
        val region = currentRegion()
        require(region in 0 until info.regions)
        val first = region * info.channels + 1
        val last = first + info.channels - 1
        val candidates = master.filter { it.slot in first..last && FrequencyCatalog.supports(it,"UV-5R") }
        // Unknown FRS/GMRS equipment certification: preserve monitoring only.
        // DCS wire code mapping must be validated before programming those memories.
        val compatible = candidates.filter { it.txDcs == null && it.rxDcs == null }.map {
            val tx = it.transmit
            val permitted = tx != null && (tx in 144.0..148.0 || tx in 420.0..450.0) && it.mode != "AM"
            it.copy(transmit = if (permitted) tx else null, name = it.name.take(10))
        }
        return BankPlan(region,info.channels,compatible)
    }
    fun writeBank(plan: BankPlan, saveBackup: (List<ByteArray>) -> Unit): String {
        require(plan.channels.isNotEmpty())
        require(currentRegion() == plan.region) { "Active bank changed; review again" }
        val original = (0 until plan.perBank).map { channel(it) }
        saveBackup(original)
        val replacements = plan.channels.map { c ->
            val index = c.slot - plan.region * plan.perBank - 1
            val old = original[index]
            require(u(old[14]) and 0xe0 == 0) { "Channel " + c.slot + " has fixed frequency, bandwidth or power" }
            val raw = old.copyOf()
            val bb = ByteBuffer.wrap(raw).order(ByteOrder.BIG_ENDIAN)
            val rx = kotlin.math.round(c.receive * 1e6).toInt()
            val tx = c.transmit?.let { kotlin.math.round(it * 1e6).toInt() } ?: rx
            bb.putInt(1,tx)
            bb.putInt(5,rx)
            bb.putShort(9,(c.txCtcss?.let { kotlin.math.round(it*100).toInt() } ?: 0).toShort())
            bb.putShort(11,(c.rxCtcss?.let { kotlin.math.round(it*100).toInt() } ?: 0).toShort())
            var flags = u(raw[13]) and 0xac // preserve scan, talk-around, pre/deemphasis, sign
            if (c.mode == "FM") flags = flags or 0x10
            if (c.transmit == null) flags = flags or 1
            if (c.power.equals("High",true)) flags = flags or 0x40
            if (c.power.equals("Medium",true)) flags = flags or 2
            raw[13] = flags.toByte()
            for (i in 15 until 25) raw[i] = 0
            val name = c.name.filter { it.code in 32..126 }.toByteArray(Charsets.US_ASCII).take(10)
            name.forEachIndexed { i,b -> raw[15+i] = b }
            index to raw
        }
        for ((index, raw) in replacements) {
            require(currentRegion() == plan.region) { "Active bank changed during programming" }
            require(channel(index).contentEquals(original[index])) { "Memory changed since backup" }
            val reply = command(14,raw)
            require(reply.size == 1 && u(reply[0]) == index) { "Write acknowledgement mismatch" }
            require(channel(index).contentEquals(raw)) { "Memory read-back mismatch at index " + index }
        }
        return "VERIFIED " + replacements.size + " memories in active bank " + plan.region + ". Original backup retained."
    }

    fun subscribeData() {
        command(6, byteArrayOf(1), false)
        command(6, byteArrayOf(2), false)
    }
    fun pollEvent(timeout: Long): String? {
        val b = link.readPacket(timeout) ?: return null
        if (b.size < 5 || u(b[0]) != 0 || u(b[1]) != 2 || u(b[2]) != 0 || u(b[3]) != 9) return null
        return when (u(b[4])) {
            2 -> "Received RF data fragment: " + hex(b.copyOfRange(5,b.size))
            12 -> "Radio reports data TX event"
            else -> null
        }
    }
}

internal class MeshCoreSession(private val link: BluetoothLink) : DeviceSession {
    private fun u(b: Byte) = b.toInt() and 255
    private fun exchange(data: ByteArray, expected: Set<Int>): ByteArray {
        link.writePacket(data)
        val until = System.nanoTime()+TimeUnit.SECONDS.toNanos(5)
        while (System.nanoTime() < until) {
            val b = link.readPacket(200) ?: continue
            if (b.isEmpty()) continue
            val code = u(b[0])
            if (code in expected) return b
            if (code == 1) throw IOException("MeshCore rejected command " + u(data[0]))
        }
        throw IOException("MeshCore command " + u(data[0]) + " timed out")
    }
    private var maxChannels = 0
    override fun identity(): String {
        val info = exchange(byteArrayOf(22,3), setOf(13))
        require(info.size >= 4)
        maxChannels = u(info[3])
        val self = exchange(byteArrayOf(1,0,0,0,0,0,0,0)+"RadioFleet".toByteArray(),setOf(5))
        require(self.size >= 36)
        val key = self.copyOfRange(4,10).joinToString("") { "%02x".format(u(it)) }
        return "MeshCore companion protocol " + u(info[1]) + "\nNode " + key + "… · " + maxChannels + " channel slots"
    }
    override fun status(): String {
        val b = exchange(byteArrayOf(20),setOf(12))
        require(b.size >= 3)
        return "Battery " + (u(b[1])+u(b[2])*256)/1000.0 + " V"
    }
    data class Group(val index: Int,val name: String) { override fun toString() = index.toString() + " · " + name }
    fun channels(): List<Group> {
        val result = mutableListOf<Group>()
        for (i in 0 until maxChannels.coerceAtMost(64)) {
            try {
                val b = exchange(byteArrayOf(31,i.toByte()),setOf(18))
                if (b.size >= 34 && u(b[1]) == i) {
                    val name = b.copyOfRange(2,34).takeWhile { it != 0.toByte() }.toByteArray().toString(Charsets.UTF_8)
                    if (name.isNotBlank()) result.add(Group(i,name))
                }
            } catch (_: IOException) { /* No keys persisted or logged. */ }
        }
        return result
    }
    data class Contact(val key: ByteArray, val name: String, val type: Int) {
        override fun toString() = name + " · " + key.take(6).joinToString("") { "%02x".format(it.toInt() and 255) }
    }
    fun contacts(): List<Contact> {
        val start = exchange(byteArrayOf(4),setOf(2))
        require(start.size == 5)
        val count = ByteBuffer.wrap(start,1,4).order(ByteOrder.LITTLE_ENDIAN).int
        require(count in 0..4096)
        val out = mutableListOf<Contact>()
        val deadline = System.nanoTime() + TimeUnit.SECONDS.toNanos(90)
        while (System.nanoTime() < deadline) {
            val b = link.readPacket(5000) ?: throw IOException("Contact sync timed out")
            if (b.isEmpty()) continue
            when (u(b[0])) {
                4 -> return out
                1 -> throw IOException("Contact sync rejected")
                3 -> {
                    // Name and four 32-bit trailing fields follow fixed path data.
                    require(b.size >= 84)
                    val nameStart = b.size-48
                    val name = b.copyOfRange(nameStart,nameStart+32).takeWhile { it != 0.toByte() }.toByteArray().toString(Charsets.UTF_8)
                    out.add(Contact(b.copyOfRange(1,33), name.ifBlank { "Unnamed node" },u(b[33])))
                    require(out.size <= 4096)
                }
            }
        }
        throw IOException("Contact sync timed out")
    }
    fun sendDirect(contact: Contact, message: String): String {
        require(contact.type == 1) { "Select a chat contact" }
        val bytes = message.toByteArray(Charsets.UTF_8)
        require(bytes.isNotEmpty() && bytes.size <= 120 && !message.contains('\u0000'))
        val header = ByteBuffer.allocate(13).order(ByteOrder.LITTLE_ENDIAN)
            .put(2.toByte()).put(0.toByte()).put(0.toByte()).putInt((System.currentTimeMillis()/1000).toInt())
            .put(contact.key.copyOfRange(0,6)).array()
        val reply = exchange(header+bytes,setOf(6))
        require(reply.size == 10)
        return "Node accepted direct message; over-air delivery not confirmed."
    }
    fun setName(name: String): String {
        val bytes = name.trim().toByteArray(Charsets.UTF_8)
        require(bytes.isNotEmpty() && bytes.size <= 31 && !name.contains('\u0000'))
        exchange(byteArrayOf(8)+bytes,setOf(0))
        return "Node acknowledged name update."
    }
    fun send(group: Int, message: String) {
        require(group in 0 until maxChannels)
        val body = message.toByteArray(Charsets.UTF_8)
        require(body.isNotEmpty() && body.size <= 120 && !message.contains('\u0000'))
        val header = ByteBuffer.allocate(7).order(ByteOrder.LITTLE_ENDIAN)
            .put(3.toByte()).put(0.toByte()).put(group.toByte()).putInt((System.currentTimeMillis()/1000).toInt()).array()
        exchange(header+body,setOf(0))
    }
    fun syncMessages(): List<String> {
        val out = mutableListOf<String>()
        repeat(64) {
            val b = exchange(byteArrayOf(10),setOf(7,8,10,16,17,27))
            when (u(b[0])) {
                10 -> return out
                17,8 -> {
                    val start = if (u(b[0]) == 17) 4 else 1
                    if (b.size >= start+7) out.add("CH " + u(b[start]) + ": " + b.copyOfRange(start+7,b.size).toString(Charsets.UTF_8))
                }
                16,7 -> {
                    val start = if (u(b[0]) == 16) 4 else 1
                    if (b.size >= start+12 && u(b[start+7]) == 0) {
                        val sender = b.copyOfRange(start,start+6).joinToString("") { "%02x".format(u(it)) }
                        out.add(sender + ": "+b.copyOfRange(start+12,b.size).toString(Charsets.UTF_8))
                    } else out.add("Non-text direct message received")
                }
                else -> out.add("Mesh data received (not decoded)")
            }
        }
        return out
    }
}
