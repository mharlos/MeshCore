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
        section(host, "DISCOVER & TOOLS")
        addButton(host, "Frequency library — browse and add") { showLibrary() }
        addButton(host, "Nearby repeater finder") { findRepeaters() }
        addButton(host, "DTMF code builder") { dtmfBuilder() }
        section(host, "MASTER CHANNEL PLAN")
        for (channel in channels.sortedBy { it.slot }) {
            val rx = String.format(Locale.US, "%.5f", channel.receive)
            val tx = channel.transmit?.let { String.format(Locale.US, "%.5f", it) } ?: "RX ONLY"
            val tones = channel.txCtcss?.toString() ?: "OFF"
            addButton(host, String.format(Locale.US, "%03d", channel.slot) + "  " +
                channel.name + "  |  " + channel.service + "  " + channel.mode + "\nRX " + rx + "  TX " + tx + "  CT " + tones + (channel.txDcs?.let { " DCS " + it } ?: "")) {
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
        val captions = listOf("Memory slot (1–127 shared)", "Channel name (12 chars max; UV-5R uses 7)",
            "Receive MHz", "Transmit MHz (blank = receive only)",
            "Transmit CTCSS Hz (blank = OFF)", "Receive CTCSS Hz (blank = OFF)",
            "Modulation: FM, NFM or AM", "Power: High, Medium or Low", "TX DCS (blank = OFF)", "RX DCS (blank = OFF)", "DCS polarity (NN, NR, RN, RR)", "Service category", "Description")
        val initial = listOf(
            (channel?.slot ?: ((1..127).firstOrNull { n -> channels.none { it.slot == n } } ?: 128)).toString(),
            channel?.name ?: "", channel?.receive?.toString() ?: "",
            channel?.transmit?.toString() ?: "", channel?.txCtcss?.toString() ?: "",
            channel?.rxCtcss?.toString() ?: "", channel?.mode ?: "FM",
            channel?.power ?: "High", channel?.txDcs?.toString() ?: "", channel?.rxDcs?.toString() ?: "",
            channel?.dcsPolarity ?: "NN", channel?.service ?: "Custom", channel?.description ?: ""
        )
        val fields = captions.indices.map { index ->
            host.addView(label(captions[index], 12f, Color.DKGRAY))
            input(initial[index], index in listOf(0, 2, 3, 4, 5, 8, 9)).also { host.addView(it) }
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
                    require(slot in 1..127 || channel?.slot == 0 && slot == 0) { "Memory must be 1–127" }
                    require(channels.none { it.slot == slot && it.slot != channel?.slot }) { "Slot already occupied" }
                    val name = v[1].uppercase(Locale.US)
                    require(name.isNotBlank() && name.length <= 12) { "Name must be 1–12 characters" }
                    val receive = v[2].toDouble()
                    val transmit = v[3].takeIf { it.isNotBlank() }?.toDouble()
                    require(receive in 1.0..999.0 && (transmit == null || transmit in 1.0..999.0))
                    val mode = v[6].uppercase(Locale.US)
                    require(mode in listOf("FM", "NFM", "AM"))
                    require(FrequencyCatalog.txAllowed(receive, transmit, mode)) { "Use amateur TX, an exact GMRS simplex/repeater pair, or receive-only" }
                    val power = v[7].lowercase(Locale.US).replaceFirstChar { it.uppercase() }
                    require(power in listOf("Low","Medium","High"))
                    val txTone = v[4].takeIf { it.isNotBlank() }?.toDouble()
                    val rxTone = v[5].takeIf { it.isNotBlank() }?.toDouble()
                    val txDcs = v[8].takeIf { it.isNotBlank() }?.toInt()
                    val rxDcs = v[9].takeIf { it.isNotBlank() }?.toInt()
                    require(txTone == null || txDcs == null) { "Choose TX CTCSS or DCS" }
                    require(rxTone == null || rxDcs == null) { "Choose RX CTCSS or DCS" }
                    require(v[10] in listOf("NN","NR","RN","RR")) { "Invalid DCS polarity" }
                    val next = Channel(slot, name, receive, transmit, txTone, rxTone,
                        mode == "NFM", power, mode, txDcs, rxDcs, v[10], v[11], v[12])
                    channels.removeAll { it.slot == slot || (channel != null && it.slot == channel.slot) }
                    channels.add(next)
                    save()
                } catch (e: Exception) { message("Invalid memory", e.message ?: "Invalid fields") }
            }.show()
    }

    private fun showLibrary() {
        val keys = FrequencyCatalog.groups.keys.toList()
        AlertDialog.Builder(this).setTitle("US frequency library")
            .setItems(keys.toTypedArray()) { _, index ->
                val list = FrequencyCatalog.groups.getValue(keys[index])
                chooseCatalog(keys[index], list)
            }.setNegativeButton("Close", null).show()
    }

    private fun chooseCatalog(title: String, list: List<Channel>) {
        val selected = BooleanArray(list.size)
        val labels = list.map { c ->
            c.name + "  " + String.format(Locale.US, "%.5f", c.receive) + " " + c.mode +
                (if (c.transmit == null) " RX ONLY" else " TX " + c.transmit) + "\n" + c.description
        }.toTypedArray()
        if (list.isEmpty()) { message(title, "No matching entries."); return }
        fun add(items: List<Channel>) {
            val before = channels.size
            channels = FrequencyCatalog.append(channels, items).toMutableList()
            save()
            message(title, "Added " + (channels.size - before) + " memories. Existing slots preserved. " +
                "Duplicates, unsupported references and entries beyond available capacity skipped.")
        }
        AlertDialog.Builder(this).setTitle(title)
            .setMultiChoiceItems(labels, selected) { _, index, checked -> selected[index] = checked }
            .setPositiveButton("Add selected") { _, _ -> add(list.filterIndexed { i, _ -> selected[i] }) }
            .setNeutralButton("Add all") { _, _ -> add(list) }
            .setNegativeButton("Cancel", null).show()
    }

    private fun findRepeaters() {
        val host = stack()
        host.addView(label("City or ZIP (geocoded; no GPS permission required)", 13f, Color.DKGRAY))
        val location = input("Rainier, Oregon, USA")
        host.addView(location)
        host.addView(label("Radius miles", 13f, Color.DKGRAY))
        val radius = input("50", true)
        host.addView(radius)
        host.addView(label("Optional callsign filter", 13f, Color.DKGRAY))
        val filter = input("")
        host.addView(filter)
        AlertDialog.Builder(this).setTitle("Nearby repeaters — HearHam")
            .setView(host).setNegativeButton("Cancel", null)
            .setPositiveButton("Search") { _, _ ->
                val city = location.text.toString().trim()
                val miles = radius.text.toString().toDoubleOrNull()
                val query = filter.text.toString().trim()
                if (city.isBlank() || miles == null || miles !in 1.0..200.0) {
                    message("Invalid search", "Enter a city or ZIP and radius 1–200 miles.")
                } else {
                    val progress = android.app.ProgressDialog.show(this, "Repeater finder",
                        "Resolving location and loading directory…", true, false)
                    Thread {
                        try {
                            val result = RepeaterDirectory.search(this, city, miles, query)
                            runOnUiThread {
                                if (!isFinishing) { progress.dismiss(); chooseCatalog(result.first, result.second) }
                            }
                        } catch (e: Exception) {
                            runOnUiThread {
                                if (!isFinishing) { progress.dismiss(); message("Repeater search failed", e.message ?: "Unknown error") }
                            }
                        }
                    }.start()
                }
            }.show()
    }

    private fun dtmfBuilder() {
        val host = stack()
        val digits = input("")
        digits.filters = arrayOf(android.text.InputFilter.LengthFilter(32))
        host.addView(label("DTMF sequence (reference / radio keypad)", 13f, Color.DKGRAY))
        host.addView(digits)
        val keys = listOf("123A", "456B", "789C", "*0#D")
        for (line in keys) {
            val row = LinearLayout(this)
            for (key in line) {
                val button = Button(this).apply {
                    text = key.toString()
                    setOnClickListener { if (digits.length() < 32) digits.append(key.toString()) }
                }
                row.addView(button, LinearLayout.LayoutParams(0, -2, 1f))
            }
            host.addView(row)
        }
        AlertDialog.Builder(this).setTitle("DTMF code builder").setView(host)
            .setNeutralButton("Clear") { _, _ -> dtmfBuilder() }
            .setPositiveButton("Copy") { _, _ ->
                val text = digits.text.toString().uppercase(Locale.US)
                if (text.all { it in "0123456789ABCD*#" }) {
                    (getSystemService(CLIPBOARD_SERVICE) as android.content.ClipboardManager)
                        .setPrimaryClip(android.content.ClipData.newPlainText("DTMF", text))
                } else message("Invalid DTMF", "Use 0–9, A–D, * or #.")
            }.setNegativeButton("Close", null).show()
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
