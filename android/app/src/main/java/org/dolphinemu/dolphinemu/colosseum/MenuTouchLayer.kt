// Colosseum Native for Android: touch input for the in-game menu.
//
// Sits over the game surface and the on-screen controller. While the menu is open it takes
// taps and passes them to the menu; otherwise it lets every touch through to the controls.
// Also adds the small "Menu" button that opens the menu.
package org.dolphinemu.dolphinemu.colosseum

import android.annotation.SuppressLint
import android.app.Activity
import android.content.Context
import android.content.ContextWrapper
import android.view.Gravity
import android.view.MotionEvent
import android.view.View
import android.view.ViewGroup
import android.widget.FrameLayout
import com.google.android.material.button.MaterialButton
import kotlin.math.abs

@SuppressLint("ViewConstructor")
class MenuTouchLayer(context: Context) : View(context) {
    private var downX = 0f
    private var downY = 0f

    @SuppressLint("ClickableViewAccessibility")
    override fun onTouchEvent(event: MotionEvent): Boolean {
        if (!ColosseumNative.menuIsOpen()) return false
        when (event.actionMasked) {
            MotionEvent.ACTION_DOWN -> {
                downX = event.x
                downY = event.y
            }
            MotionEvent.ACTION_UP -> {
                val slop = 24 * resources.displayMetrics.density
                if (abs(event.x - downX) < slop && abs(event.y - downY) < slop && width > 0 && height > 0)
                    ColosseumNative.menuTap(event.x / width, event.y / height)
            }
        }
        return true
    }

    companion object {
        /** Adds the touch layer and the Menu button on top of the emulation screen. */
        @JvmStatic
        fun attach(root: ViewGroup) {
            val context = root.context
            root.addView(
                MenuTouchLayer(context),
                FrameLayout.LayoutParams(FrameLayout.LayoutParams.MATCH_PARENT, FrameLayout.LayoutParams.MATCH_PARENT)
            )
            val density = context.resources.displayMetrics.density
            val button = MaterialButton(
                context, null, com.google.android.material.R.attr.materialButtonOutlinedStyle
            ).apply {
                text = "Menu"
                isAllCaps = false
                alpha = 0.55f
                minHeight = 0
                minimumHeight = 0
                setPadding((14 * density).toInt(), 0, (14 * density).toInt(), 0)
                setOnClickListener {
                    // Touch: the native settings panel. (A controller's Select opens the
                    // on-screen menu, which also takes taps through this layer.)
                    var c: Context = context
                    while (c !is Activity && c is ContextWrapper) c = c.baseContext
                    if (c is Activity) GameMenuPanel.toggle(c) else ColosseumNative.menuToggle()
                }
            }
            root.addView(
                button,
                FrameLayout.LayoutParams(FrameLayout.LayoutParams.WRAP_CONTENT, (36 * density).toInt()).apply {
                    gravity = Gravity.TOP or Gravity.CENTER_HORIZONTAL
                    topMargin = (6 * density).toInt()
                }
            )
        }
    }
}
