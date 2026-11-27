package com.mharlos.radiofleet

import android.app.Activity
import android.app.AlertDialog
import android.content.Intent
import android.graphics.Color
import android.net.Uri
import android.os.Bundle
import android.text.InputType
import android.view.View
import android.widget.Button
import android.widget.EditText
import android.widget.LinearLayout
import android.widget.ScrollView
import android.widget.TextView
import java.util.Locale

/**
 * Local-first fleet manager with a native BLE programming workflow.
 * Physical radio writes require a local backup, exact model identification,
 * and per-memory read-back verification.
 */
class MainActivity : Activity() {
    private lateinit var store: FleetStore
    private var channels = mutableListOf<Channel>()
    private var radios = mutableListOf<FleetRadio>()
    private var verifyRadioIndex = -1
    private val bg = Color.rgb(10, 17, 26)
    private val surface = Color.rgb(20, 33, 45)
    private val green = Color.rgb(96, 227, 172)
    private val foreground = Color.rgb(230, 239, 242)
    private val muted = Color.rgb(163, 184, 193)

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        store = FleetStore(this)
        channels = store.channels().toMutableList()
        radios = store.radios().toMutableList()
        window.statusBarColor = bg
        window.navigationBarColor = bg
        render()
    }
    private fun label(text: String, size: Float = 15f, color: Int = foreground): TextView =
        TextView(this).apply {
            this.text = text
            textSize = size
            setTextColor(color)
            setPadding(4, 7, 4, 7)
        }
    private fun stack(): LinearLayout = LinearLayout(this).apply {
        orientation = LinearLayout.VERTICAL
        setPadding(16, 12, 16, 12)
    }
    private fun addButton(host: LinearLayout, text: String, handler: () -> Unit) {
        val btn = Button(this).apply {
            this.text = text
            isAllCaps = false
            setTextColor(Color.WHITE)
            setBackgroundColor(surface)
            setOnClickListener { handler() }
        }
        val lp = LinearLayout.LayoutParams(-1, -2)
        lp.bottomMargin = 6
        host.addView(btn, lp)
    }
    private fun section(host: LinearLayout, title: String) {
        host.addView(label(title, 19f, green))
    }
    private fun render() {
        val host = stack()
        val scroll = ScrollView(this).apply { setBackgroundColor(bg); addView(host) }
        setContentView(scroll)
        host.addView(label("FARM RADIO FLEET", 26f, green))
        host.addView(label(
            "One master channel list across UV-5R, UV-5RM and BTECH UV-PRO.",
            13f, muted
        ))
        val currentHash = ChirpCsv.fingerprint(channels)
        host.addView(label("Master revision " + currentHash + "  •  " + channels.size + " memories", 13f, muted))
        section(host, "MASTER CHANNEL PLAN")
        for (channel in channels.sortedBy { it.slot }) {
            val rx = String.format(Locale.US, "%.5f", channel.receive)
            val tx = channel.transmit?.let { String.format(Locale.US, "%.5f", it) } ?: "RX ONLY"
            val tones = channel.txCtcss?.toString() ?: "OFF"
            addButton(host, String.format(Locale.US, "%03d", channel.slot) + "  " +
                channel.name + "  |  RX " + rx + "  TX " + tx + "  CT " + tones) {
                edit(channel)
            }
        }
        addButton(host, "+ Add / edit a channel") { edit(null) }
        addButton(host, "Export CHIRP CSV master") { exportCsv() }
        addButton(host, "Import CHIRP CSV into master") { chooseCsv(REQUEST_IMPORT) }
        section(host, "RADIO INVENTORY")
        if (radios.isEmpty()) host.addView(label("No radios registered yet.", 14f, muted))
        for (radio in radios) {
            val synced = radio.verifiedHash == currentHash
            addButton(host, radio.label + "  (" + radio.model + ")  •  " +
                (if (synced) "READ-BACK VERIFIED" else "NOT VERIFIED / CHANGED")) {
                radioActions(radio)
            }
        }
        addButton(host, "+ Add a radio") { addRadio() }
        addButton(host, "Compare a radio read-back CSV") { pickVerifyRadio() }
        section(host, "PROGRAMMING")
        addButton(host, "READ / WRITE RADIOS OVER BLUETOOTH") {
            startActivity(Intent(this, RadioProgrammingActivity::class.java))
        }
        addButton(host, "Program a radio via CHIRP / RadioDroid") { showProgrammingFlow() }
        addButton(host, "Inspect TD-BL1 Bluetooth (read-only)") {
            startActivity(Intent(this, BleInspectorActivity::class.java))
        }
        host.addView(label(
            "This app includes a direct Bluetooth programming screen with read-back verification.",
            13f, muted
        ))
        host.addView(label(
            "Repeater values are starter entries—confirm locally. Channel 5 defaults to receive-only on a UV-5R.",
            12f, muted
        ))
    }

    private fun input(value: String, numeric: Boolean = false): EditText = EditText(this).apply {
        setSingleLine(true)
        setText(value)
        textSize = 15f
        setTextColor(Color.BLACK)
        if (numeric) inputType = InputType.TYPE_CLASS_NUMBER or
            InputType.TYPE_NUMBER_FLAG_DECIMAL or InputType.TYPE_NUMBER_FLAG_SIGNED
        setSelectAllOnFocus(true)
    }

    private fun edit(channel: Channel?) {
        val host = stack()
        val captions = listOf("Memory slot (0–127)", "Channel name (7 chars max)",
            "Receive MHz", "Transmit MHz (blank = receive only)",
            "Transmit CTCSS Hz (blank = OFF)", "Receive CTCSS Hz (blank = OFF)",
            "Bandwidth: FM or NFM", "Power: High or Low")
        val initial = listOf(
            (channel?.slot ?: ((channels.maxOfOrNull { it.slot } ?: 0) + 1)).toString(),
            channel?.name ?: "", channel?.receive?.toString() ?: "",
            channel?.transmit?.toString() ?: "", channel?.txCtcss?.toString() ?: "",
            channel?.rxCtcss?.toString() ?: "", if (channel?.narrow == true) "NFM" else "FM",
            channel?.power ?: "High"
        )
        val fields = captions.indices.map { index ->
            host.addView(label(captions[index], 12f, Color.DKGRAY))
            input(initial[index], index in listOf(0, 2, 3, 4, 5)).also { host.addView(it) }
        }
        val scroll = ScrollView(this).apply { addView(host) }
        AlertDialog.Builder(this).setTitle("Edit memory").setView(scroll)
            .setNegativeButton("Cancel", null)
            .setNeutralButton("Delete") { _, _ ->
                if (channel != null) { channels.removeAll { it.slot == channel.slot }; save() }
            }
            .setPositiveButton("Save") { _, _ ->
                try {
                    val v = fields.map { it.text.toString().trim() }
                    val slot = v[0].toInt()
                    require(slot in 0..127) { "Memory must be 0–127" }
                    val name = v[1].uppercase(Locale.US)
                    require(name.isNotBlank() && name.length <= 7) { "Name must be 1–7 characters" }
                    val receive = v[2].toDouble()
                    val transmit = v[3].takeIf { it.isNotBlank() }?.toDouble()
                    require(receive in 1.0..999.0 && (transmit == null || transmit in 1.0..999.0))
                    val narrow = v[6].uppercase(Locale.US) == "NFM"
                    val next = Channel(slot, name, receive, transmit,
                        v[4].takeIf { it.isNotBlank() }?.toDouble(),
                        v[5].takeIf { it.isNotBlank() }?.toDouble(),
                        narrow, if (v[7].equals("low", true)) "Low" else "High")
                    channels.removeAll { it.slot == slot || (channel != null && it.slot == channel.slot) }
                    channels.add(next)
                    save()
                } catch (e: Exception) { message("Invalid memory", e.message ?: "Invalid fields") }
            }.show()
    }

    private fun addRadio() {
        val host = stack()
        host.addView(label("Friendly name", 13f, Color.DKGRAY))
        val name = input("UV-5RM field radio")
        host.addView(name)
        host.addView(label("Model / CHIRP driver", 13f, Color.DKGRAY))
        val model = input("Baofeng UV-5RM")
        host.addView(model)
        AlertDialog.Builder(this).setTitle("Register radio").setView(host)
            .setNegativeButton("Cancel", null)
            .setPositiveButton("Add") { _, _ ->
                val id = System.currentTimeMillis().toString()
                radios.add(FleetRadio(id, name.text.toString().trim(), model.text.toString().trim()))
                store.saveRadios(radios)
                render()
            }.show()
    }
    private fun radioActions(radio: FleetRadio) {
        AlertDialog.Builder(this).setTitle(radio.label)
            .setMessage("Model: " + radio.model + "\nMaster: " + ChirpCsv.fingerprint(channels) +
                "\nStatus: " + if (radio.verifiedHash == ChirpCsv.fingerprint(channels))
                    "Verified by an imported read-back" else "Unverified or outdated")
            .setPositiveButton("Verify CSV") { _, _ ->
                verifyRadioIndex = radios.indexOfFirst { it.id == radio.id }
                chooseCsv(REQUEST_VERIFY)
            }
            .setNeutralButton("Remove") { _, _ ->
                radios.removeAll { it.id == radio.id }
                store.saveRadios(radios)
                render()
            }
            .setNegativeButton("Close", null).show()
    }
    private fun save() { store.saveChannels(channels); render() }
    private fun exportCsv() {
        val intent = Intent(Intent.ACTION_CREATE_DOCUMENT).apply {
            addCategory(Intent.CATEGORY_OPENABLE)
            type = "text/csv"
            putExtra(Intent.EXTRA_TITLE, "farm-radio-master.csv")
        }
        startActivityForResult(intent, REQUEST_EXPORT)
    }
    private fun chooseCsv(request: Int) {
        val intent = Intent(Intent.ACTION_OPEN_DOCUMENT).apply {
            addCategory(Intent.CATEGORY_OPENABLE)
            type = "*/*"
            putExtra(Intent.EXTRA_MIME_TYPES, arrayOf("text/csv", "text/plain",
                "application/csv", "application/octet-stream"))
        }
        startActivityForResult(intent, request)
    }
    private fun pickVerifyRadio() {
        if (radios.isEmpty()) {
            message("Register a radio first", "Add its model and a friendly label, then import its read-back CSV.")
            return
        }
        AlertDialog.Builder(this).setTitle("Select the radio to verify")
            .setItems(radios.map { it.label + " • " + it.model }.toTypedArray()) { _, selected ->
                verifyRadioIndex = selected
                chooseCsv(REQUEST_VERIFY)
            }.show()
    }
    override fun onActivityResult(requestCode: Int, resultCode: Int, data: Intent?) {
        super.onActivityResult(requestCode, resultCode, data)
        val uri = data?.data ?: return
        if (resultCode != RESULT_OK) return
        try {
            if (requestCode == REQUEST_EXPORT) {
                contentResolver.openOutputStream(uri)?.use {
                    it.write(ChirpCsv.export(channels).toByteArray(Charsets.UTF_8))
                } ?: error("Could not open the export destination")
                message("Master exported", "CHIRP CSV saved. Load this CSV into a fresh read of each radio using its own model driver.")
            } else {
                val csv = contentResolver.openInputStream(uri)?.bufferedReader()?.use { it.readText() }
                    ?: error("Could not read the selected CSV")
                val parsed = ChirpCsv.parse(csv)
                if (requestCode == REQUEST_IMPORT) {
                    AlertDialog.Builder(this).setTitle("Replace master channel list?")
                        .setMessage("Replace " + channels.size + " memories with " + parsed.size +
                            " imported memories? This updates the master revision and marks earlier verifications stale.")
                        .setNegativeButton("Cancel", null)
                        .setPositiveButton("Replace") { _, _ ->
                            channels = parsed.toMutableList(); save()
                        }.show()
                } else if (requestCode == REQUEST_VERIFY) {
                    require(verifyRadioIndex in radios.indices) { "Select a radio before verifying" }
                    val discrepancies = ChirpCsv.diff(channels, parsed)
                    if (discrepancies.isEmpty()) {
                        val current = radios[verifyRadioIndex]
                        radios[verifyRadioIndex] = current.copy(verifiedHash = ChirpCsv.fingerprint(channels))
                        store.saveRadios(radios)
                        render()
                        message("Read-back verified", current.label + " matches every channel in the master.")
                    } else message("Differences found", discrepancies.take(15).joinToString("\n"))
                }
            }
        } catch (e: Exception) { message("File operation failed", e.message ?: "Unknown error") }
    }
    private fun showProgrammingFlow() {
        AlertDialog.Builder(this).setTitle("Program one radio")
            .setMessage(
                "1. Export the master CHIRP CSV here.\n" +
                "2. Open RadioDroid and select this radio's exact model.\n" +
                "3. Read the physical radio and save its original backup.\n" +
                "4. Clear the target memories in the programmer after backing them up; import into selected empty slots. Confirm every destination slot, tone and TX inhibit.\n" +
                "5. Write to the radio and wait for completion.\n" +
                "6. Read the radio again, export its CHIRP CSV, then return here to Verify.\n\n" +
                "Never write a raw UV-5R image into a UV-5RM."
            )
            .setNegativeButton("Close", null)
            .setPositiveButton("Open programmer") { _, _ ->
                val launch = packageManager.getLaunchIntentForPackage("com.radiodroid.app")
                if (launch != null) startActivity(launch)
                else startActivity(Intent(Intent.ACTION_VIEW,
                    Uri.parse("https://github.com/jnyer27/RadioDroid/releases")))
            }.show()
    }
    private fun message(title: String, body: String) {
        AlertDialog.Builder(this).setTitle(title).setMessage(body)
            .setPositiveButton("OK", null).show()
    }
    companion object {
        private const val REQUEST_EXPORT = 101
        private const val REQUEST_IMPORT = 102
        private const val REQUEST_VERIFY = 103
    }
}
