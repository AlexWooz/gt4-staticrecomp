// SPDX-License-Identifier: MIT
// TASK-180 (docs/ANDROID-PORT-PLAN.md section 4): what the launcher (Rust, src/android.rs) needs from Android itself.
// - pickDocument: the Storage Access Framework picker. The disc image keeps a persistable read permission (never copied);
//   an import is copied into a file the launcher names.
// - openFd: a read descriptor for a document the app holds a permission to (gt4l_disc reads the image through it).
// - saveDocument: "save as" for exports (saves, replays, the log bundle).
// - startGame: Play. The game activity (GameActivity, its own ":game" process) gets the launch contract: the same
//   arguments and environment the Windows launcher gives gt4vulkan.exe.
// - openUrl: links and the Android update download, in the browser.
// - deviceInfo: the phone's facts for system.txt in the log bundle (model, Android version, SoC, RAM, display).
// - gameExits: how the game's (and the launcher's) process ended (ApplicationExitInfo, Android 11+), with its tombstone or ANR trace, so a
//   crash is recorded for the log bundle even though the game died.
package io.github.moesuito.gt4recomp

import android.app.Activity
import android.app.ActivityManager
import android.app.ApplicationExitInfo
import android.content.ActivityNotFoundException
import android.content.Intent
import android.content.Context
import android.content.pm.PackageManager
import android.net.Uri
import android.os.Build
import android.os.PowerManager
import android.provider.OpenableColumns
import androidx.activity.result.ActivityResult
import app.tauri.annotation.ActivityCallback
import app.tauri.annotation.Command
import app.tauri.annotation.InvokeArg
import app.tauri.annotation.TauriPlugin
import app.tauri.plugin.Invoke
import app.tauri.plugin.JSArray
import app.tauri.plugin.JSObject
import app.tauri.plugin.Plugin
import java.io.File
import java.io.FileInputStream
import java.io.FileOutputStream

@InvokeArg
class PickArgs {
    var mimeTypes: Array<String> = arrayOf("*/*")
    var persist: Boolean = false
    var copyTo: String? = null
}

@InvokeArg
class UriArgs {
    var uri: String = ""
}

@InvokeArg
class SaveArgs {
    var source: String = ""
    var name: String = ""
    var mimeType: String = "application/octet-stream"
}

@InvokeArg
class GameArgs {
    var arguments: Array<String> = arrayOf()
    var environment: Array<String> = arrayOf()
}

@InvokeArg
class UrlArgs {
    var url: String = ""
}

@InvokeArg
class ExitsArgs {
    var since: Long = 0
    var folder: String = ""
}

@TauriPlugin
class GameBridgePlugin(private val activity: Activity) : Plugin(activity) {

    @Command
    fun pickDocument(invoke: Invoke) {
        val args = invoke.parseArgs(PickArgs::class.java)
        val intent = Intent(Intent.ACTION_OPEN_DOCUMENT).apply {
            addCategory(Intent.CATEGORY_OPENABLE)
            type = "*/*"
            putExtra(Intent.EXTRA_MIME_TYPES, args.mimeTypes)
            var flags = Intent.FLAG_GRANT_READ_URI_PERMISSION
            if (args.persist) flags = flags or Intent.FLAG_GRANT_PERSISTABLE_URI_PERMISSION
            addFlags(flags)
        }
        startActivityForResult(invoke, intent, "pickDocumentResult")
    }

    @ActivityCallback
    fun pickDocumentResult(invoke: Invoke, result: ActivityResult) {
        val uri = result.data?.data
        if (result.resultCode != Activity.RESULT_OK || uri == null) {
            invoke.resolve(JSObject())
            return
        }
        val args = invoke.parseArgs(PickArgs::class.java)
        // A copy can take a moment: off the main thread.
        Thread {
            try {
                if (args.persist) {
                    activity.contentResolver.takePersistableUriPermission(uri, Intent.FLAG_GRANT_READ_URI_PERMISSION)
                }
                val target = args.copyTo
                if (target != null) {
                    val input = activity.contentResolver.openInputStream(uri)
                        ?: throw IllegalStateException("$uri cannot be read")
                    input.use { from -> FileOutputStream(File(target)).use { to -> from.copyTo(to) } }
                }
                val answer = JSObject()
                answer.put("uri", uri.toString())
                answer.put("name", displayName(uri))
                invoke.resolve(answer)
            } catch (error: Exception) {
                invoke.reject(error.message ?: error.toString())
            }
        }.start()
    }

    @Command
    fun openFd(invoke: Invoke) {
        val args = invoke.parseArgs(UriArgs::class.java)
        try {
            val descriptor = activity.contentResolver.openFileDescriptor(Uri.parse(args.uri), "r")
            val answer = JSObject()
            // Detached: the Rust side owns the descriptor from here and closes it.
            answer.put("fd", descriptor?.detachFd() ?: -1)
            invoke.resolve(answer)
        } catch (error: Exception) {
            invoke.reject(error.message ?: error.toString())
        }
    }

    @Command
    fun saveDocument(invoke: Invoke) {
        val args = invoke.parseArgs(SaveArgs::class.java)
        val intent = Intent(Intent.ACTION_CREATE_DOCUMENT).apply {
            addCategory(Intent.CATEGORY_OPENABLE)
            type = args.mimeType
            putExtra(Intent.EXTRA_TITLE, args.name)
        }
        startActivityForResult(invoke, intent, "saveDocumentResult")
    }

    @ActivityCallback
    fun saveDocumentResult(invoke: Invoke, result: ActivityResult) {
        val uri = result.data?.data
        if (result.resultCode != Activity.RESULT_OK || uri == null) {
            invoke.resolve(JSObject().put("saved", false))
            return
        }
        val args = invoke.parseArgs(SaveArgs::class.java)
        Thread {
            try {
                val output = activity.contentResolver.openOutputStream(uri, "wt")
                    ?: throw IllegalStateException("$uri cannot be written")
                output.use { to -> FileInputStream(File(args.source)).use { from -> from.copyTo(to) } }
                invoke.resolve(JSObject().put("saved", true))
            } catch (error: Exception) {
                invoke.reject(error.message ?: error.toString())
            }
        }.start()
    }

    @Command
    fun startGame(invoke: Invoke) {
        val args = invoke.parseArgs(GameArgs::class.java)
        val intent = Intent(activity, GameActivity::class.java).apply {
            putExtra(GameActivity.EXTRA_ARGUMENTS, args.arguments)
            putExtra(GameActivity.EXTRA_ENVIRONMENT, args.environment)
        }
        activity.runOnUiThread {
            try {
                activity.startActivity(intent)
                invoke.resolve(JSObject())
            } catch (error: Exception) {
                invoke.reject(error.message ?: error.toString())
            }
        }
    }

    @Command
    fun openUrl(invoke: Invoke) {
        val args = invoke.parseArgs(UrlArgs::class.java)
        val url = Uri.parse(args.url)
        if (url.scheme != "https") {
            invoke.reject("only https links are opened")
            return
        }
        activity.runOnUiThread {
            try {
                activity.startActivity(Intent(Intent.ACTION_VIEW, url).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK))
                invoke.resolve(JSObject())
            } catch (error: ActivityNotFoundException) {
                invoke.reject("no browser to open ${args.url}")
            }
        }
    }

    @Command
    fun deviceInfo(invoke: Invoke) {
        val lines = mutableListOf<String>()
        try {
            lines.add("device: ${Build.MANUFACTURER} ${Build.MODEL} (${Build.DEVICE}, product ${Build.PRODUCT})")
            lines.add("android: ${Build.VERSION.RELEASE} (API ${Build.VERSION.SDK_INT}), security patch ${Build.VERSION.SECURITY_PATCH}")
            lines.add("build: ${Build.FINGERPRINT}")
            if (Build.VERSION.SDK_INT >= 31) {
                lines.add("soc: ${Build.SOC_MANUFACTURER} ${Build.SOC_MODEL}")
            }
            lines.add("hardware: ${Build.HARDWARE}, board ${Build.BOARD}, ABIs ${Build.SUPPORTED_ABIS.joinToString(",")}")
            lines.add("processors: ${Runtime.getRuntime().availableProcessors()}")
            val manager = activity.getSystemService(Context.ACTIVITY_SERVICE) as ActivityManager
            val memory = ActivityManager.MemoryInfo()
            manager.getMemoryInfo(memory)
            lines.add("memory: ${memory.totalMem / (1024 * 1024)} MB total, ${memory.availMem / (1024 * 1024)} MB available, " +
                "low-memory device ${manager.isLowRamDevice}, app heap limit ${manager.largeMemoryClass} MB")
            val features = activity.packageManager.systemAvailableFeatures
            fun feature(name: String): String {
                val found = features.firstOrNull { it.name == name } ?: return "none"
                return "0x" + Integer.toHexString(found.version)
            }
            lines.add("vulkan features: hardware version ${feature(PackageManager.FEATURE_VULKAN_HARDWARE_VERSION)}, " +
                "level ${feature(PackageManager.FEATURE_VULKAN_HARDWARE_LEVEL)}, " +
                "compute ${feature(PackageManager.FEATURE_VULKAN_HARDWARE_COMPUTE)}, " +
                "dEQP level ${feature(PackageManager.FEATURE_VULKAN_DEQP_LEVEL)}")
            val gles = manager.deviceConfigurationInfo.reqGlEsVersion
            lines.add("opengl es: ${gles shr 16}.${gles and 0xFFFF}")
            @Suppress("DEPRECATION")
            val display = activity.display ?: activity.windowManager.defaultDisplay
            val mode = display.mode
            val bounds = activity.windowManager.maximumWindowMetrics.bounds
            lines.add("display: ${mode.physicalWidth}x${mode.physicalHeight} at ${"%.2f".format(mode.refreshRate)} Hz, " +
                "window ${bounds.width()}x${bounds.height()}, density ${activity.resources.displayMetrics.densityDpi} dpi")
            lines.add("display modes: " + display.supportedModes.joinToString(", ") {
                "${it.physicalWidth}x${it.physicalHeight}@${"%.0f".format(it.refreshRate)}"
            })
            val power = activity.getSystemService(Context.POWER_SERVICE) as PowerManager
            lines.add("power: battery saver ${power.isPowerSaveMode}, thermal status ${power.currentThermalStatus}, " +
                "sustained performance ${power.isSustainedPerformanceModeSupported}")
            val app = activity.packageManager.getPackageInfo(activity.packageName, 0)
            lines.add("app: ${activity.packageName} ${app.versionName} (code ${app.longVersionCode})")
        } catch (error: Exception) {
            lines.add("device facts stopped: $error")
        }
        val answer = JSObject()
        answer.put("lines", JSArray(lines))
        invoke.resolve(answer)
    }

    // The ends of the app's processes (the launcher, and the game's ":game") after `since` (milliseconds since the epoch), oldest first. A native crash
    // leaves its tombstone (a protobuf, Android 12+) and an ANR its trace (text) in `folder`; the launcher turns the
    // tombstone into text for the log bundle.
    @Command
    fun gameExits(invoke: Invoke) {
        val args = invoke.parseArgs(ExitsArgs::class.java)
        val list = JSArray()
        if (Build.VERSION.SDK_INT < 30) {
            invoke.resolve(JSObject().put("exits", list))
            return
        }
        try {
            val manager = activity.getSystemService(Context.ACTIVITY_SERVICE) as ActivityManager
            val exits = manager.getHistoricalProcessExitReasons(activity.packageName, 0, 32)
                .filter { it.processName == activity.packageName || it.processName.startsWith(activity.packageName + ":") }
                .filter { it.timestamp > args.since }
                .sortedBy { it.timestamp }
            for (exit in exits) {
                val entry = JSObject()
                entry.put("time", exit.timestamp)
                entry.put("pid", exit.pid)
                entry.put("process", exit.processName)
                entry.put("reason", exit.reason)
                entry.put("reasonName", reasonName(exit.reason))
                entry.put("status", exit.status)
                entry.put("importance", exit.importance)
                entry.put("description", exit.description ?: "")
                entry.put("pssKb", exit.pss)
                entry.put("rssKb", exit.rss)
                if (args.folder.isNotEmpty()) {
                    val suffix = when (exit.reason) {
                        ApplicationExitInfo.REASON_CRASH_NATIVE -> "tombstone.pb"
                        ApplicationExitInfo.REASON_ANR -> "anr.txt"
                        else -> "trace.txt"
                    }
                    try {
                        exit.traceInputStream?.use { from ->
                            val file = File(args.folder, "game-exit-${exit.timestamp}.$suffix")
                            file.parentFile?.mkdirs()
                            FileOutputStream(file).use { to -> from.copyTo(to) }
                            entry.put("traceFile", file.absolutePath)
                        }
                    } catch (error: Exception) {
                        entry.put("traceError", error.toString())
                    }
                }
                list.put(entry)
            }
        } catch (error: Exception) {
            invoke.reject(error.message ?: error.toString())
            return
        }
        invoke.resolve(JSObject().put("exits", list))
    }

    private fun reasonName(reason: Int): String = when (reason) {
        ApplicationExitInfo.REASON_EXIT_SELF -> "exit"
        ApplicationExitInfo.REASON_SIGNALED -> "signaled"
        ApplicationExitInfo.REASON_LOW_MEMORY -> "low memory"
        ApplicationExitInfo.REASON_CRASH -> "crash (Java)"
        ApplicationExitInfo.REASON_CRASH_NATIVE -> "crash (native)"
        ApplicationExitInfo.REASON_ANR -> "not responding (ANR)"
        ApplicationExitInfo.REASON_INITIALIZATION_FAILURE -> "initialization failure"
        ApplicationExitInfo.REASON_PERMISSION_CHANGE -> "permission change"
        ApplicationExitInfo.REASON_EXCESSIVE_RESOURCE_USAGE -> "excessive resource usage"
        ApplicationExitInfo.REASON_USER_REQUESTED -> "user requested"
        ApplicationExitInfo.REASON_USER_STOPPED -> "user stopped"
        ApplicationExitInfo.REASON_DEPENDENCY_DIED -> "dependency died"
        ApplicationExitInfo.REASON_OTHER -> "other"
        else -> "reason $reason"
    }

    private fun displayName(uri: Uri): String? {
        activity.contentResolver.query(uri, arrayOf(OpenableColumns.DISPLAY_NAME), null, null, null)?.use { cursor ->
            if (cursor.moveToFirst()) return cursor.getString(0)
        }
        return null
    }
}
