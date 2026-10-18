// Colosseum Native for Android: the GPU driver screen.
//
// Shows the phone's GPU and the driver in use, a recommended driver for this GPU with one
// Install button, then the other available drivers (newest first) as readable cards, and
// "Install from a file" / "Use the phone's own driver".
package org.dolphinemu.dolphinemu.colosseum

import android.content.Context
import android.content.Intent
import android.graphics.Typeface
import android.net.Uri
import android.os.Bundle
import android.view.Gravity
import android.view.ViewGroup
import android.widget.LinearLayout
import android.widget.ScrollView
import android.widget.TextView
import android.widget.Toast
import androidx.activity.result.contract.ActivityResultContracts
import androidx.appcompat.app.AppCompatActivity
import androidx.core.view.ViewCompat
import androidx.core.view.WindowInsetsCompat
import androidx.core.view.updatePadding
import androidx.lifecycle.lifecycleScope
import com.google.android.material.button.MaterialButton
import com.google.android.material.card.MaterialCardView
import com.google.android.material.color.MaterialColors
import com.google.android.material.dialog.MaterialAlertDialogBuilder
import com.google.android.material.progressindicator.CircularProgressIndicator
import com.google.android.material.progressindicator.LinearProgressIndicator
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import org.dolphinemu.dolphinemu.utils.AfterDirectoryInitializationRunner
import org.dolphinemu.dolphinemu.utils.DirectoryInitialization
import org.dolphinemu.dolphinemu.utils.GpuDriverHelper
import org.dolphinemu.dolphinemu.utils.GpuDriverInstallResult
import java.io.File

class GpuDriverActivity : AppCompatActivity() {
    private lateinit var column: LinearLayout
    private val dp get() = resources.displayMetrics.density

    private val pickZip = registerForActivityResult(ActivityResultContracts.OpenDocument()) { uri ->
        if (uri != null) installFromFile(uri)
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        column = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            val pad = (20 * dp).toInt()
            setPadding(pad, pad, pad, pad)
        }
        val scroll = ScrollView(this).apply {
            addView(column, ViewGroup.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT))
        }
        ViewCompat.setOnApplyWindowInsetsListener(scroll) { v, insets ->
            val bars = insets.getInsets(WindowInsetsCompat.Type.systemBars())
            v.updatePadding(top = bars.top, bottom = bars.bottom, left = bars.left, right = bars.right)
            insets
        }
        setContentView(scroll)
        DirectoryInitialization.start(this)
        AfterDirectoryInitializationRunner().runWithLifecycle(this) { load() }
    }

    // ----- building blocks --------------------------------------------------------------
    private fun text(value: String, sizeSp: Float, bold: Boolean = false, alpha: Float = 1f) =
        TextView(this).apply {
            text = value
            textSize = sizeSp
            if (bold) setTypeface(typeface, Typeface.BOLD)
            this.alpha = alpha
        }

    private fun section(title: String) = text(title.uppercase(), 13f, bold = true, alpha = 0.75f).apply {
        setPadding(0, (22 * dp).toInt(), 0, (8 * dp).toInt())
        letterSpacing = 0.08f
    }

    private fun card(highlight: Boolean = false, content: LinearLayout.() -> Unit): MaterialCardView {
        val inner = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            val pad = (16 * dp).toInt()
            setPadding(pad, pad, pad, pad)
            content()
        }
        return MaterialCardView(this).apply {
            radius = 16 * dp
            if (highlight) {
                strokeWidth = (2 * dp).toInt()
                strokeColor = MaterialColors.getColor(this, com.google.android.material.R.attr.colorPrimary)
            }
            addView(inner)
            layoutParams = LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT
            ).apply { bottomMargin = (10 * dp).toInt() }
        }
    }

    private fun button(label: String, filled: Boolean = true, onClick: () -> Unit) =
        MaterialButton(
            this, null,
            if (filled) com.google.android.material.R.attr.materialButtonStyle
            else com.google.android.material.R.attr.materialButtonOutlinedStyle
        ).apply {
            text = label
            isAllCaps = false
            setOnClickListener { onClick() }
        }

    private fun header() {
        column.removeAllViews()
        column.addView(text("GPU driver", 26f, bold = true))
        val gpu = DriverDownloader.gpuName().ifBlank { "Unknown GPU" }
        column.addView(text("Your GPU: $gpu", 15f, alpha = 0.85f).apply { setPadding(0, (6 * dp).toInt(), 0, 0) })
        column.addView(text("In use: ${DriverDownloader.currentDriverName(this)}", 15f, alpha = 0.85f))
    }

    // ----- screen -----------------------------------------------------------------------
    private fun load() {
        header()
        if (!GpuDriverHelper.supportsCustomDriverLoading()) {
            column.addView(text(
                "\nThis phone's GPU doesn't support custom drivers. They are for Qualcomm Adreno GPUs.",
                15f
            ))
            addFooter()
            return
        }
        val spinner = CircularProgressIndicator(this).apply { isIndeterminate = true }
        column.addView(spinner, LinearLayout.LayoutParams(
            ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT
        ).apply { gravity = Gravity.CENTER_HORIZONTAL; topMargin = (32 * dp).toInt() })
        lifecycleScope.launch {
            val packages = withContext(Dispatchers.IO) { DriverDownloader.fetchPackages() }
            show(packages)
        }
    }

    private fun show(packages: List<DriverDownloader.DriverPackage>) {
        header()
        if (packages.isEmpty()) {
            column.addView(text("\nNo drivers could be listed. Check the internet connection and try again.", 15f))
            column.addView(button("Try again") { load() })
            addFooter()
            return
        }
        val recommended = DriverDownloader.recommend(packages)
        if (recommended != null) {
            val (pkg, why) = recommended
            column.addView(section("Recommended for your phone"))
            column.addView(card(highlight = true) {
                addView(text(pkg.title, 19f, bold = true))
                addView(text(pkg.subtitle(this@GpuDriverActivity), 13f, alpha = 0.75f))
                addView(text(why, 14f).apply { setPadding(0, (8 * dp).toInt(), 0, (8 * dp).toInt()) })
                addView(button("Install") { downloadAndInstall(pkg) })
            })
        }
        column.addView(section("All drivers"))
        packages.filter { it != recommended?.first }.take(30).forEach { pkg ->
            column.addView(card {
                val row = LinearLayout(this@GpuDriverActivity).apply {
                    orientation = LinearLayout.HORIZONTAL
                    gravity = Gravity.CENTER_VERTICAL
                }
                val labels = LinearLayout(this@GpuDriverActivity).apply {
                    orientation = LinearLayout.VERTICAL
                    addView(text(pkg.title, 16f, bold = true))
                    addView(text(pkg.subtitle(this@GpuDriverActivity), 13f, alpha = 0.75f))
                }
                row.addView(labels, LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f))
                row.addView(button("Install", filled = false) { downloadAndInstall(pkg) })
                addView(row)
            })
        }
        addFooter()
    }

    private fun addFooter() {
        column.addView(section("Other options"))
        column.addView(button("Install a driver from a file…", filled = false) {
            pickZip.launch(arrayOf("application/zip", "application/octet-stream", "*/*"))
        })
        column.addView(button("Use the phone's own driver", filled = false) {
            DriverDownloader.useSystemDriver()
            Toast.makeText(this, "Using the phone's own driver.", Toast.LENGTH_SHORT).show()
            load()
        })
        column.addView(text(
            "Drivers are loaded when the game starts. If one crashes or draws wrongly, come back here and pick another or use the phone's own.",
            13f, alpha = 0.7f
        ).apply { setPadding(0, (16 * dp).toInt(), 0, 0) })
    }

    // ----- actions ----------------------------------------------------------------------
    private fun downloadAndInstall(pkg: DriverDownloader.DriverPackage) {
        val bar = LinearProgressIndicator(this).apply {
            isIndeterminate = true
            val pad = (24 * dp).toInt()
            setPadding(pad, pad / 2, pad, 0)
        }
        val dialog = MaterialAlertDialogBuilder(this)
            .setTitle("Installing ${pkg.title}")
            .setView(bar)
            .setCancelable(false)
            .show()
        lifecycleScope.launch {
            val zip = File(cacheDir, "gpu_driver_download.zip")
            val message = withContext(Dispatchers.IO) {
                try {
                    DriverDownloader.download(pkg, zip) { percent ->
                        runOnUiThread {
                            if (percent >= 0) {
                                bar.isIndeterminate = false
                                bar.setProgressCompat(percent, true)
                            }
                        }
                    }
                    DriverDownloader.resultMessage(DriverDownloader.install(zip))
                } catch (e: Exception) {
                    "The download failed (${e.message})."
                } finally {
                    zip.delete()
                }
            }
            dialog.dismiss()
            Toast.makeText(this@GpuDriverActivity, message, Toast.LENGTH_LONG).show()
            load()
        }
    }

    private fun installFromFile(uri: Uri) {
        lifecycleScope.launch {
            val result = withContext(Dispatchers.IO) {
                val zip = File(cacheDir, "gpu_driver_file.zip")
                try {
                    contentResolver.openInputStream(uri)?.use { input ->
                        zip.outputStream().use { input.copyTo(it) }
                    } ?: return@withContext GpuDriverInstallResult.FileNotFound
                    DriverDownloader.install(zip)
                } finally {
                    zip.delete()
                }
            }
            Toast.makeText(this@GpuDriverActivity, DriverDownloader.resultMessage(result), Toast.LENGTH_LONG).show()
            load()
        }
    }

    companion object {
        @JvmStatic
        fun launch(context: Context) {
            context.startActivity(Intent(context, GpuDriverActivity::class.java))
        }
    }
}
