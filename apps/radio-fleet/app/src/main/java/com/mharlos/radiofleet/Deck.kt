package com.mharlos.radiofleet

import android.content.Context
import android.content.res.ColorStateList
import android.graphics.*
import android.graphics.drawable.Drawable
import android.graphics.drawable.GradientDrawable
import android.graphics.drawable.RippleDrawable
import android.view.Gravity
import android.view.View
import android.widget.*
import java.util.Locale

/** Shared native cyberdeck design system. No new runtime dependencies. */
internal object Deck {
    val bg = Color.rgb(5,12,18)
    val panel = Color.rgb(13,24,34)
    val ink = Color.rgb(235,247,249)
    val muted = Color.rgb(151,175,186)
    val cyan = Color.rgb(49,231,220)
    val pink = Color.rgb(255,76,170)
    val lime = Color.rgb(174,240,103)
    val amber = Color.rgb(255,191,83)
    val line = Color.rgb(42,65,76)
    fun dp(c: Context,n: Int) = (n*c.resources.displayMetrics.density+0.5f).toInt()
    fun tinted(c: Int, alpha: Int) = (c and 0x00ffffff) or (alpha shl 24)

    private class CutPanel(private val accent: Int, private val active: Boolean, private val cut: Float): Drawable() {
        private val paint = Paint(Paint.ANTI_ALIAS_FLAG)
        override fun draw(canvas: Canvas) {
            val b = bounds
            val l=b.left+1f; val t=b.top+1f; val r=b.right-1f; val bot=b.bottom-1f
            val k = cut.coerceAtMost((bot-t)/3).coerceAtLeast(0f)
            val path = Path().apply {
                moveTo(l+k,t); lineTo(r-k,t); lineTo(r,t+k); lineTo(r,bot-k)
                lineTo(r-k,bot); lineTo(l+k,bot); lineTo(l,bot-k); lineTo(l,t+k); close()
            }
            paint.style=Paint.Style.FILL
            paint.shader=LinearGradient(l,t,r,bot,
                intArrayOf(if(active) Color.rgb(20,53,58) else panel,bg),null,Shader.TileMode.CLAMP)
            canvas.drawPath(path,paint)
            paint.shader=null; paint.style=Paint.Style.STROKE; paint.strokeWidth=if(active) 2f else 1f
            paint.color=tinted(accent,if(active) 240 else 130)
            canvas.drawPath(path,paint)
            paint.style=Paint.Style.FILL; paint.color=accent
            canvas.drawRect(l,t+k+1,l+3,bot-k-1,paint)
        }
        override fun setAlpha(alpha: Int) {}
        override fun setColorFilter(colorFilter: ColorFilter?) {}
        @Deprecated("Deprecated in Java") override fun getOpacity() = PixelFormat.TRANSLUCENT
    }
    fun frame(c:Context,accent:Int=cyan,active:Boolean=false):Drawable =
        CutPanel(accent,active,dp(c,9).toFloat())
    fun text(c:Context,value:String,size:Float=14f,color:Int=ink,mono:Boolean=false,bold:Boolean=false) =
        TextView(c).apply {
            text=value; textSize=size; setTextColor(color)
            typeface=Typeface.create(if(mono) "monospace" else "sans-serif",if(bold) Typeface.BOLD else Typeface.NORMAL)
            setLineSpacing(dp(c,2).toFloat(),1f)
        }
    fun stack(c:Context,pad:Int=0) = LinearLayout(c).apply {
        orientation=LinearLayout.VERTICAL; setPadding(dp(c,pad),dp(c,pad),dp(c,pad),dp(c,pad))
    }
    fun row(c:Context) = LinearLayout(c).apply { orientation=LinearLayout.HORIZONTAL; gravity=Gravity.CENTER_VERTICAL }
    fun add(parent:LinearLayout,view:View,margin:Int=8) {
        parent.addView(view,LinearLayout.LayoutParams(-1,-2).apply { bottomMargin=dp(parent.context,margin) })
    }
    fun weighted(parent:LinearLayout,view:View) {
        parent.addView(view,LinearLayout.LayoutParams(0,-2,1f).apply { marginEnd=dp(parent.context,4) })
    }
    fun card(c:Context,accent:Int=cyan):LinearLayout = stack(c,13).apply { background=frame(c,accent) }
    fun heading(parent:LinearLayout,title:String,accent:Int=cyan) {
        val c=parent.context
        val r=row(c)
        weighted(r,text(c,title.uppercase(Locale.US),12f,accent,true,true))
        r.addView(text(c,"////",12f,tinted(accent,150),true,true))
        add(parent,r,9)
    }
    fun styleButton(b:Button,accent:Int=cyan,active:Boolean=false) {
        val c=b.context
        b.isAllCaps=false; b.textSize=13f
        b.typeface=Typeface.create("sans-serif-medium",Typeface.NORMAL)
        b.setTextColor(ColorStateList(arrayOf(intArrayOf(-android.R.attr.state_enabled),intArrayOf()),intArrayOf(muted,ink)))
        b.background=RippleDrawable(ColorStateList.valueOf(tinted(accent,65)),frame(c,accent,active),null)
        b.setPadding(dp(c,10),dp(c,11),dp(c,10),dp(c,11))
        b.minimumHeight=dp(c,48); b.minHeight=dp(c,48); b.minimumWidth=0; b.minWidth=0
    }
    fun button(c:Context,title:String,accent:Int=cyan,active:Boolean=false,action:()->Unit) =
        Button(c).apply { text=title; styleButton(this,accent,active); setOnClickListener { action() } }
    fun action(parent:LinearLayout,title:String,accent:Int=cyan,action:()->Unit) =
        add(parent,button(parent.context,title,accent,false,action))
    fun field(e:EditText) {
        val c=e.context
        e.setTextColor(ink); e.setHintTextColor(muted); e.background=frame(c,line)
        e.setPadding(dp(c,12),dp(c,10),dp(c,12),dp(c,10)); e.minimumHeight=dp(c,48)
    }
    fun header(c:Context,subtitle:String):LinearLayout = card(c,cyan).apply {
        val r=row(c)
        weighted(r,stack(c).apply {
            add(this,text(c,"FARM RADIO FLEET",20f,ink,false,true),3)
            add(this,text(c,subtitle.uppercase(Locale.US),11f,cyan,true,true),0)
        })
        r.addView(AntennaMark(c),LinearLayout.LayoutParams(dp(c,54),dp(c,48)))
        add(this,r,0)
    }
    private class AntennaMark(c:Context):View(c) {
        private val p=Paint(Paint.ANTI_ALIAS_FLAG)
        override fun onDraw(canvas:Canvas) {
            super.onDraw(canvas); val w=width.toFloat(); val h=height.toFloat()
            p.color=pink; p.style=Paint.Style.STROKE; p.strokeWidth=dp(context,2).toFloat()
            canvas.drawArc(w*.15f,h*.02f,w*.85f,h*.73f,205f,130f,false,p)
            p.color=cyan
            canvas.drawArc(w*.28f,h*.14f,w*.72f,h*.58f,205f,130f,false,p)
            canvas.drawLine(w*.5f,h*.36f,w*.24f,h*.9f,p)
            canvas.drawLine(w*.5f,h*.36f,w*.76f,h*.9f,p)
            canvas.drawLine(w*.34f,h*.69f,w*.66f,h*.69f,p)
            canvas.drawLine(w*.24f,h*.9f,w*.76f,h*.9f,p)
            p.style=Paint.Style.FILL; canvas.drawCircle(w*.5f,h*.31f,dp(context,3).toFloat(),p)
        }
    }
    data class Band(val key:String,val name:String,val accent:Int,val channels:List<Channel>)
    fun bands(channels:List<Channel>):List<Band> {
        fun key(c:Channel):String = when {
            c.service.contains("aviation",true)||c.mode=="AM" -> "air"
            c.service.contains("marine",true) -> "marine"
            c.service.startsWith("Local",true) -> "local"
            FrequencyCatalog.isFrs(c) -> "frs"
            c.service.contains("GMRS repeater",true)||c.name.startsWith("GMR",true)&&c.transmit!=null&&kotlin.math.abs(c.transmit-c.receive-5.0)<.0001 -> "repeat"
            c.service.startsWith("GMRS",true)||c.name.startsWith("GMRS",true) -> "gmrs"
            c.service.contains("weather",true) -> "weather"
            c.slot in 1..19 -> "farm"
            else -> "custom"
        }
        val grouped=channels.sortedBy { it.slot }.groupBy(::key)
        return listOf(Triple("farm","Farm communications",lime),Triple("air","Regional aviation",pink),
            Triple("marine","Columbia marine",cyan),Triple("local","Local services",amber),
            Triple("frs","FRS simplex / monitoring",lime),Triple("gmrs","GMRS simplex",cyan),
            Triple("repeat","GMRS repeaters",pink),Triple("weather","Weather",cyan),Triple("custom","Other / custom",muted))
            .mapNotNull { (k,n,a) -> grouped[k]?.let { Band(k,n,a,it) } }
    }
    fun slots(channels:List<Channel>):String {
        val sorted=channels.map { it.slot }.distinct().sorted()
        if(sorted.isEmpty()) return "—"
        val ranges=mutableListOf<String>(); var start=sorted.first(); var end=start
        fun flush(){ranges.add(if(start==end) start.toString() else start.toString()+"–"+end) }
        for(n in sorted.drop(1)) { if(n==end+1) end=n else {flush();start=n;end=n} }; flush()
        return ranges.joinToString(", ")
    }
}
