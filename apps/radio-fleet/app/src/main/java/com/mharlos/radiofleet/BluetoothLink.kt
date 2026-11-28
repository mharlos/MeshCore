package com.mharlos.radiofleet

import android.annotation.SuppressLint
import android.bluetooth.BluetoothDevice
import android.bluetooth.BluetoothGatt
import android.bluetooth.BluetoothGattCallback
import android.bluetooth.BluetoothGattCharacteristic
import android.bluetooth.BluetoothGattDescriptor
import android.bluetooth.BluetoothGattService
import android.bluetooth.BluetoothManager
import android.bluetooth.BluetoothProfile
import android.bluetooth.BluetoothStatusCodes
import android.bluetooth.le.ScanCallback
import android.bluetooth.le.ScanResult
import android.content.Context
import android.os.Build
import android.os.Handler
import android.os.Looper
import java.io.IOException
import java.util.UUID
import java.util.concurrent.CountDownLatch
import java.util.concurrent.LinkedBlockingQueue
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicBoolean

/**
 * GATT serial transport shared by the TD-BL1 dongle and Benshi UV-PRO radios.
 *
 * No native Android write succeeds unless GATT is connected, subscription is
 * acknowledged, and every write has succeeded or timed out.
 *
 * The TD-BL1 may additionally require a vendor authentication handshake.
 * This transport must not infer successful programmer authentication from
 * an established GATT connection.
 */
@SuppressLint("MissingPermission")
class BluetoothLink(private val context: Context) {
    enum class Family { BAOFENG_DONLGE, BENSHI_UVPRO }
    @Volatile var lastGattReport: String = "No GATT discovery yet."
        private set
    data class Candidate(val device: BluetoothDevice, val name: String, val rssi: Int)
    private val manager = context.getSystemService(Context.BLUETOOTH_SERVICE) as BluetoothManager
    private val handler = Handler(Looper.getMainLooper())
    private var scan: ScanCallback? = null
    private var gatt: BluetoothGatt? = null
    private var writeChar: BluetoothGattCharacteristic? = null
    private var notifyChar: BluetoothGattCharacteristic? = null
    private val received = LinkedBlockingQueue<Int>(8192)
    private var chunkSize = 20
    private var writeLatch: CountDownLatch? = null
    @Volatile private var writeOk = false
    @Volatile var ready = false
        private set
    @Volatile var family: Family? = null
        private set
    private val closed = AtomicBoolean(false)
    private val uuidCccd = UUID.fromString("00002902-0000-1000-8000-00805f9b34fb")
    private val uartServices = listOf(
        UUID.fromString("0000ffe0-0000-1000-8000-00805f9b34fb"),
        UUID.fromString("6e400001-b5a3-f393-e0a9-e50e24dcca9e"),
        UUID.fromString("49535343-fe7d-4ae5-8fa9-9fafd205e455"),
        UUID.fromString("0000ff00-0000-1000-8000-00805f9b34fb"),
        UUID.fromString("0000fff0-0000-1000-8000-00805f9b34fb")
    )
    // TIDRADIO KIT firmware exposes two data-looking paths in the FF00 service.
    // FF01 notify + FF02 write is the documented transparent BLE-UART channel.
    // FF21 notify + FF22 write is a separate OEM/control path; do not mistake
    // cross-paired FF01/FF22 or FF21/FF02 for valid serial transports.
    private val tidService = UUID.fromString("0000ff00-0000-1000-8000-00805f9b34fb")
    private val tidDataRx = UUID.fromString("0000ff01-0000-1000-8000-00805f9b34fb")
    private val tidDataTx = UUID.fromString("0000ff02-0000-1000-8000-00805f9b34fb")
    private val tidAuxRx = UUID.fromString("0000ff21-0000-1000-8000-00805f9b34fb")
    private val tidAuxTx = UUID.fromString("0000ff22-0000-1000-8000-00805f9b34fb")
    private val benshiService = UUID.fromString("00001100-d102-11e1-9b23-00025b00a5a5")
    private val benshiWrite = UUID.fromString("00001101-d102-11e1-9b23-00025b00a5a5")
    private val benshiNotify = UUID.fromString("00001102-d102-11e1-9b23-00025b00a5a5")

    fun scan(onFound: (Candidate) -> Unit, onError: (String) -> Unit) {
        stopScan()
        val adapter = manager.adapter
        if (adapter?.isEnabled != true) { onError("Enable Bluetooth."); return }
        val scanner = adapter.bluetoothLeScanner ?: run {
            onError("Bluetooth LE scanning unavailable."); return
        }
        val seen = HashSet<String>()
        val cb = object : ScanCallback() {
            override fun onScanResult(callbackType: Int, result: ScanResult) {
                val d = result.device
                if (seen.add(d.address)) {
                    val name = d.name ?: result.scanRecord?.deviceName ?: "Unnamed BLE peripheral"
                    handler.post { onFound(Candidate(d, name, result.rssi)) }
                }
            }
            override fun onScanFailed(errorCode: Int) {
                handler.post { onError("BLE scan error " + errorCode) }
            }
        }
        scan = cb
        scanner.startScan(cb)
        handler.postDelayed({ if (scan === cb) stopScan() }, 13000)
    }
    fun stopScan() {
        val cb = scan ?: return
        scan = null
        try { manager.adapter.bluetoothLeScanner?.stopScan(cb) }
        catch (_: Exception) { }
    }
    /**
     * Full GATT service/characteristic report, with BLE address excluded.
     * Zero radio write operations occur during this enumeration.
     */
    private fun describeGatt(services: List<BluetoothGattService>): String = buildString {
        appendLine("Farm Radio Fleet — GATT diagnostic")
        appendLine("Services: " + services.size)
        for (service in services) {
            appendLine("SERVICE " + service.uuid)
            for (char in service.characteristics) {
                val p = char.properties
                val modes = listOfNotNull(
                    if (p and BluetoothGattCharacteristic.PROPERTY_READ != 0) "READ" else null,
                    if (p and BluetoothGattCharacteristic.PROPERTY_WRITE != 0) "WRITE" else null,
                    if (p and BluetoothGattCharacteristic.PROPERTY_WRITE_NO_RESPONSE != 0) "WRITE_NO_RESPONSE" else null,
                    if (p and BluetoothGattCharacteristic.PROPERTY_NOTIFY != 0) "NOTIFY" else null,
                    if (p and BluetoothGattCharacteristic.PROPERTY_INDICATE != 0) "INDICATE" else null
                )
                appendLine("  CHARACTERISTIC " + char.uuid + " [" + modes.joinToString("|") + "]")
                for (descriptor in char.descriptors) appendLine("    DESCRIPTOR " + descriptor.uuid)
            }
        }
    }

    private data class UartPair(
        val service: BluetoothGattService,
        val write: BluetoothGattCharacteristic,
        val notify: BluetoothGattCharacteristic
    )

    /**
     * Choose matching service-local UART channels only when unambiguous.
     * Never auto-connect to an arbitrary GATT service on a UV-PRO:
     * the Benshi protocol has explicit fixed UUIDs.
     */
    private fun findUart(services: List<BluetoothGattService>): UartPair? {
        // First handle the specific TIDRADIO KIT layout supplied by the
        // operator's BLE GATT report. Don't build a cross-product of two
        // independent RX/TX channel pairs.
        val tid = services.firstOrNull { it.uuid == tidService }
        if (tid != null) {
            val dataRx = tid.getCharacteristic(tidDataRx)
            val dataTx = tid.getCharacteristic(tidDataTx)
            if (dataRx != null && dataTx != null &&
                dataRx.properties and BluetoothGattCharacteristic.PROPERTY_NOTIFY != 0 &&
                dataRx.getDescriptor(uuidCccd) != null &&
                dataTx.properties and (BluetoothGattCharacteristic.PROPERTY_WRITE or
                    BluetoothGattCharacteristic.PROPERTY_WRITE_NO_RESPONSE) != 0
            ) {
                lastGattReport += "\\nTIDRADIO KIT: FF01 (RX notifications) / FF02 (TX writes) selected." +
                    "\\nSecondary FF21/FF22 channel left untouched."
                return UartPair(tid, dataTx, dataRx)
            }
            // FF21/FF22 alone is not proof of a transparent serial transport;
            // give the operator the inventory report rather than writing to
            // an OEM control path of unknown semantics.
            if (tid.getCharacteristic(tidAuxRx) != null ||
                tid.getCharacteristic(tidAuxTx) != null) {
                lastGattReport += "\\nTIDRADIO: FF21/FF22 OEM channel present, no verified FF01/FF02 data pair."
                return null
            }
        }
        val pairs = services.flatMap { service ->
            val writeChars = service.characteristics.filter {
                it.properties and (BluetoothGattCharacteristic.PROPERTY_WRITE or
                    BluetoothGattCharacteristic.PROPERTY_WRITE_NO_RESPONSE) != 0
            }
            val notifyChars = service.characteristics.filter {
                it.properties and (BluetoothGattCharacteristic.PROPERTY_NOTIFY or
                    BluetoothGattCharacteristic.PROPERTY_INDICATE) != 0 &&
                    it.getDescriptor(uuidCccd) != null
            }
            writeChars.flatMap { write ->
                notifyChars.map { notify -> UartPair(service, write, notify) }
            }
        }
        // Prioritize explicitly recognized UART services.
        val known = pairs.filter { it.service.uuid in uartServices }
        return when {
            known.size == 1 -> known.single()
            known.size > 1 -> null
            pairs.size == 1 -> pairs.single()
            else -> null // require GATT inspection for ambiguous/unknown layouts
        }
    }

    fun connect(device: BluetoothDevice, desired: Family, callback: (Result<String>) -> Unit) {
        disconnect()
        closed.set(false)
        lastGattReport = "Connecting — awaiting GATT discovery."
        family = desired
        val resultOnce = AtomicBoolean()
        fun deliver(result: Result<String>) {
            if (resultOnce.compareAndSet(false, true)) {
                handler.post { callback(result) }
            }
        }
        val connection = object : BluetoothGattCallback() {
            override fun onConnectionStateChange(g: BluetoothGatt, status: Int, newState: Int) {
                if (closed.get()) return
                if (status != BluetoothGatt.GATT_SUCCESS ||
                    newState != BluetoothProfile.STATE_CONNECTED) {
                    ready = false
                    deliver(Result.failure(IOException("GATT disconnected: " + status +
                        " state " + newState)))
                    return
                }
                g.requestMtu(185)
                // Some adapters never respond to MTU requests.
                handler.postDelayed({
                    if (!ready && !closed.get() && gatt === g) g.discoverServices()
                }, 1250)
            }
            override fun onMtuChanged(g: BluetoothGatt, mtu: Int, status: Int) {
                if (status == BluetoothGatt.GATT_SUCCESS)
                    chunkSize = (mtu - 3).coerceIn(20, 180)
                if (!closed.get()) g.discoverServices()
            }
            override fun onServicesDiscovered(g: BluetoothGatt, status: Int) {
                if (closed.get()) return
                if (status != BluetoothGatt.GATT_SUCCESS) {
                    deliver(Result.failure(IOException("GATT service discovery failed: " + status)))
                    return
                }
                // Always capture advertised services before choosing a transport.
                // The TD-BL1 appears as "TIDRADIO KIT" but hardware revisions
                // expose different UART UUIDs.
                lastGattReport = describeGatt(g.services)
                val pair = when (desired) {
                    Family.BENSHI_UVPRO -> {
                        val svc = g.getService(benshiService)
                        val write = svc?.getCharacteristic(benshiWrite)
                        val notify = svc?.getCharacteristic(benshiNotify)
                        if (svc != null && write != null && notify != null)
                            UartPair(svc, write, notify) else null
                    }
                    Family.BAOFENG_DONLGE -> findUart(g.services)
                }
                if (pair == null) {
                    deliver(Result.failure(IOException(
                        "No unambiguous UART service/characteristic pair found. " +
                        "Copy the GATT diagnostic report and send it for adapter support."
                    )))
                    return
                }
                val writable = pair.write
                val notify = pair.notify
                lastGattReport += "\nSELECTED SERVICE " + pair.service.uuid +
                    "\nSELECTED WRITE " + writable.uuid +
                    "\nSELECTED NOTIFY " + notify.uuid
                if (writable == null || notify == null) {
                    deliver(Result.failure(IOException("BLE UART requires writable + notifying characteristics")))
                    return
                }
                writeChar = writable
                notifyChar = notify
                received.clear()
                if (!g.setCharacteristicNotification(notify, true)) {
                    deliver(Result.failure(IOException("Could not enable notifications")))
                    return
                }
                val cccd = notify.getDescriptor(uuidCccd)
                if (cccd == null) {
                    deliver(Result.failure(IOException("CCCD missing; notification activation unverified")))
                    return
                }
                val value = if (notify.properties and BluetoothGattCharacteristic.PROPERTY_NOTIFY != 0)
                    BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE
                else BluetoothGattDescriptor.ENABLE_INDICATION_VALUE
                val scheduled = if (Build.VERSION.SDK_INT >= 33)
                    g.writeDescriptor(cccd, value) == BluetoothStatusCodes.SUCCESS
                else {
                    @Suppress("DEPRECATION")
                    cccd.value = value
                    @Suppress("DEPRECATION")
                    g.writeDescriptor(cccd)
                }
                if (!scheduled) deliver(Result.failure(IOException("CCCD write failed")))
            }
            override fun onDescriptorWrite(g: BluetoothGatt, descriptor: BluetoothGattDescriptor, status: Int) {
                if (closed.get()) return
                if (status == BluetoothGatt.GATT_SUCCESS) {
                    ready = true
                    deliver(Result.success("BLE transport connected; radio read/authentication still unverified.\nUART service: " + (notifyChar?.service?.uuid ?: "?")))
                } else deliver(Result.failure(IOException("CCCD subscribe failed: " + status)))
            }
            override fun onCharacteristicChanged(
                g: BluetoothGatt, characteristic: BluetoothGattCharacteristic, value: ByteArray
            ) {
                if (characteristic.uuid == notifyChar?.uuid)
                    for (v in value) received.offer(v.toInt() and 255)
            }
            @Suppress("DEPRECATION")
            override fun onCharacteristicChanged(g: BluetoothGatt, c: BluetoothGattCharacteristic) {
                if (Build.VERSION.SDK_INT < 33 && c.uuid == notifyChar?.uuid)
                    for (v in c.value ?: byteArrayOf()) received.offer(v.toInt() and 255)
            }
            override fun onCharacteristicWrite(g: BluetoothGatt, c: BluetoothGattCharacteristic, status: Int) {
                writeOk = status == BluetoothGatt.GATT_SUCCESS
                writeLatch?.countDown()
            }
        }
        try { gatt = device.connectGatt(context, false, connection, BluetoothDevice.TRANSPORT_LE) }
        catch (e: Exception) { deliver(Result.failure(e)) }
    }

    @Synchronized
    fun write(bytes: ByteArray) {
        val g = gatt ?: throw IOException("Not connected")
        val ch = writeChar ?: throw IOException("No writable BLE characteristic")
        if (!ready || closed.get()) throw IOException("Bluetooth link is not ready")
        var off = 0
        while (off < bytes.size) {
            val count = minOf(bytes.size - off, chunkSize)
            val part = bytes.copyOfRange(off, off + count)
            val withResponse =
                ch.properties and BluetoothGattCharacteristic.PROPERTY_WRITE != 0
            val kind = if (withResponse) BluetoothGattCharacteristic.WRITE_TYPE_DEFAULT
                else BluetoothGattCharacteristic.WRITE_TYPE_NO_RESPONSE
            writeOk = false
            val latch = CountDownLatch(1)
            writeLatch = latch
            val accepted = if (Build.VERSION.SDK_INT >= 33) {
                g.writeCharacteristic(ch, part, kind) == BluetoothStatusCodes.SUCCESS
            } else {
                @Suppress("DEPRECATION")
                ch.value = part
                ch.writeType = kind
                @Suppress("DEPRECATION")
                g.writeCharacteristic(ch)
            }
            if (!accepted) {
                writeLatch = null
                throw IOException("BLE write was rejected at byte " + off)
            }
            if (withResponse) {
                if (!latch.await(3, TimeUnit.SECONDS) || !writeOk) {
                    writeLatch = null
                    throw IOException("BLE write ACK failed or timed out at byte " + off)
                }
            } else {
                // WRITE_NO_RESPONSE has no guaranteed onCharacteristicWrite callback.
                // Keep a conservative gap to avoid overrunning inexpensive UART dongles.
                Thread.sleep(18)
                if (!ready || closed.get())
                    throw IOException("BLE disconnected during no-response transfer")
            }
            writeLatch = null
            off += count
        }
    }
    fun read(size: Int, timeoutMs: Int): ByteArray {
        require(size in 0..8192)
        if (!ready || closed.get()) throw IOException("Link disconnected while reading")
        val out = ByteArray(size)
        var position = 0
        val deadline = System.nanoTime() + TimeUnit.MILLISECONDS.toNanos(timeoutMs.toLong().coerceAtLeast(1))
        while (position < size && !closed.get()) {
            val left = deadline - System.nanoTime()
            if (left <= 0) break
            val b = received.poll(left, TimeUnit.NANOSECONDS) ?: break
            out[position++] = b.toByte()
        }
        return out.copyOf(position)
    }
    fun clearInput() = received.clear()
    fun disconnect() {
        stopScan()
        closed.set(true)
        ready = false
        writeLatch?.countDown()
        try { gatt?.disconnect() } catch (_: Exception) {}
        try { gatt?.close() } catch (_: Exception) {}
        gatt = null; writeChar = null; notifyChar = null
        received.clear()
    }
}

/** Chaquopy Python serial shim calls this on a worker thread. */
object RadioIo {
    @Volatile var active: BluetoothLink? = null
    @JvmStatic fun read(size: Int, timeoutMs: Int): ByteArray =
        (active ?: throw IOException("No connected radio")).read(size, timeoutMs)
    @JvmStatic fun write(data: ByteArray): Int {
        (active ?: throw IOException("No connected radio")).write(data)
        return data.size
    }
    @JvmStatic fun flushInput() {
        (active ?: throw IOException("No connected radio")).clearInput()
    }
}
