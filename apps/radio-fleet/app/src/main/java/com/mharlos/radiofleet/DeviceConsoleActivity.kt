package com.mharlos.radiofleet

import android.Manifest
import android.app.Activity
import android.app.AlertDialog
import android.bluetooth.BluetoothDevice
import android.content.ClipData
import android.content.ClipboardManager
import android.content.Context
import android.content.pm.PackageManager
import android.os.Build
import android.os.Bundle
import android.widget.*
import android.view.View
import java.io.File
import java.io.IOException
import java.util.concurrent.CountDownLatch
import java.util.concurrent.Executors
import java.util.concurrent.TimeUnit

/** One device session per console. No automatic RF transmission on connection. */
class DeviceConsoleActivity : Activity() {
    private lateinit var link: BluetoothLink
    private lateinit var host: LinearLayout
    private lateinit var devices: LinearLayout
    private lateinit var report: TextView
    private lateinit var controls: LinearLayout
    private val work = Executors.newSingleThreadExecutor()
    @Volatile private var busy = false
    @Volatile private var destroyed = false
    private var session: DeviceSession? = null
    private var meshGroups: List<MeshCoreSession.Group> = emptyList()
    private var family = BluetoothLink.Family.BENSHI_UVPRO
    private var selectedDevice: BluetoothDevice? = null

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        family = if (intent.getStringExtra("family") == "meshcore") BluetoothLink.Family.MESHCORE
            else BluetoothLink.Family.BENSHI_UVPRO
        link = BluetoothLink(this)
        host = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(20,20,20,20)
        }
        val scroll = ScrollView(this).apply { addView(host) }
        setContentView(scroll)
        host.addView(TextView(this).apply {
            textSize = 22f
            text = if (family == BluetoothLink.Family.MESHCORE) "MeshCore Companion" else "BTECH Native Bluetooth"
        })
        button(host, "Scan / connect") { if (!busy && permitted()) scan() }
        button(host, "Disconnect") { if (!busy) { link.disconnect(); session = null; controls.removeAllViews(); report.text = "Disconnected" } }
        devices = LinearLayout(this).apply { orientation = LinearLayout.VERTICAL }
        host.addView(devices)
        report = TextView(this).apply { text = "Select a native Bluetooth device."; setTextIsSelectable(true) }
        host.addView(report)
        controls = LinearLayout(this).apply { orientation = LinearLayout.VERTICAL }
        host.addView(controls)
        button(host, "Copy BLE diagnostics") {
            (getSystemService(Context.CLIPBOARD_SERVICE) as ClipboardManager)
                .setPrimaryClip(ClipData.newPlainText("Bluetooth diagnostic", link.lastGattReport + "\n" + report.text))
        }
    }
    private fun button(parent: LinearLayout, title: String, action: () -> Unit) {
        parent.addView(Button(this).apply { text = title; isAllCaps = false; setOnClickListener { action() } })
    }
    private fun permitted(): Boolean {
        val required = if (Build.VERSION.SDK_INT >= 31)
            arrayOf(Manifest.permission.BLUETOOTH_SCAN, Manifest.permission.BLUETOOTH_CONNECT)
        else arrayOf(Manifest.permission.ACCESS_FINE_LOCATION)
        val missing = required.filter { checkSelfPermission(it) != PackageManager.PERMISSION_GRANTED }
        if (missing.isNotEmpty()) { requestPermissions(missing.toTypedArray(), 42); return false }
        return true
    }
    private fun scan() {
        link.disconnect()
        session = null
        meshGroups = emptyList()
        selectedDevice = null
        devices.removeAllViews()
        controls.removeAllViews()
        report.text = "Scanning… pair the radio from its menu if requested."
        link.scan({ c -> button(devices, c.name + " · " + c.rssi + " dBm") { connect(c.device) } },
            { error -> report.text = error })
    }
    private fun connect(device: BluetoothDevice) {
        if (busy) return
        session = null
        selectedDevice = device
        controls.removeAllViews()
        runTask("Connecting…") {
            val latch = CountDownLatch(1)
            var result: Result<String>? = null
            link.connect(device, family) { result = it; latch.countDown() }
            if (!latch.await(18, TimeUnit.SECONDS)) throw IOException("BLE discovery timeout")
            result?.getOrThrow() ?: throw IOException("No connection response")
            val next: DeviceSession = if (family == BluetoothLink.Family.MESHCORE) MeshCoreSession(link) else BenshiSession(link)
            val identity = next.identity()
            session = next
            runOnUiThread { if (!destroyed) renderControls(next) }
            identity
        }
    }
    private fun runTask(label: String, action: () -> String) {
        if (busy || destroyed) return
        busy = true
        report.text = label
        work.submit {
            try {
                val result = action()
                runOnUiThread { if (!destroyed) report.text = result }
            } catch (e: Exception) {
                link.disconnect()
                session = null
                runOnUiThread { if (!destroyed) {
                    report.text = "Operation failed: " + (e.message ?: e.javaClass.simpleName) + "\nReconnect before retrying."
                    controls.removeAllViews()
                } }
            } finally { busy = false }
        }
    }
    private fun renderControls(s: DeviceSession) {
        controls.removeAllViews()
        button(controls, "Refresh status") { runTask("Querying radio…") { s.status() } }
        when (s) {
            is BenshiSession -> {
                button(controls, "Battery voltage") { runTask("Reading battery…") { s.battery() } }
                button(controls, "Radio GPS position") { runTask("Reading GPS…") { s.position() } }
                button(controls, "Read active bank memories") {
                    runTask("Reading active bank…") {
                        val rows = s.activeBankMemories()
                        val file = File(filesDir, "benshi-memory-" + System.currentTimeMillis() + ".txt")
                        file.writeText(rows.joinToString("\n"))
                        "Saved active-bank memory summary: " + file.name + "\n" + rows.joinToString("\n")
                    }
                }
                button(controls, "Program master memories in active bank") {
                    runTask("Inspecting active radio bank…") {
                        val plan = s.planBank(FleetStore(this).channels())
                        val preview = plan.channels.joinToString("\n") { c ->
                            c.slot.toString() + " · " + c.name + " · " + c.receive +
                                (c.transmit?.let { " TX " + it } ?: " RX ONLY")
                        }
                        runOnUiThread {
                            if (!destroyed && plan.channels.isNotEmpty()) {
                                AlertDialog.Builder(this).setTitle("Write active bank " + (plan.region+1) + "?")
                                    .setMessage("Only the listed compatible master memories are updated. GMRS/FRS receive-only; DCS skipped. Original bank backed up.\n\n" + preview)
                                    .setNegativeButton("Cancel",null).setPositiveButton("Backup + write + verify") { _,_ ->
                                        if (session === s) runTask("Programming active bank…") {
                                            s.writeBank(plan) { records ->
                                                val file = File(filesDir,"benshi-bank-" + plan.region + "-" + System.currentTimeMillis() + ".bin")
                                                file.outputStream().use { out -> records.forEach { out.write(it) } }
                                            }
                                        }
                                    }.show()
                            }
                        }
                        if (plan.channels.isEmpty()) "No compatible master memories in active bank." else
                            "Review " + plan.channels.size + " memories for active bank " + (plan.region+1)
                    }
                }
                button(controls, "Read radio settings") { runTask("Reading settings…") { s.settingsSummary() } }
                button(controls, "Set squelch (backup + verify)") {
                    val field = EditText(this).apply {
                        inputType = android.text.InputType.TYPE_CLASS_NUMBER
                        hint = "0–9"
                    }
                    AlertDialog.Builder(this).setTitle("Set squelch").setView(field)
                        .setNegativeButton("Cancel",null)
                        .setPositiveButton("Write + verify") { _,_ ->
                            val n = field.text.toString().toIntOrNull()
                            if (n != null && n in 0..9) runTask("Updating settings…") {
                                s.setSquelch(n) { original ->
                                    File(filesDir,"benshi-settings-" + System.currentTimeMillis() + ".bin").writeBytes(original)
                                }
                            } else report.text = "Enter a squelch level 0–9."
                        }.show()
                }
                button(controls, "Listen for received RF data (15 seconds)") {
                    runTask("Listening for radio data…") {
                        s.subscribeData()
                        val until = System.nanoTime() + TimeUnit.SECONDS.toNanos(15)
                        val events = mutableListOf<String>()
                        while (!destroyed && System.nanoTime() < until) {
                            s.pollEvent(300)?.let { if (events.size < 100) events.add(it) }
                        }
                        events.joinToString("\n").ifBlank { "No RF data notifications received." }
                    }
                }
                button(controls, "KISS packet terminal (Classic Bluetooth)") {
                    if (!busy) startActivity(android.content.Intent(this, KissTerminalActivity::class.java))
                }
            }
            is MeshCoreSession -> {
                button(controls,"Read configured channels") { runTask("Reading channel names…") {
                    meshGroups = s.channels()
                    meshGroups.joinToString("\n").ifBlank { "No named channels found." }
                } }
                button(controls,"Sync queued messages") { runTask("Syncing messages…") {
                    val messages = s.syncMessages()
                    if (messages.isNotEmpty()) {
                        File(filesDir,"mesh-messages.txt").appendText(messages.joinToString("\n",postfix="\n"))
                    }
                    messages.joinToString("\n").ifBlank { "No queued messages." }
                } }
                button(controls,"Send channel message") {
                    if (busy) return@button
                    if (meshGroups.isEmpty()) { report.text = "Read configured channels first."; return@button }
                    val groups = meshGroups.toList()
                    AlertDialog.Builder(this).setTitle("Select channel").setItems(groups.map { it.toString() }.toTypedArray()) { _,i ->
                        val field = EditText(this).apply { hint = "Message (up to 120 UTF-8 bytes)" }
                        AlertDialog.Builder(this).setTitle("Send to " + groups[i].name).setView(field)
                            .setNegativeButton("Cancel",null).setPositiveButton("Send") { _,_ ->
                                val msg = field.text.toString()
                                runTask("Sending…") { s.send(groups[i].index,msg); "Accepted by node; over-air delivery not confirmed." }
                            }.show()
                    }.setNegativeButton("Cancel",null).show()
                }
            }
        }
    }
    override fun onDestroy() {
        destroyed = true
        link.disconnect()
        work.shutdownNow()
        super.onDestroy()
    }
}
