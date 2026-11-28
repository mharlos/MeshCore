package com.mharlos.radiofleet

import android.Manifest
import android.app.Activity
import android.bluetooth.BluetoothDevice
import android.bluetooth.BluetoothGatt
import android.bluetooth.BluetoothGattCallback
import android.bluetooth.BluetoothManager
import android.bluetooth.BluetoothProfile
import android.bluetooth.le.ScanCallback
import android.bluetooth.le.ScanResult
import android.content.ClipData
import android.content.ClipboardManager
import android.content.Context
import android.content.pm.PackageManager
import android.graphics.Color
import android.os.Build
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.widget.Button
import android.widget.LinearLayout
import android.widget.ScrollView
import android.widget.TextView

/** Read-only BLE inspection for identifying the TD-BL1 UART adapter, not a radio writer. */
class BleInspectorActivity : Activity() {
    private lateinit var output: TextView
    private lateinit var found: LinearLayout
    private var scanCallback: ScanCallback? = null
    private var gatt: BluetoothGatt? = null
    private val addresses = mutableSetOf<String>()
    private var report = "Farm Radio Fleet TD-BL1 GATT inspection\n"
    private val handler = Handler(Looper.getMainLooper())

    override fun onCreate(state: Bundle?) {
        super.onCreate(state)
        window.statusBarColor=Deck.bg
        window.navigationBarColor=Deck.bg
        val root=Deck.stack(this,12).apply { setBackgroundColor(Deck.bg) }
        Deck.add(root,Deck.header(this,"Bluetooth diagnostics"))
        val panel=Deck.card(this)
        Deck.heading(panel,"Read-only GATT inspection")
        Deck.add(panel,Deck.text(this,"Discover Bluetooth services and characteristic properties. No radio memories changed.",13f,Deck.muted))
        Deck.action(panel,"Scan for adapters",Deck.cyan){scan()}
        Deck.action(panel,"Copy diagnostic report",Deck.amber){
            val cm=getSystemService(CLIPBOARD_SERVICE) as ClipboardManager
            cm.setPrimaryClip(ClipData.newPlainText("TD-BL1 GATT",report))
        }
        found=Deck.stack(this)
        Deck.add(panel,found,0)
        Deck.add(root,panel)
        val terminal=Deck.card(this,Deck.lime)
        Deck.heading(terminal,"Diagnostic output",Deck.lime)
        output=Deck.text(this,"Turn on Bluetooth and the TD-BL1, then scan.",12f,Deck.lime,true).apply { setTextIsSelectable(true) }
        Deck.add(terminal,output,0);Deck.add(root,terminal)
        setContentView(ScrollView(this).apply { setBackgroundColor(Deck.bg);addView(root) })
    }
    private fun append(line: String) {
        report += line + "\n"
        output.text = report.takeLast(18000)
    }
    private fun permitted(): Boolean {
        val required = if (Build.VERSION.SDK_INT >= 31)
            arrayOf(Manifest.permission.BLUETOOTH_SCAN, Manifest.permission.BLUETOOTH_CONNECT)
        else arrayOf(Manifest.permission.ACCESS_FINE_LOCATION)
        val missing = required.filter { checkSelfPermission(it) != PackageManager.PERMISSION_GRANTED }
        if (missing.isNotEmpty()) {
            requestPermissions(missing.toTypedArray(), 501)
            return false
        }
        return true
    }
    override fun onRequestPermissionsResult(
        requestCode: Int, permissions: Array<out String>, grantResults: IntArray
    ) {
        super.onRequestPermissionsResult(requestCode, permissions, grantResults)
        if (requestCode == 501 && grantResults.isNotEmpty() &&
            grantResults.all { it == PackageManager.PERMISSION_GRANTED }) scan()
    }
    private fun scan() {
        if (!permitted()) return
        stopScan()
        addresses.clear()
        found.removeAllViews()
        append("Scanning all BLE advertisements; tap TD-BL1 when visible.")
        try {
            val adapter = (getSystemService(Context.BLUETOOTH_SERVICE) as BluetoothManager).adapter
            if (adapter == null || !adapter.isEnabled) {
                append("Enable Bluetooth before scanning.")
                return
            }
            val scanner = adapter.bluetoothLeScanner ?: run {
                append("BLE scanner not available.")
                return
            }
            val callback = object : ScanCallback() {
                override fun onScanResult(callbackType: Int, result: ScanResult) {
                    runOnUiThread {
                        val device = result.device
                        val address = device.address
                        if (!addresses.add(address)) return@runOnUiThread
                        val title = try { device.name ?: result.scanRecord?.deviceName ?: "Unnamed BLE" }
                            catch (_: SecurityException) { "Unnamed BLE" }
                        found.addView(Button(this@BleInspectorActivity).apply {
                            text = title + "  (" + address.takeLast(5) + ")"
                            Deck.styleButton(this,Deck.cyan)
                            setOnClickListener { connect(device, title) }
                        })
                    }
                }
                override fun onScanFailed(errorCode: Int) {
                    runOnUiThread { append("Scan failed. Android code " + errorCode) }
                }
            }
            scanCallback = callback
            scanner.startScan(callback)
            handler.postDelayed({ stopScan() }, 12000L)
        } catch (e: SecurityException) { append("Bluetooth permission: " + e.message) }
    }
    private fun stopScan() {
        val callback = scanCallback ?: return
        scanCallback = null
        try {
            (getSystemService(Context.BLUETOOTH_SERVICE) as BluetoothManager)
                .adapter?.bluetoothLeScanner?.stopScan(callback)
        } catch (_: SecurityException) { }
    }
    private fun connect(device: BluetoothDevice, title: String) {
        stopScan()
        try {
            gatt?.close()
            append("Connecting to " + title + " ...")
            gatt = device.connectGatt(this, false, object : BluetoothGattCallback() {
                override fun onConnectionStateChange(
                    g: BluetoothGatt, status: Int, newState: Int
                ) {
                    if (status == BluetoothGatt.GATT_SUCCESS &&
                        newState == BluetoothProfile.STATE_CONNECTED) {
                        try { g.discoverServices() }
                        catch (e: SecurityException) { runOnUiThread { append("Permission error") } }
                    } else {
                        runOnUiThread { append("Connection status " + status + " / state " + newState) }
                    }
                }
                override fun onServicesDiscovered(g: BluetoothGatt, status: Int) {
                    runOnUiThread {
                        append("Services discovered: " + status)
                        if (status == BluetoothGatt.GATT_SUCCESS) {
                            for (service in g.services) {
                                report += "SERVICE " + service.uuid + "\n"
                                for (ch in service.characteristics) {
                                    val props = ch.properties
                                    val propsText = listOfNotNull(
                                        if (props and 0x02 != 0) "READ" else null,
                                        if (props and 0x08 != 0) "WRITE" else null,
                                        if (props and 0x04 != 0) "WRITE_NO_RESPONSE" else null,
                                        if (props and 0x10 != 0) "NOTIFY" else null,
                                        if (props and 0x20 != 0) "INDICATE" else null
                                    ).joinToString(",")
                                    report += "  CHAR " + ch.uuid + "  " + propsText + "\n"
                                }
                            }
                            output.text = report.takeLast(2700)
                        }
                    }
                }
            }, BluetoothDevice.TRANSPORT_LE)
        } catch (e: SecurityException) { append("Connection permission: " + e.message) }
    }
    override fun onDestroy() {
        stopScan()
        gatt?.close()
        super.onDestroy()
    }
}
