// Colosseum Native for Android: GPU driver catalogue, download and install.
//
// Lists Adreno driver packages (Mesa Turnip and Qualcomm builds packaged for libadrenotools,
// with a meta.json) from the GitHub releases of driver projects, recommends one for the
// phone's GPU, downloads the one the player picks and installs it with Dolphin's own driver
// installer. Dolphin loads it through libadrenotools the next time the game starts.
// The screen is GpuDriverActivity.
package org.dolphinemu.dolphinemu.colosseum

import android.content.Context
import android.opengl.EGL14
import android.opengl.EGLConfig
import android.opengl.GLES20
import android.text.format.Formatter
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
import java.text.SimpleDateFormat
import java.util.Locale
import java.util.TimeZone

object DriverDownloader {
    /** GitHub repositories whose releases carry adrenotools driver zips. */
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
        val date: String,  // ISO 8601
    ) {
        val isQualcomm get() = release.contains("Qualcomm", true) || fileName.contains("qualcomm", true)
        val isTurnip get() = !isQualcomm

        /** "Turnip v26.0.0 R8" from "Turnip_v26.0.0_R8.zip". */
        val title: String
            get() = fileName.removeSuffix(".zip").removeSuffix(".ZIP").replace('_', ' ').trim()

        fun subtitle(context: Context): String {
            val kind = if (isQualcomm) "Qualcomm" else "Mesa Turnip"
            val parts = mutableListOf(kind)
            prettyDate(date)?.let { parts.add(it) }
            if (size > 0) parts.add(Formatter.formatShortFileSize(context, size))
            return parts.joinToString("  ·  ")
        }
    }

    private fun prettyDate(iso: String): String? = try {
        val parser = SimpleDateFormat("yyyy-MM-dd'T'HH:mm:ss'Z'", Locale.US)
        parser.timeZone = TimeZone.getTimeZone("UTC")
        SimpleDateFormat("d MMM yyyy", Locale.getDefault()).format(parser.parse(iso)!!)
    } catch (e: Exception) {
        null
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

    /** Driver packages from all sources, newest first; sources that fail are skipped. */
    fun fetchPackages(): List<DriverPackage> {
        val result = ArrayList<DriverPackage>()
        for (source in SOURCES) {
            try {
                val connection = get("https://api.github.com/repos/$source/releases?per_page=12")
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
        return result.sortedByDescending { it.date }
    }

    /** The GPU's name from OpenGL ES, e.g. "Adreno (TM) 750". Cached for the process. */
    @Volatile
    private var gpuName: String? = null

    fun gpuName(): String {
        gpuName?.let { return it }
        var name = ""
        try {
            val display = EGL14.eglGetDisplay(EGL14.EGL_DEFAULT_DISPLAY)
            val version = IntArray(2)
            if (EGL14.eglInitialize(display, version, 0, version, 1)) {
                val configs = arrayOfNulls<EGLConfig>(1)
                val count = IntArray(1)
                val attribs = intArrayOf(
                    EGL14.EGL_RENDERABLE_TYPE, EGL14.EGL_OPENGL_ES2_BIT,
                    EGL14.EGL_SURFACE_TYPE, EGL14.EGL_PBUFFER_BIT, EGL14.EGL_NONE
                )
                if (EGL14.eglChooseConfig(display, attribs, 0, configs, 0, 1, count, 0) && count[0] > 0) {
                    val context = EGL14.eglCreateContext(
                        display, configs[0], EGL14.EGL_NO_CONTEXT,
                        intArrayOf(EGL14.EGL_CONTEXT_CLIENT_VERSION, 2, EGL14.EGL_NONE), 0
                    )
                    val surface = EGL14.eglCreatePbufferSurface(
                        display, configs[0], intArrayOf(EGL14.EGL_WIDTH, 1, EGL14.EGL_HEIGHT, 1, EGL14.EGL_NONE), 0
                    )
                    if (EGL14.eglMakeCurrent(display, surface, surface, context))
                        name = GLES20.glGetString(GLES20.GL_RENDERER) ?: ""
                    EGL14.eglMakeCurrent(display, EGL14.EGL_NO_SURFACE, EGL14.EGL_NO_SURFACE, EGL14.EGL_NO_CONTEXT)
                    EGL14.eglDestroySurface(display, surface)
                    EGL14.eglDestroyContext(display, context)
                }
                EGL14.eglTerminate(display)
            }
        } catch (e: Exception) {
            e.printStackTrace()
        }
        gpuName = name
        return name
    }

    /** The Adreno model number (750 for "Adreno (TM) 750"), or null for other GPUs. */
    fun adrenoModel(): Int? =
        Regex("Adreno[^0-9]*([0-9]{3})").find(gpuName())?.groupValues?.get(1)?.toIntOrNull()

    /**
     * The driver to recommend for this phone, and why, or null.
     *  - Adreno 6xx and 730-799: the newest regular Mesa Turnip build.
     *  - Adreno 710-729: the newest Turnip "Gmem" build (made for those chips).
     *  - Adreno 8xx: the newest Qualcomm build (Turnip's 8xx support is still beta).
     */
    fun recommend(packages: List<DriverPackage>): Pair<DriverPackage, String>? {
        val model = adrenoModel() ?: return null
        fun special(p: DriverPackage) = listOf("gmem", "a8xx", "8xx", "handle", "autotuner", "debug")
            .any { p.fileName.contains(it, true) }
        val turnip = packages.filter { it.isTurnip }
        return when (model) {
            in 800..899 -> packages.firstOrNull { it.isQualcomm }
                ?.let { it to "Newest Qualcomm driver. Turnip's support for Adreno 8xx is still beta." }
            in 710..729 -> (turnip.firstOrNull { it.fileName.contains("gmem", true) } ?: turnip.firstOrNull { !special(it) })
                ?.let { it to "Newest Mesa Turnip build made for Adreno 710/720." }
            in 600..799 -> turnip.firstOrNull { !special(it) }
                ?.let { it to "Newest regular Mesa Turnip build: the usual pick for Adreno $model in Dolphin." }
            else -> null
        }
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
        return if (system != null) "Phone's own (${system.driverVersion})" else "Phone's own"
    }

    fun resultMessage(result: GpuDriverInstallResult): String = when (result) {
        GpuDriverInstallResult.Success -> "Driver installed. It's used from the next game start."
        GpuDriverInstallResult.InvalidArchive -> "That download isn't a valid driver package."
        GpuDriverInstallResult.MissingMetadata -> "That package has no meta.json, so it isn't made for this app's driver loader."
        GpuDriverInstallResult.InvalidMetadata -> "That package's meta.json couldn't be read."
        GpuDriverInstallResult.UnsupportedAndroidVersion -> "That driver needs a newer Android version."
        GpuDriverInstallResult.AlreadyInstalled -> "That driver is already installed."
        GpuDriverInstallResult.FileNotFound -> "The driver file couldn't be found."
    }
}
