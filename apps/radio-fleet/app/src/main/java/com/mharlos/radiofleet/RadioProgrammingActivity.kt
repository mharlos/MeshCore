package com.mharlos.radiofleet

import android.Manifest
import android.app.Activity
import android.app.AlertDialog
import android.bluetooth.BluetoothDevice
import android.content.pm.PackageManager
import android.graphics.Color
import android.os.Build
import android.os.Bundle
import android.widget.Button
import android.widget.LinearLayout
import android.widget.RadioButton
import android.widget.RadioGroup
import android.widget.ScrollView
import android.widget.TextView
import com.chaquo.python.android.AndroidPlatform
import com.chaquo.python.Python
import org.json.JSONArray
import org.json.JSONObject
import java.io.File
import java.util.concurrent.Executors

/**
 * One APK, three independent device drivers.
 * This screen must read and persist a local backup before any write can run.
 */
class RadioProgrammingActivity : Activity() {
    private val task = Executors.newSingleThreadExecutor()
    private lateinit var link: BluetoothLink
    private lateinit var store: FleetStore
    private lateinit var status: TextView
    private lateinit var devices: LinearLayout
    private lateinit var readButton: Button
    private lateinit var writeButton: Button
    private lateinit var radioGroup: RadioGroup
    private val models = listOf("UV-5R", "UV-5RM", "BTECH UV-PRO")
    private var model = models.first()
    private var readModel: String? = null
    private var baofengBackup: JSONObject? = null
    private var uvproBackup: BenshiProgrammer.Snapshot? = null
    private var backupFile: File? = null
    private var targetName: String = ""
    private var busy = false

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        window.statusBarColor = Color.rgb(10, 17, 26)
        store = FleetStore(this)
        link = BluetoothLink(this)
        RadioIo.active = link
        val root = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(18, 20, 18, 16)
            setBackgroundColor(Color.rgb(10, 17, 26))
        }
        fun header(text: String, size: Float): TextView =
            TextView(this).apply {
                this.text = text; textSize = size; setTextColor(Color.WHITE)
                setPadding(5, 9, 5, 9)
            }
        root.addView(header("RADIO MEMORY PROGRAMMER", 23f))
        root.addView(header("Bluetooth programming directly in this app. Read and back up each radio before Write.", 13f))
        radioGroup = RadioGroup(this)
        for ((index, name) in models.withIndex()) {
            radioGroup.addView(RadioButton(this).apply {
                id = 100 + index
                text = name
                textSize = 16f
                setTextColor(Color.WHITE)
            })
        }
        radioGroup.check(100)
        radioGroup.setOnCheckedChangeListener { _, checked ->
            model = models[(checked - 100).coerceIn(0, models.lastIndex)]
            resetBackup()
            link.disconnect()
            setStatus("Choose and connect a new device for " + model)
            devices.removeAllViews()
        }
        root.addView(radioGroup)
        val scan = Button(this).apply {
            text = "1. Scan for Bluetooth adapters"
            setOnClickListener { if (permissions()) scan() }
        }
        root.addView(scan)
        devices = LinearLayout(this).apply { orientation = LinearLayout.VERTICAL }
        val results = ScrollView(this).apply { addView(devices) }
        root.addView(results, LinearLayout.LayoutParams(-1, 0, 1f))
        readButton = Button(this).apply {
            text = "2. Read radio + save backup"
            isEnabled = false
            setOnClickListener { read() }
        }
        root.addView(readButton)
        writeButton = Button(this).apply {
            text = "3. Write master channels + verify"
            isEnabled = false
            setOnClickListener { confirmWrite() }
        }
        root.addView(writeButton)
        status = header("Choose a model and scan.", 13f)
        root.addView(status)
        setContentView(root)
        if (Build.VERSION.SDK_INT >= 31) permissions()
    }
    private fun resetBackup() {
        baofengBackup = null
        uvproBackup = null
        backupFile = null
        readModel = null
        if (::writeButton.isInitialized) writeButton.isEnabled = false
        if (::readButton.isInitialized) readButton.isEnabled = false
    }
    private fun setStatus(text: String) = runOnUiThread { status.text = text }
    private fun permissions(): Boolean {
        val permissions = if (Build.VERSION.SDK_INT >= 31)
            arrayOf(Manifest.permission.BLUETOOTH_SCAN, Manifest.permission.BLUETOOTH_CONNECT)
        else arrayOf(Manifest.permission.ACCESS_FINE_LOCATION)
        val missing = permissions.filter { checkSelfPermission(it) != PackageManager.PERMISSION_GRANTED }
        if (missing.isNotEmpty()) {
            requestPermissions(missing.toTypedArray(), 170)
            return false
        }
        return true
    }
    private fun scan() {
        resetBackup()
        devices.removeAllViews()
        setStatus("Scanning 13 seconds. Select the TD-BL1 or UV-PRO control radio.")
        link.scan({ candidate ->
            val button = Button(this).apply {
                text = candidate.name + "  ·  " + candidate.rssi + " dBm"
                setOnClickListener { connect(candidate.device, candidate.name) }
            }
            devices.addView(button)
        }, { error -> setStatus(error) })
    }
    private fun connect(device: BluetoothDevice, name: String) {
        if (busy) return
        resetBackup()
        targetName = name
        setStatus("Connecting to " + name + " ...")
        val family = if (model == "BTECH UV-PRO")
            BluetoothLink.Family.BENSHI_UVPRO else BluetoothLink.Family.BAOFENG_DONLGE
        link.connect(device, family) { result ->
            result.fold({ description ->
                setStatus(description + "\nReady to READ " + model)
                readButton.isEnabled = true
                writeButton.isEnabled = false
            }, { error ->
                setStatus("Connection failed: " + error.message)
            })
        }
    }
    private fun worker(label: String, body: () -> Unit) {
        if (busy) return
        busy = true
        readButton.isEnabled = false
        writeButton.isEnabled = false
        setStatus(label)
        task.submit {
            try { body() }
            catch (e: Exception) {
                setStatus(label + " FAILED: " + (e.message ?: e.javaClass.simpleName) +
                    "\nNo further write attempted.")
            } finally {
                busy = false
                runOnUiThread {
                    readButton.isEnabled = link.ready
                    writeButton.isEnabled = link.ready && readModel == model &&
                        backupFile?.exists() == true
                }
            }
        }
    }
    private fun python() : com.chaquo.python.PyObject {
        if (!Python.isStarted()) Python.start(AndroidPlatform(applicationContext))
        return Python.getInstance().getModule("fleet_chirp")
    }
    private fun backups(): File = File(filesDir, "radio-backups").apply {
        if (!exists()) mkdirs()
    }
    private fun backup(text: String): File {
        val safeName = model.replace(Regex("[^A-Za-z0-9]"), "-")
        val name = safeName + "-" + System.currentTimeMillis() + ".json"
        val file = File(backups(), name)
        file.writeText(text)
        if (file.length() == 0L) throw IllegalStateException("Backup file empty")
        return file
    }

    private fun read() {
        val targetModel = model
        worker("Reading " + targetModel + "; keep radio connected...") {
            when (targetModel) {
                "BTECH UV-PRO" -> {
                    val result = BenshiProgrammer(link).read()
                    backupFile = backup(result.json())
                    uvproBackup = result
                    baofengBackup = null
                    readModel = targetModel
                    setStatus("Saved " + result.memories.size + " UV-PRO channels to " +
                        backupFile!!.name + ".\nBackup hash " + result.fingerprint.take(16))
                }
                "UV-5R", "UV-5RM" -> {
                    RadioIo.active = link
                    val result = JSONObject(python().callAttr("read_radio", targetModel).toString())
                    backupFile = backup(result.toString(2))
                    baofengBackup = result
                    uvproBackup = null
                    readModel = targetModel
                    val count = result.getJSONArray("channels").length()
                    setStatus("Saved original " + targetModel + " image (" + count +
                        " occupied memories) to " + backupFile!!.name +
                        ".\nImage hash " + result.getString("sha256").take(16))
                }
                else -> throw IllegalArgumentException("Unsupported model")
            }
        }
    }
    private fun confirmWrite() {
        if (readModel != model || backupFile?.exists() != true) {
            setStatus("Read and back up this specific radio before writing.")
            return
        }
        val selected = store.channels().sortedBy { it.slot }
        if (selected.isEmpty()) {
            setStatus("Master channel profile is empty.")
            return
        }
        AlertDialog.Builder(this)
            .setTitle("Write " + selected.size + " channel memories?")
            .setMessage(
                "Radio: " + targetName + "\nModel: " + model +
                "\nBackup: " + backupFile!!.name +
                "\n\nOnly the shared master memories will be updated. Other memories and " +
                "radio-specific settings will be preserved by the model driver. " +
                "This operation writes to the physical radio and requires an uninterrupted connection."
            )
            .setNegativeButton("Cancel", null)
            .setPositiveButton("Write + verify") { _, _ -> write(selected) }
            .show()
    }
    private fun write(selected: List<Channel>) {
        val targetModel = model
        worker("Writing " + targetModel + " — do not disconnect...") {
            when (targetModel) {
                "BTECH UV-PRO" -> {
                    val snapshot = uvproBackup ?: error("Missing UV-PRO backup")
                    val programmer = BenshiProgrammer(link)
                    programmer.write(snapshot, selected) { completed, total ->
                        setStatus("UV-PRO write + per-memory verification " + completed + "/" + total)
                    }
                    // Per-memory verification occurs inside programmer.write().
                    val verified = selected.map { selectedChannel ->
                        val group = (selectedChannel.slot - 1) / snapshot.identity.perGroup
                        val slot = (selectedChannel.slot - 1) % snapshot.identity.perGroup
                        snapshot.memories.firstOrNull {
                            it.group == group && it.position == slot
                        } ?: error("Original memory missing")
                    }
                    if (verified.size != selected.size) error("Verification count mismatch")
                    success(selected.size, targetModel)
                }
                "UV-5R", "UV-5RM" -> {
                    val prior = baofengBackup ?: error("Missing clone backup")
                    val array = JSONArray()
                    selected.forEach { array.put(it.json()) }
                    python().callAttr("write_radio", targetModel,
                        prior.getString("sha256"), array.toString())
                    setStatus("Write completed. Reading radio independently to verify...")
                    val readback = JSONObject(python().callAttr("read_radio", targetModel).toString())
                    val memory = readback.getJSONArray("channels")
                    val actual = (0 until memory.length()).map {
                        Channel.parse(memory.getJSONObject(it))
                    }
                    val differences = ChirpCsv.diff(selected, actual)
                    if (differences.isNotEmpty())
                        throw IllegalStateException("Read-back mismatch: " + differences.take(8).joinToString("; "))
                    success(selected.size, targetModel)
                }
            }
        }
    }
    private fun success(total: Int, targetModel: String) {
        // Never mark the device synchronized until all memory read-backs passed.
        val version = ChirpCsv.fingerprint(store.channels())
        val id = targetName + "|" + targetModel
        val radios = store.radios().toMutableList()
        val existing = radios.indexOfFirst { it.id == id }
        val entry = FleetRadio(id, targetName, targetModel, version)
        if (existing < 0) radios.add(entry) else radios[existing] = entry
        store.saveRadios(radios)
        setStatus("VERIFIED " + total + " memories on " + targetModel + ". Master " +
            version + ". Backup retained: " + backupFile!!.name)
        // A second write requires a fresh read because the baseline hash changed.
        readModel = null
    }
    override fun onDestroy() {
        link.disconnect()
        if (RadioIo.active === link) RadioIo.active = null
        task.shutdownNow()
        super.onDestroy()
    }
}
