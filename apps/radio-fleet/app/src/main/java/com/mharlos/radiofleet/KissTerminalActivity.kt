package com.mharlos.radiofleet

import android.Manifest
import android.annotation.SuppressLint
import android.app.Activity
import android.app.AlertDialog
import android.bluetooth.BluetoothManager
import android.bluetooth.BluetoothSocket
import android.content.Context
import android.content.pm.PackageManager
import android.os.Build
import android.os.Bundle
import android.widget.*
import java.util.UUID
import java.util.concurrent.Executors

/** Native classic Bluetooth SPP KISS terminal. User selects a paired UV-PRO. */
@SuppressLint("MissingPermission")
class KissTerminalActivity : Activity() {
    private val io = Executors.newSingleThreadExecutor()
    @Volatile private var socket: BluetoothSocket? = null
    @Volatile private var dead = false
    private lateinit var log: TextView
    private lateinit var call: EditText
    private lateinit var to: EditText
    private lateinit var body: EditText
    private lateinit var send: Button

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        val root = LinearLayout(this).apply { orientation = LinearLayout.VERTICAL; setPadding(20,20,20,20) }
        val scroll = ScrollView(this).apply { addView(root) }
        setContentView(scroll)
        root.addView(TextView(this).apply {
            textSize = 20f
            text = "BTECH APRS / KISS terminal"
        })
        root.addView(TextView(this).apply {
            text = "Pair the radio in Android Bluetooth settings, enable its KISS TNC, and select the appropriate amateur packet channel on the radio. No automatic RF transmissions."
        })
        root.addView(Button(this).apply { text = "Connect paired radio"; setOnClickListener { choose() } })
        root.addView(Button(this).apply { text = "Disconnect"; setOnClickListener { closeSocket() } })
        fun field(h: String) = EditText(this).apply { hint = h; setSingleLine(true); root.addView(this) }
        call = field("Your amateur callsign / SSID")
        to = field("Destination callsign / SSID")
        body = field("APRS text (67 ASCII characters max)")
        send = Button(this).apply { text = "Send APRS packet"; isEnabled = false; setOnClickListener { confirmSend() } }
        root.addView(send)
        log = TextView(this).apply { setTextIsSelectable(true); text = "Disconnected" }
        root.addView(log)
    }
    private fun append(line: String) {
        runOnUiThread {
            if (!dead) log.text = (log.text.toString() + "\n" + line).takeLast(16000)
        }
    }
    private fun choose() {
        if (Build.VERSION.SDK_INT >= 31 && checkSelfPermission(Manifest.permission.BLUETOOTH_CONNECT) != PackageManager.PERMISSION_GRANTED) {
            requestPermissions(arrayOf(Manifest.permission.BLUETOOTH_CONNECT), 80)
            return
        }
        val adapter = (getSystemService(Context.BLUETOOTH_SERVICE) as BluetoothManager).adapter
        val list = adapter?.bondedDevices?.toList().orEmpty()
        if (list.isEmpty()) { append("No paired devices. Pair the radio in Android Bluetooth settings."); return }
        AlertDialog.Builder(this).setTitle("Select paired KISS radio")
            .setItems(list.map { it.name ?: "Unnamed device" }.toTypedArray()) { _, i ->
                closeSocket()
                val device = list[i]
                io.submit {
                    try {
                        val s = device.createRfcommSocketToServiceRecord(UUID.fromString("00001101-0000-1000-8000-00805f9b34fb"))
                        socket = s
                        s.connect()
                        if (dead || socket !== s) { s.close(); return@submit }
                        append("SPP connected; KISS framing pending first packet.")
                        runOnUiThread { if (!dead) send.isEnabled = true }
                        Thread {
                            val parser = KissCodec.Decoder { append(KissCodec.describe(it)) }
                            try {
                                val input = s.inputStream
                                val b = ByteArray(1024)
                                while (!dead && socket === s) {
                                    val n = input.read(b)
                                    if (n < 0) break
                                    for (j in 0 until n) parser.accept(b[j].toInt())
                                }
                            } catch (e: Exception) {
                                if (socket === s) append("Receive stopped: " + e.message)
                            } finally {
                                if (socket === s) closeSocket()
                            }
                        }.start()
                    } catch (e: Exception) { append("Connection failed: " + e.message); closeSocket() }
                }
            }.setNegativeButton("Cancel",null).show()
    }
    private fun confirmSend() {
        val s = socket ?: return
        val frame = try {
            KissCodec.encode(KissCodec.aprsMessage(call.text.toString(),to.text.toString(),body.text.toString()))
        } catch (e: Exception) { append("Invalid APRS packet: " + (e.message ?: "check fields")); return }
        AlertDialog.Builder(this).setTitle("Transmit APRS text?")
            .setMessage("Confirm the radio is on an authorized amateur packet channel and your callsign is correct. The radio will transmit RF.")
            .setNegativeButton("Cancel",null)
            .setPositiveButton("Send once") { _,_ ->
                io.submit {
                    try {
                        if (socket !== s || !s.isConnected) error("Radio disconnected")
                        s.outputStream.write(frame)
                        s.outputStream.flush()
                        append("Packet sent to TNC; over-air delivery not confirmed.")
                    } catch (e: Exception) { append("Send failed: " + e.message); closeSocket() }
                }
            }.show()
    }
    private fun closeSocket() {
        val old = socket
        socket = null
        try { old?.close() } catch (_: Exception) {}
        runOnUiThread { if (!dead) send.isEnabled = false }
    }
    override fun onDestroy() {
        dead = true
        closeSocket()
        io.shutdownNow()
        super.onDestroy()
    }
}
