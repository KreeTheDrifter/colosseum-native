// Colosseum Native for Android: GPU driver downloader.
//
// Lists Adreno driver packages (Mesa Turnip and Qualcomm builds packaged for libadrenotools,
// with a meta.json) from the GitHub releases of driver projects, downloads the one the player
// picks and installs it with Dolphin's own driver installer. Dolphin loads it through
// libadrenotools the next time the game starts.
package org.dolphinemu.dolphinemu.colosseum

import android.app.Activity
import android.content.Context
import android.text.format.Formatter
import android.widget.Toast
import androidx.appcompat.app.AlertDialog
import androidx.lifecycle.LifecycleCoroutineScope
import com.google.android.material.dialog.MaterialAlertDialogBuilder
import com.google.android.material.progressindicator.LinearProgressIndicator
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import kotlinx.serialization.json.Json
import kotlinx.serialization.json.jsonArray
import kotlinx.serialization.json.jsonObject
import kotlinx.serialization.json.jsonPrimitive
import kotlinx.serialization.json.longOrNull
import org.dolphinemu.dolphinemu.features.settings.model.NativeConfig
import org.dolphinemu.dolphinemu.features.settings.model.StringSetting
import org.dolphinemu.dolphinemu.model.GpuDriverMetadata
import org.dolphinemu.dolphinemu.utils.GpuDriverHelper
import org.dolphinemu.dolphinemu.utils.GpuDriverInstallResult
import java.io.File
import java.io.FileInputStream
import java.io.IOException
import java.net.HttpURLConnection
import java.net.URL

object DriverDownloader {
    /** GitHub repositories whose releases carry adrenotools driver zips, newest first in each. */
    private val SOURCES = listOf(
        "K11MCH1/AdrenoToolsDrivers",
        "Weab-chan/freedreno_turnip-CI",
    )

    data class DriverPackage(
        val source: String,
        val release: String,
        val fileName: String,
        val url: String,
        val size: Long,
        val date: String,
    ) {
        fun label(context: Context): String {
            val day = if (date.length >= 10) date.substring(0, 10) else date
            val title = if (release.isNotBlank()) release else fileName
            return "$title\n$fileName  ·  ${Formatter.formatShortFileSize(context, size)}  ·  $day  ·  ${
                source.substringBefore('/')
            }"
        }
    }

    private fun get(url: String): HttpURLConnection {
        val connection = URL(url).openConnection() as HttpURLConnection
        connection.connectTimeout = 15000
        connection.readTimeout = 30000
        connection.setRequestProperty("User-Agent", "ColosseumNative-Android")
        connection.setRequestProperty("Accept", "application/vnd.github+json")
        connection.instanceFollowRedirects = true
        return connection
    }

    /** Driver packages from all sources; sources that fail are skipped. */
    fun fetchPackages(): List<DriverPackage> {
        val result = ArrayList<DriverPackage>()
        for (source in SOURCES) {
            try {
                val connection = get("https://api.github.com/repos/$source/releases?per_page=15")
                if (connection.responseCode != 200) {
                    connection.disconnect()
                    continue
                }
                val text = connection.inputStream.bufferedReader().use { it.readText() }
                connection.disconnect()
                for (release in Json.parseToJsonElement(text).jsonArray) {
                    val obj = release.jsonObject
                    val name = obj["name"]?.jsonPrimitive?.content
                        ?: obj["tag_name"]?.jsonPrimitive?.content ?: ""
                    val date = obj["published_at"]?.jsonPrimitive?.content ?: ""
                    val assets = obj["assets"]?.jsonArray ?: continue
                    for (asset in assets) {
                        val a = asset.jsonObject
                        val fileName = a["name"]?.jsonPrimitive?.content ?: continue
                        if (!fileName.endsWith(".zip", ignoreCase = true)) continue
                        val url = a["browser_download_url"]?.jsonPrimitive?.content ?: continue
                        val size = a["size"]?.jsonPrimitive?.longOrNull ?: 0L
                        result.add(DriverPackage(source, name, fileName, url, size, date))
                    }
                }
            } catch (e: Exception) {
                e.printStackTrace()
            }
        }
        return result
    }

    /** Downloads [pkg] to [dest]; [progress] gets 0-100 (or -1 when the size is unknown). */
    fun download(pkg: DriverPackage, dest: File, progress: (Int) -> Unit) {
        val connection = get(pkg.url)
        connection.setRequestProperty("Accept", "application/octet-stream")
        if (connection.responseCode != 200) {
            val code = connection.responseCode
            connection.disconnect()
            throw IOException("HTTP $code")
        }
        val total = connection.contentLengthLong.takeIf { it > 0 } ?: pkg.size
        connection.inputStream.use { input ->
            dest.outputStream().use { output ->
                val buffer = ByteArray(64 * 1024)
                var done = 0L
                var last = -2
                while (true) {
                    val n = input.read(buffer)
                    if (n < 0) break
                    output.write(buffer, 0, n)
                    done += n
                    val percent = if (total > 0) (done * 100 / total).toInt() else -1
                    if (percent != last) {
                        last = percent
                        progress(percent)
                    }
                }
            }
        }
        connection.disconnect()
    }

    /** Installs a driver zip with Dolphin's installer and selects it. */
    fun install(zip: File): GpuDriverInstallResult {
        val result = FileInputStream(zip).use { GpuDriverHelper.installDriver(it) }
        if (result == GpuDriverInstallResult.Success) {
            val meta = GpuDriverHelper.getInstalledDriverMetadata()
            StringSetting.GFX_DRIVER_LIB_NAME.setString(NativeConfig.LAYER_BASE, meta?.libraryName ?: "")
            NativeConfig.save(NativeConfig.LAYER_BASE)
        }
        return result
    }

    /** Back to the phone's own driver. */
    fun useSystemDriver() {
        GpuDriverHelper.uninstallDriver()
        StringSetting.GFX_DRIVER_LIB_NAME.setString(NativeConfig.LAYER_BASE, "")
        NativeConfig.save(NativeConfig.LAYER_BASE)
    }

    fun currentDriverName(context: Context): String {
        val meta: GpuDriverMetadata? = GpuDriverHelper.getInstalledDriverMetadata()
        if (meta != null) return "${meta.name} ${meta.driverVersion}".trim()
        val system = GpuDriverHelper.getSystemDriverMetadata(context)
        return if (system != null) "System (${system.driverVersion})" else "System"
    }

    private fun resultMessage(result: GpuDriverInstallResult): String = when (result) {
        GpuDriverInstallResult.Success -> "Driver installed. It is used from the next game start."
        GpuDriverInstallResult.InvalidArchive -> "That download isn't a valid driver package."
        GpuDriverInstallResult.MissingMetadata -> "That package has no meta.json, so it isn't made for this app's driver loader."
        GpuDriverInstallResult.InvalidMetadata -> "That package's meta.json couldn't be read."
        GpuDriverInstallResult.UnsupportedAndroidVersion -> "That driver needs a newer Android version."
        GpuDriverInstallResult.AlreadyInstalled -> "That driver is already installed."
        GpuDriverInstallResult.FileNotFound -> "The driver file couldn't be found."
    }

    /**
     * Shows the list of downloadable drivers; downloads and installs the chosen one.
     * [onDone] runs on the main thread after an install (true) or never.
     */
    fun showDownloadDialog(activity: Activity, scope: LifecycleCoroutineScope, onDone: (Boolean) -> Unit) {
        if (!GpuDriverHelper.supportsCustomDriverLoading()) {
            MaterialAlertDialogBuilder(activity)
                .setTitle("GPU drivers")
                .setMessage("This phone's graphics chip doesn't support custom drivers (they are for Qualcomm Adreno GPUs).")
                .setPositiveButton(android.R.string.ok, null)
                .show()
            return
        }
        val loading = MaterialAlertDialogBuilder(activity)
            .setTitle("GPU drivers")
            .setMessage("Looking for drivers…")
            .setCancelable(true)
            .show()
        scope.launch {
            val packages = withContext(Dispatchers.IO) { fetchPackages() }
            loading.dismiss()
            if (packages.isEmpty()) {
                MaterialAlertDialogBuilder(activity)
                    .setTitle("GPU drivers")
                    .setMessage("No drivers could be listed. Check the internet connection and try again.")
                    .setPositiveButton(android.R.string.ok, null)
                    .show()
                return@launch
            }
            val labels = packages.map { it.label(activity) }.toTypedArray()
            MaterialAlertDialogBuilder(activity)
                .setTitle("Download a GPU driver")
                .setItems(labels) { _, which -> downloadAndInstall(activity, scope, packages[which], onDone) }
                .setNegativeButton(android.R.string.cancel, null)
                .show()
        }
    }

    private fun downloadAndInstall(
        activity: Activity,
        scope: LifecycleCoroutineScope,
        pkg: DriverPackage,
        onDone: (Boolean) -> Unit
    ) {
        val bar = LinearProgressIndicator(activity).apply {
            isIndeterminate = true
            val pad = (24 * activity.resources.displayMetrics.density).toInt()
            setPadding(pad, pad / 2, pad, 0)
        }
        val dialog: AlertDialog = MaterialAlertDialogBuilder(activity)
            .setTitle("Downloading")
            .setMessage(pkg.fileName)
            .setView(bar)
            .setCancelable(false)
            .show()
        scope.launch {
            val zip = File(activity.cacheDir, "gpu_driver_download.zip")
            val message = withContext(Dispatchers.IO) {
                try {
                    download(pkg, zip) { percent ->
                        activity.runOnUiThread {
                            if (percent >= 0) {
                                bar.isIndeterminate = false
                                bar.setProgressCompat(percent, true)
                            }
                        }
                    }
                    resultMessage(install(zip))
                } catch (e: Exception) {
                    "The download failed (${e.message})."
                } finally {
                    zip.delete()
                }
            }
            dialog.dismiss()
            Toast.makeText(activity, message, Toast.LENGTH_LONG).show()
            onDone(GpuDriverHelper.getInstalledDriverMetadata() != null)
        }
    }
}
