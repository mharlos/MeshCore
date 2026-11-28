package com.mharlos.radiofleet

import android.Manifest
import android.app.Activity
import android.app.AlertDialog
import android.bluetooth.BluetoothDevice
import android.content.ClipData
import android.content.ClipboardManager
import android.content.Context
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
import java.util.concurrent.CountDownLatch
import java.util.concurrent.TimeUnit
import java.io.IOException

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
    private lateinit var settingsButton: Button
    private lateinit var memoryButton: Button
    private lateinit var radioGroup: RadioGroup
    private val models = listOf("UV-5R", "UV-5RM")
    private var model = models.first()
    private var readModel: String? = null
    private var baofengBackup: JSONObject? = null
    private var uvproBackup: BenshiProgrammer.Snapshot? = null
    private var backupFile: File? = null
    private var targetName: String = ""
    @Volatile private var busy = false
    private class VerificationFailure(cause: Exception) : IllegalStateException(
        "Upload returned successfully, but read-back verification failed: " +
            (cause.message ?: cause.javaClass.simpleName), cause
    )
    @Volatile private var lastFailureReport: String = ""

    private lateinit var targetStatus: TextView
    private lateinit var backupStatus: TextView
    private lateinit var plan: LinearLayout
    private lateinit var scanButton: Button
    private val events = ArrayDeque<String>()
    private var lastEvent = ""
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        window.statusBarColor=Deck.bg
        window.navigationBarColor=Deck.bg
        store=FleetStore(this)
        link=BluetoothLink(this)
        RadioIo.active=link
        val root=Deck.stack(this,12).apply { setBackgroundColor(Deck.bg) }
        Deck.add(root,Deck.header(this,"Program radio"))
        val target=Deck.card(this)
        Deck.heading(target,"Target radio")
        radioGroup=RadioGroup(this).apply { orientation=RadioGroup.HORIZONTAL }
        for((index,name) in models.withIndex()) {
            radioGroup.addView(RadioButton(this).apply {
                id=100+index; text=name; textSize=15f; setTextColor(Deck.ink)
                buttonTintList=android.content.res.ColorStateList.valueOf(Deck.cyan)
                minimumHeight=Deck.dp(this@RadioProgrammingActivity,48)
            },RadioGroup.LayoutParams(0,-2,1f))
        }
        radioGroup.check(100)
        Deck.add(target,radioGroup)
        targetStatus=Deck.text(this,"NO DEVICE SELECTED",13f,Deck.muted,true)
        Deck.add(target,targetStatus,0)
        Deck.add(root,target)
        val workflow=Deck.card(this)
        Deck.heading(workflow,"Program workflow")
        val actions=Deck.row(this)
        scanButton=Deck.button(this,"01\nSCAN",Deck.cyan){if(!busy&&permissions())scan()}
        readButton=Deck.button(this,"02\nREAD + SAVE",Deck.lime){read()}.apply { isEnabled=false }
        writeButton=Deck.button(this,"03\nWRITE + VERIFY",Deck.pink){confirmWrite()}.apply { isEnabled=false }
        for(b in listOf(scanButton,readButton,writeButton)) {
            b.minHeight=Deck.dp(this,76)
            Deck.weighted(actions,b)
        }
        Deck.add(workflow,actions)
        devices=Deck.stack(this)
        Deck.add(workflow,devices,0)
        Deck.add(root,workflow)
        val backup=Deck.card(this,Deck.lime)
        Deck.heading(backup,"Read status",Deck.lime)
        backupStatus=Deck.text(this,"Backup required before writing.",13f,Deck.muted,true)
        Deck.add(backup,backupStatus)
        memoryButton=Deck.button(this,"Inspect radio memories",Deck.cyan){showMemories()}.apply { isEnabled=false }
        settingsButton=Deck.button(this,"Radio settings / DTMF presets",Deck.amber){showSettings()}.apply { isEnabled=false }
        Deck.add(backup,memoryButton,5);Deck.add(backup,settingsButton,0)
        Deck.add(root,backup)
        plan=Deck.card(this,Deck.line)
        Deck.add(root,plan)
        val terminal=Deck.card(this,Deck.cyan)
        Deck.heading(terminal,"Activity log")
        status=Deck.text(this,"Choose a model and scan.",12f,Deck.lime,true).apply { setTextIsSelectable(true) }
        Deck.add(terminal,status)
        Deck.action(terminal,"Copy BLE / CHIRP diagnostics",Deck.amber) {
            val diagnostic=link.lastGattReport+(if(lastFailureReport.isNotBlank()) "\n\nRADIO READ/WRITE FAILURE\n"+lastFailureReport else "")
            val clipboard=getSystemService(Context.CLIPBOARD_SERVICE) as ClipboardManager
            clipboard.setPrimaryClip(ClipData.newPlainText("Radio BLE and CHIRP diagnostics",diagnostic))
            setStatus("Copied GATT and last radio error diagnostics.")
        }
        Deck.add(root,terminal)
        radioGroup.setOnCheckedChangeListener { _,checked ->
            model=models[(checked-100).coerceIn(0,models.lastIndex)]
            resetBackup();link.disconnect();targetName=""
            devices.removeAllViews()
            setStatus("Choose and connect a new device for "+model)
            updatePlan()
        }
        updatePlan()
        setContentView(ScrollView(this).apply { setBackgroundColor(Deck.bg);addView(root) })
        if(Build.VERSION.SDK_INT>=31) permissions()
    }
    private fun updatePlan() {
        if(!::plan.isInitialized) return
        plan.removeAllViews()
        Deck.heading(plan,"Write preview")
        val master=store.channels()
        val projected=FrequencyCatalog.forModel(master,model)
        Deck.add(plan,Deck.text(this,projected.size.toString()+" COMPATIBLE / "+master.size+" MASTER",12f,Deck.cyan,true,true))
        for(b in Deck.bands(projected)) Deck.add(plan,Deck.text(this,b.name+" · "+b.channels.size+" / "+Deck.slots(b.channels),12f,b.accent,true),5)
        Deck.add(plan,Deck.text(this,(master.size-projected.size).toString()+" unsupported memories skipped. FRS / GMRS target permissions apply.",12f,Deck.amber),0)
    }
    private fun refreshStatus() {
        if(!::targetStatus.isInitialized) return
        val ready=link.ready&&(!link.hasTidradioControl||link.confirmedBaud!=null)
        targetStatus.text=model+" · "+(targetName.ifBlank { "No adapter selected" })+"\n"+
            (if(ready) "LINK READY" else "NOT READY")+(link.confirmedBaud?.let { " · UART "+it+" baud" }?:"")
        targetStatus.setTextColor(if(ready) Deck.lime else Deck.muted)
        val snapshot=baofengBackup
        val count=snapshot?.optJSONArray("channels")?.length()
        val valid=ready&&readModel==model&&backupFile?.exists()==true
        backupStatus.text=(if(valid) "BACKUP SAVED · WRITE REVIEW AVAILABLE" else "FRESH READ REQUIRED")+
            (count?.let { "\n"+it+" occupied memories" }?:"")+
            (snapshot?.optString("sha256")?.take(16)?.let { "\nSHA-256 "+it }?:"")+
            (backupFile?.name?.let { "\n"+it }?:"")
        backupStatus.setTextColor(if(valid) Deck.lime else Deck.amber)
        scanButton.isEnabled=!busy
    }
    private fun resetBackup() {
        baofengBackup = null
        uvproBackup = null
        backupFile = null
        readModel = null
        if (::writeButton.isInitialized) writeButton.isEnabled = false
        if (::readButton.isInitialized) readButton.isEnabled = false
        if (::settingsButton.isInitialized) settingsButton.isEnabled = false
        if (::memoryButton.isInitialized) memoryButton.isEnabled = false
    }
    private fun setStatus(text:String)=runOnUiThread {
        if(!::status.isInitialized) return@runOnUiThread
        val now=java.text.SimpleDateFormat("HH:mm:ss",java.util.Locale.US).format(java.util.Date())
        if(text!=lastEvent) {
            lastEvent=text
            events.addLast("["+now+"] "+text)
            while(events.size>12) events.removeFirst()
        }
        status.text=events.joinToString("\n\n")
        refreshStatus()
    }
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
            Deck.styleButton(button,Deck.cyan)
            Deck.add(devices,button,5)
        }, { error -> setStatus(error) })
    }
    private fun connect(device: BluetoothDevice, name: String) {
        if (busy) return
        resetBackup()
        targetName = name
        val targetModel = model
        val family = if (targetModel == "BTECH UV-PRO")
            BluetoothLink.Family.BENSHI_UVPRO else BluetoothLink.Family.BAOFENG_DONLGE
        worker("Connecting to " + name + " ...") {
            val description = connectBlocking(device, family)
            if (link.hasTidradioControl) {
                val baud = TidradioAdapterController(link).ensureBaud(targetModel, {
                    link.disconnect()
                    // The adapter needs time to restart advertising after AT+RST.
                    Thread.sleep(1200)
                    var last: Exception? = null
                    var connected = false
                    for (attempt in 1..3) {
                        try {
                            connectBlocking(device, family)
                            connected = true
                            break
                        } catch (e: Exception) {
                            if (e is InterruptedException) throw e
                            last = e
                            link.disconnect()
                            if (attempt < 3) Thread.sleep(1500)
                        }
                    }
                    if (!connected) throw IOException("TD-BL1 reconnect failed", last)
                }, ::setStatus)
                setStatus("TD-BL1 UART verified: " + baud + " baud.\nReady to READ " + targetModel)
            } else {
                setStatus(description + "\nReady to READ " + targetModel)
            }
        }
    }

    private fun connectBlocking(device: BluetoothDevice, family: BluetoothLink.Family): String {
        val done = CountDownLatch(1)
        var result: Result<String>? = null
        // BluetoothLink delivers its result on the main thread. Never block that thread.
        link.connect(device, family) {
            result = it
            done.countDown()
        }
        if (!done.await(15, TimeUnit.SECONDS)) {
            link.disconnect()
            throw IOException("BLE connection timed out")
        }
        return result?.getOrThrow() ?: throw IOException("BLE connection result missing")
    }

    private fun worker(label: String, body: () -> Unit) {
        if (busy) return
        lastFailureReport = ""
        busy = true
        for (i in 0 until radioGroup.childCount) radioGroup.getChildAt(i).isEnabled = false
        readButton.isEnabled = false
        writeButton.isEnabled = false
        settingsButton.isEnabled = false
        memoryButton.isEnabled = false
        setStatus(label)
        task.submit {
            try { body() }
            catch (e: Exception) {
                // The CHIRP driver masks many exceptions behind a generic
                // RadioError. Preserve its Python-side diagnostic and our
                // Java exception chain for the operator to copy and report.
                lastFailureReport = e.stackTraceToString().take(6500)
                // Never reuse authorization after a failed or partial transaction.
                readModel = null
                baofengBackup = null
                uvproBackup = null
                link.disconnect()
                val heading = if (e is VerificationFailure)
                    "UPLOAD COMPLETED — VERIFICATION FAILED" else label + " FAILED"
                setStatus(heading + ":\n" +
                    (e.message ?: e.javaClass.simpleName).take(1250) +
                    "\nNo further write attempted. Copy diagnostics, reconnect, then READ again.")
            } finally {
                busy = false
                runOnUiThread {
                    for (i in 0 until radioGroup.childCount) radioGroup.getChildAt(i).isEnabled = true
                    val configured = link.ready && (!link.hasTidradioControl || link.confirmedBaud != null)
                    readButton.isEnabled = configured
                    writeButton.isEnabled = configured && readModel == model &&
                        backupFile?.exists() == true
                    settingsButton.isEnabled = writeButton.isEnabled && baofengBackup != null
                    memoryButton.isEnabled = baofengBackup != null
                    refreshStatus()
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
        if (link.hasTidradioControl && link.confirmedBaud != TidradioBaudProtocol.required(targetModel)) {
            setStatus("Connect and verify TD-BL1 baud before reading.")
            return
        }
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
    private fun showMemories() {
        val memories = baofengBackup?.optJSONArray("channels") ?: return
        val list = (0 until memories.length()).map { i ->
            val c = Channel.parse(memories.getJSONObject(i))
            c.slot.toString() + "  " + c.name + "  " + c.receive + " MHz  " + c.mode +
                (c.transmit?.let { " TX " + it } ?: " RX ONLY") +
                (c.txCtcss?.let { " TX CTCSS " + it } ?: "") +
                (c.txDcs?.let { " TX DCS " + it } ?: "")
        }
        AlertDialog.Builder(this).setTitle("Radio memories (" + list.size + ")")
            .setItems(list.toTypedArray(), null).setPositiveButton("Close", null).show()
    }

    private fun showSettings() {
        if (busy || readModel != model || !link.ready) return
        val snapshot = baofengBackup ?: return
        val arr = snapshot.optJSONArray("settings") ?: JSONArray()
        if (arr.length() == 0) {
            AlertDialog.Builder(this).setTitle("Settings unavailable")
                .setMessage(snapshot.optString("settings_error").ifBlank { "Driver reported no supported operator settings." })
                .setPositiveButton("OK", null).show()
            return
        }
        val panel = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(24, 12, 24, 12)
        }
        val getters = linkedMapOf<String, () -> String>()
        val previous = linkedMapOf<String, String>()
        for (i in 0 until arr.length()) {
            val setting = arr.getJSONObject(i)
            val key = setting.getString("key")
            val value = setting.getString("value")
            previous[key] = value
            panel.addView(TextView(this).apply { text = setting.getString("label"); setPadding(0, 12, 0, 2) })
            if (setting.getString("kind") == "choice") {
                val options = setting.getJSONArray("options")
                val values = (0 until options.length()).map { options.getString(it) }
                val spinner = android.widget.Spinner(this)
                spinner.adapter = android.widget.ArrayAdapter(this, android.R.layout.simple_spinner_dropdown_item, values)
                spinner.setSelection(values.indexOf(value).coerceAtLeast(0))
                panel.addView(spinner)
                getters[key] = { spinner.selectedItem?.toString() ?: value }
            } else {
                val field = android.widget.EditText(this).apply {
                    setSingleLine(true)
                    setText(value)
                    Deck.field(this)
                    if (setting.getString("kind") == "integer") {
                        inputType = android.text.InputType.TYPE_CLASS_NUMBER or android.text.InputType.TYPE_NUMBER_FLAG_SIGNED
                        hint = setting.optInt("min").toString() + "–" + setting.optInt("max")
                    } else filters = arrayOf(android.text.InputFilter.LengthFilter(setting.optInt("max", 32)))
                }
                panel.addView(field)
                getters[key] = { field.text.toString() }
            }
        }
        val scroll = ScrollView(this).apply { addView(panel) }
        AlertDialog.Builder(this).setTitle("Model-specific radio settings")
            .setView(scroll).setNegativeButton("Cancel", null)
            .setPositiveButton("Review changes") { _, _ ->
                val changes = JSONObject()
                val summary = mutableListOf<String>()
                for ((key, get) in getters) {
                    val next = get()
                    if (next != previous[key]) {
                        changes.put(key, next)
                        summary.add(key + ": " + previous[key] + " → " + next)
                    }
                }
                if (changes.length() == 0) return@setPositiveButton
                AlertDialog.Builder(this).setTitle("Write " + changes.length() + " settings?")
                    .setMessage(summary.joinToString("\n"))
                    .setNegativeButton("Cancel", null)
                    .setPositiveButton("Write + verify") { _, _ -> writeSettings(snapshot, changes) }.show()
            }.show()
    }

    private fun writeSettings(snapshot: JSONObject, changes: JSONObject) {
        if (busy || !link.ready || readModel != model || backupFile?.exists() != true ||
            baofengBackup !== snapshot) return
        val target = model
        worker("Writing settings — keep radio connected...") {
            python().callAttr("write_settings", target, snapshot.getString("sha256"), changes.toString())
            try {
                val readback = JSONObject(python().callAttr("read_radio", target, true).toString())
                val arr = readback.getJSONArray("settings")
                val actual = (0 until arr.length()).associate { i ->
                    val row = arr.getJSONObject(i)
                    row.getString("key") to row.getString("value")
                }
                val keys = changes.keys().asSequence().toList()
                val mismatch = keys.filter { actual[it] != changes.getString(it).trimEnd() }
                if (mismatch.isNotEmpty()) error("Settings mismatch: " + mismatch.joinToString())
                val previous = snapshot.getJSONArray("channels")
                val current = readback.getJSONArray("channels")
                val oldChannels = (0 until previous.length()).map { Channel.parse(previous.getJSONObject(it)) }
                val newChannels = (0 until current.length()).map { Channel.parse(current.getJSONObject(it)) }
                val differences = ChirpCsv.diff(oldChannels, newChannels)
                if (differences.isNotEmpty() || previous.length() != current.length())
                    error("Memory changes detected after settings write: " + differences.take(5).joinToString())
                // Keep the original file; require a fresh backup for any later write.
                baofengBackup = readback
                readModel = null
                setStatus("VERIFIED " + keys.size + " settings on " + target + ". Original backup retained; read again before another write.")
            } catch (e: Exception) {
                throw VerificationFailure(e)
            }
        }
    }

    private fun confirmWrite() {
        if (busy || !link.ready || readModel != model || backupFile?.exists() != true) {
            setStatus("Read and back up this specific radio before writing.")
            return
        }
        val master = store.channels().sortedBy { it.slot }
        fun review(gmrsAuthorized: Boolean) {
            val selected = FrequencyCatalog.forModel(master, model, gmrsAuthorized)
            val skipped = master.size - selected.size
            val enabled = FrequencyCatalog.gmrsTxCount(selected)
            val inhibited = FrequencyCatalog.gmrsTxCount(master) - enabled
            if (selected.isEmpty()) {
                setStatus("Master channel profile has no compatible memories.")
                return
            }
            AlertDialog.Builder(this)
                .setTitle("Write " + selected.size + " channel memories?")
                .setMessage(
                    "Radio: " + targetName + "\nModel: " + model +
                    "\nBackup: " + backupFile!!.name +
                    "\nUnsupported / unavailable memories skipped: " + skipped +
                    "\nFRS TX inhibited (no certified FRS driver): " + FrequencyCatalog.frsTxCount(master) +
                    "\nGMRS TX enabled: " + enabled +
                    "\nGMRS TX inhibited / unavailable: " + inhibited +
                    "\n\nOther memories and radio-specific settings will be preserved. " +
                    "Keep the connection uninterrupted."
                )
                .setNegativeButton("Cancel", null)
                .setPositiveButton("Write + verify") { _, _ ->
                    if (!busy && link.ready && readModel == model && backupFile?.exists() == true)
                        write(selected, gmrsAuthorized)
                }.show()
        }
        if (FrequencyCatalog.gmrsTxCount(master) == 0) {
            review(false)
        } else {
            AlertDialog.Builder(this)
                .setTitle("GMRS transmit permissions")
                .setMessage("Choose GMRS TX only for a GMRS-certified radio with compatible firmware. " +
                    "Channels 8–14 remain RX-only with these drivers because 0.5 W ERP is not verified. " +
                    "No firmware TX locks are bypassed.")
                .setNegativeButton("Cancel", null)
                .setNeutralButton("RX only") { _, _ -> review(false) }
                .setPositiveButton("Certified GMRS target") { _, _ -> review(true) }
                .show()
        }
    }
    private fun write(selected: List<Channel>, gmrsAuthorized: Boolean = false) {
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
                        prior.getString("sha256"), array.toString(), gmrsAuthorized)
                    setStatus("Write completed. Reading radio independently to verify...")
                    try {
                        val readback = JSONObject(python().callAttr("read_radio", targetModel, true).toString())
                        val memory = readback.getJSONArray("channels")
                        val actual = (0 until memory.length()).map {
                            Channel.parse(memory.getJSONObject(it))
                        }
                        val differences = ChirpCsv.diff(selected, actual)
                        if (differences.isNotEmpty())
                            throw IllegalStateException("Read-back mismatch: " + differences.take(8).joinToString("; "))
                    } catch (e: Exception) {
                        throw VerificationFailure(e)
                    }
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
        val skipped = store.channels().size - total
        setStatus("VERIFIED " + total + " compatible memories on " + targetModel +
            " (" + skipped + " master memories skipped). Master " +
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
