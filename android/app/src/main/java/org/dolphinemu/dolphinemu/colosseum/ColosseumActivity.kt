// Colosseum Native for Android: the start screen.
//
// First launch asks for the player's own Pokemon Colosseum (USA) disc image and unpacks it
// into the app (once). After that: Play, Settings, Controls and GPU driver.
package org.dolphinemu.dolphinemu.colosseum

import android.graphics.Typeface
import android.net.Uri
import android.os.Bundle
import android.view.Gravity
import android.view.ViewGroup
import android.view.ViewGroup.LayoutParams.MATCH_PARENT
import android.view.ViewGroup.LayoutParams.WRAP_CONTENT
import android.widget.LinearLayout
import android.widget.ScrollView
import android.widget.TextView
import androidx.activity.result.contract.ActivityResultContracts
import androidx.appcompat.app.AppCompatActivity
import androidx.lifecycle.lifecycleScope
import com.google.android.material.button.MaterialButton
import com.google.android.material.dialog.MaterialAlertDialogBuilder
import com.google.android.material.progressindicator.LinearProgressIndicator
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.delay
import kotlinx.coroutines.isActive
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import org.dolphinemu.dolphinemu.activities.EmulationActivity
import org.dolphinemu.dolphinemu.features.settings.ui.MenuTag
import org.dolphinemu.dolphinemu.features.settings.ui.SettingsActivity
import org.dolphinemu.dolphinemu.utils.AfterDirectoryInitializationRunner
import org.dolphinemu.dolphinemu.utils.DirectoryInitialization
import java.io.File

class ColosseumActivity : AppCompatActivity() {
    private lateinit var status: TextView
    private lateinit var play: MaterialButton
    private lateinit var driverButton: MaterialButton
    private var ready = false

    private val gameDir: File get() = File(filesDir, "Game")
    private val modulePath: String
        get() = File(applicationInfo.nativeLibraryDir, "libgGC6E01_recomp.so").absolutePath

    private val pickDisc = registerForActivityResult(ActivityResultContracts.OpenDocument()) { uri ->
        if (uri != null) unpack(uri)
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        val density = resources.displayMetrics.density
        val pad = (24 * density).toInt()

        val column = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            gravity = Gravity.CENTER_HORIZONTAL
            setPadding(pad, pad, pad, pad)
        }
        column.addView(TextView(this).apply {
            text = "Pokémon Colosseum"
            textSize = 28f
            setTypeface(typeface, Typeface.BOLD)
            gravity = Gravity.CENTER
        })
        column.addView(TextView(this).apply {
            text = "Native · private test build"
            textSize = 14f
            gravity = Gravity.CENTER
            alpha = 0.7f
        })
        status = TextView(this).apply {
            textSize = 15f
            gravity = Gravity.CENTER
            setPadding(0, pad, 0, pad / 2)
        }
        column.addView(status)

        fun button(label: String, onClick: () -> Unit) = MaterialButton(this).apply {
            text = label
            isAllCaps = false
            textSize = 17f
            setOnClickListener { onClick() }
            layoutParams = LinearLayout.LayoutParams((320 * density).toInt(), WRAP_CONTENT).apply {
                topMargin = (8 * density).toInt()
            }
        }
        play = button("Play") { launchGame() }
        column.addView(play)
        column.addView(button("Settings") { SettingsActivity.launch(this, MenuTag.SETTINGS) })
        column.addView(button("Controls") { SettingsActivity.launch(this, MenuTag.GCPAD_TYPE) })
        driverButton = button("GPU driver") { GpuDriverActivity.launch(this) }
        column.addView(driverButton)
        column.addView(button("Choose disc image…") { pickDisc.launch(arrayOf("*/*")) })

        setContentView(ScrollView(this).apply {
            isFillViewport = true
            addView(column, ViewGroup.LayoutParams(MATCH_PARENT, WRAP_CONTENT))
        })

        status.text = "Getting ready…"
        play.isEnabled = false
        DirectoryInitialization.start(this)
        AfterDirectoryInitializationRunner().runWithLifecycle(this) {
            ready = true
            refresh()
        }
    }

    override fun onResume() {
        super.onResume()
        if (ready) refresh()
        if (ColosseumNative.changeDriverRequested) {
            ColosseumNative.changeDriverRequested = false
            GpuDriverActivity.launch(this)
        }
    }

    private fun refresh() {
        val haveGame = ColosseumNative.isGameReady(gameDir.absolutePath)
        val haveModule = File(modulePath).isFile
        play.isEnabled = haveGame && haveModule
        status.text = when {
            !haveModule -> "This install is missing the game code (libgGC6E01_recomp.so). Install the full APK."
            !haveGame -> "Choose your own Pokémon Colosseum (USA) disc image to start. ISO, GCM and CISO files work. This only happens once."
            else -> "Ready."
        }
        driverButton.text = "GPU driver: ${DriverDownloader.currentDriverName(this)}"
        if (!haveGame && haveModule && !askedForDisc) {
            askedForDisc = true
            pickDisc.launch(arrayOf("*/*"))
        }
    }

    private var askedForDisc = false

    private fun unpack(uri: Uri) {
        val fd = try {
            contentResolver.openFileDescriptor(uri, "r")?.detachFd()
        } catch (e: Exception) {
            null
        }
        if (fd == null) {
            showMessage("That file could not be opened.")
            return
        }
        val bar = LinearProgressIndicator(this).apply {
            max = 100
            val pad = (24 * resources.displayMetrics.density).toInt()
            setPadding(pad, pad / 2, pad, 0)
        }
        val dialog = MaterialAlertDialogBuilder(this)
            .setTitle("Unpacking your disc")
            .setMessage("First launch only. Keep the app open.")
            .setView(bar)
            .setCancelable(false)
            .show()
        lifecycleScope.launch {
            val poll = launch {
                while (isActive) {
                    bar.setProgressCompat(ColosseumNative.getSetupProgress(), true)
                    delay(250)
                }
            }
            val error = withContext(Dispatchers.IO) {
                ColosseumNative.setupGame(fd, gameDir.absolutePath)
            }
            poll.cancel()
            dialog.dismiss()
            if (error.isNotEmpty()) showMessage(error) else refresh()
        }
    }

    private fun launchGame() {
        val error = ColosseumNative.prepareLaunch(
            gameDir.absolutePath, modulePath, DriverDownloader.currentDriverName(this)
        )
        if (error.isNotEmpty()) {
            showMessage(error)
            refresh()
            return
        }
        EmulationActivity.launch(this, File(gameDir, "sys/main.dol").absolutePath, false)
    }

    private fun showMessage(text: String) {
        MaterialAlertDialogBuilder(this)
            .setMessage(text)
            .setPositiveButton(android.R.string.ok, null)
            .show()
    }
}
