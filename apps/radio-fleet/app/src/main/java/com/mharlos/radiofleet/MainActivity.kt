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
    private val bg = Deck.bg
    private val surface = Deck.panel
    private val green = Deck.cyan
    private val foreground = Deck.ink
    private val muted = Deck.muted

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        store = FleetStore(this)
        channels = store.channels().toMutableList()
        radios = store.radios().toMutableList()
        window.statusBarColor = bg
        window.navigationBarColor = bg
        render()
    }
    private var tab = "Home"
    private var query = ""
    private var filter = "All"
    private val expanded = mutableSetOf<String>()
    private var initialized = false
    private var revision = ""
    private val favorites: MutableSet<String> by lazy {
        getSharedPreferences("deck_ui",MODE_PRIVATE).getStringSet("favorites",emptySet())!!.toMutableSet()
    }
    private fun favKey(c:Channel) = c.slot.toString() + ":" + c.name + ":" + c.receive
    private fun label(text:String,size:Float=15f,color:Int=foreground) = Deck.text(this,text,size,if(color==Color.DKGRAY) Deck.muted else color)
    private fun stack() = Deck.stack(this,14)
    private fun addButton(host:LinearLayout,text:String,handler:()->Unit) = Deck.action(host,text,Deck.cyan,handler)
    private fun section(host:LinearLayout,title:String) = Deck.heading(host,title)
    private fun program() { startActivity(Intent(this,RadioProgrammingActivity::class.java)) }
    private fun native(family:String) { startActivity(Intent(this,DeviceConsoleActivity::class.java).putExtra("family",family)) }
    private fun kiss() { startActivity(Intent(this,KissTerminalActivity::class.java)) }
    override fun onResume() {
        super.onResume()
        if(initialized) { channels=store.channels().toMutableList(); radios=store.radios().toMutableList(); render() }
    }
    private fun render() {
        initialized=true
        revision=ChirpCsv.fingerprint(channels)
        val root=Deck.stack(this)
        root.setBackgroundColor(Deck.bg)
        val scroll=ScrollView(this).apply { isFillViewport=true }
        val host=Deck.stack(this,12)
        scroll.addView(host)
        root.addView(scroll,LinearLayout.LayoutParams(-1,0,1f))
        Deck.add(host,Deck.header(this,when(tab) {
            "Home" -> "Field communications command"
            "Channels" -> "Master frequency plan"
            "Program" -> "Memory programming"
            "Devices" -> "Device management"
            else -> "System configuration"
        }))
        when(tab) {
            "Home" -> dashboard(host)
            "Channels" -> channelPage(host)
            "Program" -> programPage(host)
            "Devices" -> devicePage(host)
            else -> settingsPage(host)
        }
        val dock=Deck.row(this).apply {
            setPadding(Deck.dp(this@MainActivity,4),Deck.dp(this@MainActivity,5),Deck.dp(this@MainActivity,4),Deck.dp(this@MainActivity,5))
            setBackgroundColor(Deck.panel)
        }
        for ((name,icon) in listOf("Home" to "⌂","Channels" to "≡","Program" to "▣","Devices" to "⌘","Settings" to "⚙")) {
            val b=Deck.button(this,icon+"\n"+name,if(tab==name) Deck.cyan else Deck.line,tab==name) { if(tab!=name){tab=name;render()} }
            b.textSize=11f
            b.setPadding(0,Deck.dp(this,6),0,Deck.dp(this,6))
            Deck.weighted(dock,b)
        }
        root.addView(dock,LinearLayout.LayoutParams(-1,-2))
        setContentView(root)
    }
    private fun dashboard(host:LinearLayout) {
        val stats=Deck.row(this)
        for((v,t) in listOf(channels.size.toString() to "MEMORIES",Deck.bands(channels).size.toString() to "GROUPS",radios.size.toString() to "REGISTERED")) {
            val box=Deck.card(this,Deck.line)
            Deck.add(box,Deck.text(this,v,24f,Deck.cyan,true,true),2)
            Deck.add(box,Deck.text(this,t,10f,Deck.muted,true),0)
            Deck.weighted(stats,box)
        }
        Deck.add(host,stats)
        val tools=Deck.card(this)
        Deck.heading(tools,"Discover / tools")
        val r1=Deck.row(this); val r2=Deck.row(this)
        Deck.weighted(r1,Deck.button(this,"≡  Frequency library",Deck.cyan){showLibrary()})
        Deck.weighted(r1,Deck.button(this,"⌁  Repeater finder",Deck.pink){findRepeaters()})
        Deck.weighted(r2,Deck.button(this,"▦  DTMF tools",Deck.amber){dtmfBuilder()})
        Deck.weighted(r2,Deck.button(this,"↗  APRS / KISS",Deck.lime){kiss()})
        Deck.add(tools,r1,5);Deck.add(tools,r2,0);Deck.add(host,tools)
        Deck.heading(host,"Master channel plan")
        for(b in Deck.bands(channels)) {
            val card=Deck.card(this,b.accent)
            val row=Deck.row(this)
            Deck.weighted(row,Deck.stack(this).apply {
                Deck.add(this,Deck.text(this@MainActivity,b.name,15f,Deck.ink,false,true),3)
                Deck.add(this,Deck.text(this@MainActivity,"SLOTS "+Deck.slots(b.channels),11f,b.accent,true),0)
            })
            row.addView(Deck.text(this,b.channels.size.toString()+"  ›",17f,b.accent,true,true))
            Deck.add(card,row,0)
            card.contentDescription=b.name+", "+b.channels.size+" channels"
            card.setOnClickListener { tab="Channels";query="";filter="All";expanded.add(b.key);render() }
            Deck.add(host,card,6)
        }
        Deck.add(host,Deck.text(this,"MASTER REV "+revision,11f,Deck.muted,true))
    }
    private fun channelPage(host:LinearLayout) {
        val controls=Deck.card(this)
        val input=EditText(this).apply { hint="Search name, frequency, service or slot";setSingleLine(true);setText(query);Deck.field(this) }
        Deck.add(controls,input,8)
        val result=Deck.stack(this)
        val filters=Deck.row(this)
        val choices=listOf("All","RX only","TX","★")
        val buttons=mutableListOf<Button>()
        for(f in choices) {
            val b=Deck.button(this,f,Deck.cyan,filter==f) {
                filter=f
                buttons.forEachIndexed { i,v -> Deck.styleButton(v,Deck.cyan,choices[i]==filter) }
                renderBands(result)
            }
            buttons.add(b);Deck.weighted(filters,b)
        }
        Deck.add(controls,filters,0);Deck.add(host,controls)
        Deck.action(host,"+  New memory",Deck.lime){edit(null)}
        Deck.add(host,result,0)
        input.addTextChangedListener(object:android.text.TextWatcher {
            override fun beforeTextChanged(s:CharSequence?,start:Int,count:Int,after:Int){}
            override fun onTextChanged(s:CharSequence?,start:Int,before:Int,count:Int){query=s?.toString().orEmpty();renderBands(result)}
            override fun afterTextChanged(s:android.text.Editable?){}
        })
        renderBands(result)
    }
    private fun renderBands(result:LinearLayout) {
        result.removeAllViews()
        val filtered=channels.filter { c ->
            val text=listOf(c.name,c.slot.toString(),c.receive.toString(),c.transmit?.toString().orEmpty(),c.service,c.description,c.mode).joinToString(" ")
            text.contains(query.trim(),true) && when(filter) {
                "RX only" -> c.transmit==null
                "TX" -> c.transmit!=null
                "★" -> favKey(c) in favorites
                else -> true
            }
        }
        Deck.add(result,Deck.text(this,filtered.size.toString()+" / "+channels.size+" MEMORIES · TX shows profile intent",11f,Deck.muted,true))
        for(b in Deck.bands(filtered)) {
            val card=Deck.card(this,b.accent)
            val rows=Deck.stack(this)
            val open=b.key in expanded||query.isNotBlank()||filter!="All"
            val header=Deck.row(this)
            Deck.weighted(header,Deck.stack(this).apply {
                Deck.add(this,Deck.text(this@MainActivity,b.name,15f,Deck.ink,false,true),3)
                Deck.add(this,Deck.text(this@MainActivity,"SLOTS "+Deck.slots(b.channels),11f,b.accent,true),0)
            })
            val toggle=Deck.text(this,b.channels.size.toString()+"  "+if(open)"⌃" else "⌄",17f,b.accent,true,true)
            header.addView(toggle)
            header.minimumHeight=Deck.dp(this,48)
            header.setOnClickListener {
                val showing=rows.visibility==View.VISIBLE
                if(showing) expanded.remove(b.key) else expanded.add(b.key)
                rows.visibility=if(showing) View.GONE else View.VISIBLE
                toggle.text=b.channels.size.toString()+"  "+if(showing)"⌄" else "⌃"
            }
            Deck.add(card,header,0)
            for(c in b.channels) {
                val row=Deck.row(this).apply {
                    minimumHeight=Deck.dp(this@MainActivity,68)
                    setPadding(0,Deck.dp(this@MainActivity,7),0,Deck.dp(this@MainActivity,7))
                }
                val left=Deck.stack(this)
                Deck.add(left,Deck.text(this,String.format(Locale.US,"%03d  %s",c.slot,c.name),14f,b.accent,true,true),3)
                val rx=String.format(Locale.US,"%.5f",c.receive)
                val tx=c.transmit?.let { String.format(Locale.US,"%.5f",it) }
                Deck.add(left,Deck.text(this,"RX "+rx+" MHz  ·  "+c.mode,13f,Deck.ink,true),2)
                Deck.add(left,Deck.text(this,if(tx==null)"RX ONLY" else "TX "+tx+" MHz",11f,if(tx==null)Deck.lime else Deck.amber,true),2)
                if(c.txCtcss!=null||c.rxCtcss!=null||c.txDcs!=null||c.rxDcs!=null)
                    Deck.add(left,Deck.text(this,"Tones TX "+(c.txCtcss?:c.txDcs?:"off")+" / RX "+(c.rxCtcss?:c.rxDcs?:"off"),11f,Deck.muted),2)
                if(c.description.isNotBlank()) Deck.add(left,Deck.text(this,c.description,11f,Deck.muted),0)
                left.setOnClickListener { edit(c) }
                Deck.weighted(row,left)
                val star=Deck.button(this,if(favKey(c) in favorites)"★" else "☆",Deck.amber){
                    if(!favorites.add(favKey(c))) favorites.remove(favKey(c))
                    getSharedPreferences("deck_ui",MODE_PRIVATE).edit().putStringSet("favorites",favorites.toSet()).apply()
                    renderBands(result)
                }
                star.contentDescription="Toggle favorite "+c.name
                row.addView(star,LinearLayout.LayoutParams(Deck.dp(this,48),-2))
                val editBtn=Deck.button(this,"✎",b.accent){edit(c)}
                editBtn.contentDescription="Edit "+c.name
                row.addView(editBtn,LinearLayout.LayoutParams(Deck.dp(this,48),-2))
                rows.addView(View(this).apply { setBackgroundColor(Deck.line) },LinearLayout.LayoutParams(-1,Deck.dp(this,1)))
                Deck.add(rows,row,0)
            }
            rows.visibility=if(open)View.VISIBLE else View.GONE
            Deck.add(card,rows,0);Deck.add(result,card,7)
        }
        if(filtered.isEmpty()) Deck.add(result,Deck.text(this,"No channels match the current filter.",14f,Deck.muted))
    }
    private fun programPage(host:LinearLayout) {
        val card=Deck.card(this)
        Deck.heading(card,"Target radio")
        Deck.add(card,Deck.text(this,"BAOFENG / TD-BL1",18f,Deck.ink,true,true))
        Deck.add(card,Deck.text(this,"UV-5R · UV-5RM  /  Connect, read, save backup, then write and verify.",13f,Deck.muted))
        Deck.action(card,"Open memory programmer  ›",Deck.cyan){program()}
        Deck.action(card,"BTECH UV-PRO · native bank programmer",Deck.pink){native("benshi")}
        Deck.add(host,card)
        val preview=Deck.card(this,Deck.line)
        Deck.heading(preview,"Master write plan")
        for(b in Deck.bands(channels)) Deck.add(preview,Deck.text(this,b.name+"  ·  "+b.channels.size+"  /  "+Deck.slots(b.channels),13f,b.accent,true),5)
        Deck.add(preview,Deck.text(this,"Target-specific RX/TX limits and supported modes applied during programming.",12f,Deck.amber))
        Deck.add(host,preview)
    }
    private fun devicePage(host:LinearLayout) {
        val interfaces=Deck.card(this)
        Deck.heading(interfaces,"Device interfaces")
        Deck.action(interfaces,"UV-5R / UV-5RM  ·  TD-BL1",Deck.cyan){program()}
        Deck.action(interfaces,"BTECH UV-PRO  ·  Native BLE / experimental",Deck.pink){native("benshi")}
        Deck.action(interfaces,"MeshCore  ·  Companion / experimental",Deck.lime){native("meshcore")}
        Deck.action(interfaces,"APRS / KISS  ·  Classic Bluetooth",Deck.amber){kiss()}
        Deck.add(host,interfaces)
        val inventory=Deck.card(this,Deck.line)
        Deck.heading(inventory,"Registered radios")
        if(radios.isEmpty()) Deck.add(inventory,Deck.text(this,"No registered radios.",14f,Deck.muted))
        for(r in radios) Deck.action(inventory,r.label+"\n"+r.model+" · "+if(r.verifiedHash==revision)"REVISION VERIFIED" else "NOT VERIFIED",if(r.verifiedHash==revision)Deck.lime else Deck.amber){radioActions(r)}
        Deck.action(inventory,"+ Register radio",Deck.cyan){addRadio()}
        Deck.action(inventory,"Compare read-back CSV",Deck.cyan){pickVerifyRadio()}
        Deck.add(host,inventory)
    }
    private fun settingsPage(host:LinearLayout) {
        val data=Deck.card(this)
        Deck.heading(data,"Profile / storage")
        Deck.add(data,Deck.text(this,"MASTER REV "+revision,12f,Deck.muted,true))
        Deck.action(data,"Export CHIRP CSV",Deck.cyan){exportCsv()}
        Deck.action(data,"Import CHIRP CSV",Deck.amber){chooseCsv(REQUEST_IMPORT)}
        Deck.action(data,"Merge missing default regional / FRS / GMRS channels",Deck.lime){
            val before=channels.size
            channels=StarterProfile.mergeMissing(channels).toMutableList()
            save()
            val conflicts=StarterProfile.reservedDefaults.count { c -> channels.none { it.slot==c.slot&&it.name==c.name } }
            message("Default channels","Added "+(channels.size-before)+" memories. "+conflicts+" reserved slots differ; existing channels retained.")
        }
        Deck.add(host,data)
        val support=Deck.card(this,Deck.line)
        Deck.heading(support,"Diagnostics / tools")
        Deck.action(support,"Inspect TD-BL1 BLE services",Deck.cyan){startActivity(Intent(this,BleInspectorActivity::class.java))}
        Deck.action(support,"External CHIRP / RadioDroid workflow",Deck.cyan){showProgrammingFlow()}
        Deck.action(support,"Browse frequency library",Deck.cyan){showLibrary()}
        Deck.add(host,support)
    }

    private fun input(value: String, numeric: Boolean = false): EditText = EditText(this).apply {
        setSingleLine(true)
        setText(value)
        textSize = 15f
        Deck.field(this)
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
