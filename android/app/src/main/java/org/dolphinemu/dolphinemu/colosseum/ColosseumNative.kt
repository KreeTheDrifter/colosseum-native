// Colosseum Native for Android: native calls (android/jni/ColosseumNative.cpp).
package org.dolphinemu.dolphinemu.colosseum

import org.dolphinemu.dolphinemu.NativeLibrary

object ColosseumNative {
    init {
        System.loadLibrary("main")
    }

    /** Unpacks the disc image open on [fd] (closed by the call) into [gameDir]. Returns an error or "". */
    @JvmStatic
    external fun setupGame(fd: Int, gameDir: String): String

    /** Unpacking progress, 0-100. */
    @JvmStatic
    external fun getSetupProgress(): Int

    @JvmStatic
    external fun isGameReady(gameDir: String): Boolean

    /**
     * Writes settings, patches main.dol to match the recompiled code and remembers the game
     * code library and the GPU driver name for the in-game menu. Returns an error or "".
     */
    @JvmStatic
    external fun prepareLaunch(gameDir: String, modulePath: String, gpuDriverName: String): String

    /** A tap at normalized surface coordinates. True when the in-game menu is open and used it. */
    @JvmStatic
    external fun menuTap(x: Float, y: Float): Boolean

    /** Opens the in-game menu, or closes it when it is open. */
    @JvmStatic
    external fun menuToggle()

    @JvmStatic
    external fun menuIsOpen(): Boolean

    /** Set when the in-game menu's "GPU driver" was chosen: the start screen opens the driver dialog. */
    @Volatile
    @JvmStatic
    var changeDriverRequested = false

    /** From native code (the in-game menu): leave the game to change the GPU driver. */
    @JvmStatic
    fun onChangeGpuDriver() {
        changeDriverRequested = true
        NativeLibrary.StopEmulation()
    }
}
