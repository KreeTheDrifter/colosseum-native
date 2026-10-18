// Colosseum Native for Android: the in-game settings panel (the "Menu" button).
//
// The same settings as the desktop's in-game menu (quality presets, resolution,
// anti-aliasing, frame rate, save/load state, GPU driver, ...), as a native touch panel:
// every setting has big < and > buttons, actions are buttons. The game pauses while it is
// open. The rows come from the shared menu code (runtime/game_menu.cpp) through
// ColosseumNative.menuItems / menuChange.
package org.dolphinemu.dolphinemu.colosseum

import android.app.Activity
import android.app.Dialog
import android.graphics.Color
import android.graphics.Typeface
import android.graphics.drawable.GradientDrawable
import android.view.Gravity
import android.view.ViewGroup
import android.view.Window
import android.view.WindowManager
import android.widget.LinearLayout
import android.widget.ScrollView
import android.widget.TextView
import com.google.android.material.button.MaterialButton
import org.dolphinemu.dolphinemu.NativeLibrary

class GameMenuPanel private constructor(private val activity: Activity) {
    private val dp = activity.resources.displayMetrics.density
    private val dialog = Dialog(activity)
    private val list = LinearLayout(activity).apply { orientation = LinearLayout.VERTICAL }
    private val status = TextView(activity).apply {
        textSize = 14f
        setTextColor(Color.rgb(140, 230, 140))
        gravity = Gravity.CENTER
    }
    private var wasRunning = false

    private fun text(value: String, sizeSp: Float, bold: Boolean = false, color: Int = Color.WHITE) =
        TextView(activity).apply {
            text = value
            textSize = sizeSp
            setTextColor(color)
            if (bold) setTypeface(typeface, Typeface.BOLD)
        }

    private fun button(label: String, outlined: Boolean, onClick: () -> Unit) = MaterialButton(
        activity, null,
        if (outlined) com.google.android.material.R.attr.materialButtonOutlinedStyle
        else com.google.android.material.R.attr.materialButtonStyle
    ).apply {
        text = label
        isAllCaps = false
        textSize = 16f
        minWidth = (52 * dp).toInt()
        minimumWidth = (52 * dp).toInt()
        setOnClickListener { onClick() }
    }

    private fun build() {
        list.removeAllViews()
        val rows = ColosseumNative.menuItems()
        rows.forEachIndexed { index, encoded ->
            val parts = encoded.split('\u001f')
            val kind = parts.getOrElse(0) { "s" }
            val label = parts.getOrElse(1) { "" }
            val value = parts.getOrElse(2) { "" }
            when (kind) {
                "h" -> list.addView(text(label.uppercase(), 13f, bold = true, color = Color.rgb(255, 200, 64)).apply {
                    setPadding(0, (18 * dp).toInt(), 0, (6 * dp).toInt())
                    letterSpacing = 0.08f
                })
                "a" -> {
                    if (label == "Resume") return@forEachIndexed  // the panel has its own
                    list.addView(button(label, outlined = true) { action(index, label) },
                        LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, (52 * dp).toInt())
                            .apply { bottomMargin = (4 * dp).toInt() })
                }
                else -> {
                    val row = LinearLayout(activity).apply {
                        orientation = LinearLayout.HORIZONTAL
                        gravity = Gravity.CENTER_VERTICAL
                        minimumHeight = (56 * dp).toInt()
                    }
                    row.addView(text(label, 16f), LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f))
                    row.addView(button("‹", outlined = true) { change(index, -1) },
                        LinearLayout.LayoutParams((52 * dp).toInt(), (48 * dp).toInt()))
                    row.addView(text(value, 15f, bold = true).apply {
                        gravity = Gravity.CENTER
                        maxLines = 2
                    }, LinearLayout.LayoutParams((150 * dp).toInt(), ViewGroup.LayoutParams.WRAP_CONTENT))
                    row.addView(button("›", outlined = true) { change(index, 1) },
                        LinearLayout.LayoutParams((52 * dp).toInt(), (48 * dp).toInt()))
                    list.addView(row)
                }
            }
        }
    }

    private fun change(index: Int, dir: Int) {
        val message = ColosseumNative.menuChange(index, dir)
        status.text = message
        build()
    }

    private fun action(index: Int, label: String) {
        when {
            label.startsWith("Quit") || label.startsWith("GPU driver") -> {
                // These leave the game: close the panel first.
                dismiss()
                ColosseumNative.menuChange(index, 1)
            }
            label.startsWith("Save state") || label.startsWith("Load state") -> {
                // Saving/loading runs while the game runs; close and resume.
                ColosseumNative.menuChange(index, 1)
                dismiss()
            }
            else -> change(index, 1)
        }
    }

    private fun show() {
        val root = LinearLayout(activity).apply {
            orientation = LinearLayout.VERTICAL
            val pad = (20 * dp).toInt()
            setPadding(pad, pad, pad, pad)
            background = GradientDrawable().apply {
                setColor(Color.argb(240, 22, 26, 34))
                cornerRadius = 20 * dp
            }
        }
        val top = LinearLayout(activity).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = Gravity.CENTER_VERTICAL
        }
        top.addView(text("Colosseum settings", 20f, bold = true), LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f))
        top.addView(button("Resume", outlined = false) { dismiss() })
        root.addView(top)
        root.addView(status)
        root.addView(ScrollView(activity).apply { addView(list) },
            LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, 0, 1f))
        build()

        dialog.requestWindowFeature(Window.FEATURE_NO_TITLE)
        dialog.setContentView(root)
        dialog.setOnDismissListener { onClosed() }
        dialog.window?.apply {
            setBackgroundDrawableResource(android.R.color.transparent)
            val metrics = activity.resources.displayMetrics
            setLayout(
                minOf((640 * dp).toInt(), (metrics.widthPixels * 0.92f).toInt()),
                (metrics.heightPixels * 0.92f).toInt()
            )
            addFlags(WindowManager.LayoutParams.FLAG_DIM_BEHIND)
            setDimAmount(0.4f)
        }

        ColosseumNative.menuSetPanelOpen(true)
        wasRunning = NativeLibrary.IsRunningAndUnpaused()
        if (wasRunning) NativeLibrary.PauseEmulation(false)
        dialog.show()
        current = this
    }

    private fun dismiss() {
        if (dialog.isShowing) dialog.dismiss()
    }

    private fun onClosed() {
        ColosseumNative.menuSetPanelOpen(false)
        if (wasRunning && NativeLibrary.IsRunning()) NativeLibrary.UnPauseEmulation()
        if (current === this) current = null
    }

    companion object {
        private var current: GameMenuPanel? = null

        @JvmStatic
        fun toggle(activity: Activity) {
            val open = current
            if (open != null) open.dismiss() else GameMenuPanel(activity).show()
        }
    }
}
