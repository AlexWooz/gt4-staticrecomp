// SPDX-License-Identifier: MIT
// TASK-180 (docs/ANDROID-PORT-PLAN.md section 4): the game. SDL3's SDLActivity creates the surface, forwards input, audio
// and the lifecycle, and calls SDL_main in libgt4.so (native/android_main.cpp) on its own thread. It runs in its own
// process (":game", AndroidManifest.xml), started only by the launcher's Play (GameBridgePlugin.startGame) with the
// launch contract:
// - EXTRA_ENVIRONMENT: NAME=VALUE entries, set in the process environment before SDL_main runs (game.ini values, paths);
// - EXTRA_ARGUMENTS: the command line. A content:// argument (the disc image, a document the app holds a permission to)
//   is opened here and passed as fd:<descriptor>; the game reads the image through it.
// The process ends with the activity, so every start gets fresh native state.
package io.github.moesuito.gt4recomp

import android.net.Uri
import android.os.Process
import android.util.Log
import org.libsdl.app.SDLActivity

class GameActivity : SDLActivity() {
    companion object {
        const val EXTRA_ARGUMENTS = "io.github.moesuito.gt4recomp.ARGUMENTS"
        const val EXTRA_ENVIRONMENT = "io.github.moesuito.gt4recomp.ENVIRONMENT"
        private const val TAG = "GT4Recomp-game"
    }

    // Load order: the C++ runtime, SDL3, then the game; the last one holds SDL_main.
    override fun getLibraries(): Array<String> = arrayOf("c++_shared", "SDL3", "gt4")

    // Called on SDL's thread, after the libraries are loaded and before SDL_main.
    override fun getArguments(): Array<String> {
        for (entry in intent.getStringArrayExtra(EXTRA_ENVIRONMENT) ?: arrayOf()) {
            val equals = entry.indexOf('=')
            if (equals > 0) SDLActivity.nativeSetenv(entry.substring(0, equals), entry.substring(equals + 1))
        }
        Log.i(TAG, "launch contract: ${intent.getStringArrayExtra(EXTRA_ARGUMENTS)?.size ?: 0} arguments, " +
            "${intent.getStringArrayExtra(EXTRA_ENVIRONMENT)?.size ?: 0} environment entries")
        return (intent.getStringArrayExtra(EXTRA_ARGUMENTS) ?: arrayOf()).map { argument ->
            if (argument.startsWith("content://")) descriptorArgument(argument) else argument
        }.toTypedArray()
    }

    private fun descriptorArgument(uri: String): String {
        return try {
            val descriptor = contentResolver.openFileDescriptor(Uri.parse(uri), "r")
            "fd:" + (descriptor?.detachFd() ?: -1)
        } catch (error: Exception) {
            Log.e(TAG, "cannot open $uri: $error")
            "fd:-1"
        }
    }

    override fun onDestroy() {
        Log.i(TAG, "game activity closed; the game process ends")
        super.onDestroy()
        // Fresh native state at every start: the game's globals live as long as this process.
        Process.killProcess(Process.myPid())
    }
}
