// SPDX-License-Identifier: MIT
// TASK-180 test2 (owner): the shader prepare before Play, as `gt4.exe --prepare-shaders` on Windows. It runs in its own
// process (":shaders", AndroidManifest.xml) so a driver crash while compiling cannot take the launcher with it, and the
// launcher's progress card stays on screen. The launcher starts it with the launch plan's environment and a log file;
// libgt4.so writes `shaders progress D T` and `shaders ready` there and ends the process (exit 0: the cache is ready).
package io.github.moesuito.gt4recomp

import android.app.Service
import android.content.Intent
import android.os.IBinder
import android.util.Log

class ShaderPrepareService : Service() {
    companion object {
        const val EXTRA_ENVIRONMENT = "io.github.moesuito.gt4recomp.SHADER_ENVIRONMENT"
        const val EXTRA_LOG = "io.github.moesuito.gt4recomp.SHADER_LOG"

        @JvmStatic
        external fun nativePrepare(environment: Array<String>, log: String)
    }

    private var started = false

    override fun onBind(intent: Intent?): IBinder? = null

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        if (started || intent == null) return START_NOT_STICKY
        started = true
        val environment = intent.getStringArrayExtra(EXTRA_ENVIRONMENT) ?: arrayOf()
        val log = intent.getStringExtra(EXTRA_LOG) ?: File(filesDir, "shaders.log").path
        Thread {
            try {
                System.loadLibrary("SDL3")
                System.loadLibrary("gt4")
                nativePrepare(environment, log)  // ends the process
            } catch (error: Throwable) {
                Log.e("GT4Recomp-game", "shader prepare could not start: $error")
                android.os.Process.killProcess(android.os.Process.myPid())
            }
        }.start()
        return START_NOT_STICKY
    }
}

private typealias File = java.io.File
