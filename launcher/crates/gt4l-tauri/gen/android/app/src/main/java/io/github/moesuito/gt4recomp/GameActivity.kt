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

import android.app.AlertDialog
import android.content.Intent
import android.graphics.Bitmap
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.Paint
import android.graphics.Typeface
import android.graphics.drawable.GradientDrawable
import android.net.Uri
import android.os.Build
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.os.Process
import android.util.Log
import android.util.TypedValue
import android.view.Gravity
import android.view.View
import android.widget.FrameLayout
import android.widget.TextView
import android.window.OnBackInvokedDispatcher
import java.io.File
import org.libsdl.app.SDLActivity

class GameActivity : SDLActivity() {
    companion object {
        const val EXTRA_ARGUMENTS = "io.github.moesuito.gt4recomp.ARGUMENTS"
        const val EXTRA_ENVIRONMENT = "io.github.moesuito.gt4recomp.ENVIRONMENT"
        // The pause dialog's texts in the launcher's language: title, keep playing, minimize, close game.
        const val EXTRA_TEXTS = "io.github.moesuito.gt4recomp.TEXTS"
        // Set by the launcher's Resume: the activity comes back and the game goes on.
        const val EXTRA_RESUME = "io.github.moesuito.gt4recomp.RESUME"
        // "<pid> <running|paused> <epoch ms>", rewritten every 2 s (GameBridgePlugin.gameState reads it).
        const val HEARTBEAT_FILE = "game-heartbeat"
        private const val TAG = "GT4Recomp-game"

        // native/window_sdl.cpp: 1 starts a performance interval (F7), 2 writes its report (F8) to the game log.
        @JvmStatic
        external fun nativeReportRequest(request: Int)

        // native/window_sdl.cpp: true parks the game (EE stopped, card files written, audio paused), false lets it go on.
        @JvmStatic
        external fun nativeSetPaused(paused: Boolean)
    }

    private var recording = false
    private var pauseDialog: AlertDialog? = null
    private var paused = false
    private val heartbeat = Handler(Looper.getMainLooper())
    private val beat = object : Runnable {
        override fun run() {
            try {
                File(filesDir, HEARTBEAT_FILE).writeText("${Process.myPid()} ${if (paused) "paused" else "running"} ${System.currentTimeMillis()}")
            } catch (error: Exception) {
                Log.w(TAG, "heartbeat not written: $error")
            }
            heartbeat.postDelayed(this, 2000)
        }
    }

    // Load order: SDL3, then the game (the C++ runtime is linked into it); the last one holds SDL_main.
    override fun getLibraries(): Array<String> = arrayOf("SDL3", "gt4")

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        val environment = intent.getStringArrayExtra(EXTRA_ENVIRONMENT) ?: arrayOf()
        if (environment.contains("GT4_PERF=1")) addReportButton()
        // TASK-180 test2 (owner): Back pauses the game and asks what to do. Android 13+ (and every app targeting 36)
        // delivers Back through the dispatcher; older versions call onBackPressed.
        if (Build.VERSION.SDK_INT >= 33) {
            onBackInvokedDispatcher.registerOnBackInvokedCallback(OnBackInvokedDispatcher.PRIORITY_DEFAULT) { onBack() }
        }
        heartbeat.post(beat)
    }

    @Deprecated("Android 12 and older")
    override fun onBackPressed() {
        onBack()
    }

    private fun texts(): Array<String> {
        val given = intent.getStringArrayExtra(EXTRA_TEXTS) ?: arrayOf()
        return if (given.size >= 4) given else arrayOf("Game paused", "Keep playing", "Minimize", "Close game")
    }

    private fun setPaused(value: Boolean, why: String) {
        if (paused == value) return
        paused = value
        nativeSetPaused(value)
        Log.i(TAG, (if (value) "paused: " else "resumed: ") + why)
    }

    private fun onBack() {
        if (pauseDialog?.isShowing == true) {
            pauseDialog?.cancel()
            return
        }
        showPauseDialog()
    }

    private fun showPauseDialog() {
        setPaused(true, "Back")
        val (title, keep, minimize, close) = texts()
        val dialog = AlertDialog.Builder(this, android.R.style.Theme_DeviceDefault_Dialog_Alert)
            .setTitle(title)
            .setPositiveButton(keep) { _, _ -> setPaused(false, "keep playing") }
            .setNeutralButton(minimize) { _, _ -> minimize() }
            .setNegativeButton(close) { _, _ -> closeGame() }
            .setOnCancelListener { setPaused(false, "Back again") }
            .create()
        dialog.setOnDismissListener { pauseDialog = null }
        pauseDialog = dialog
        dialog.show()
        dialog.getButton(AlertDialog.BUTTON_POSITIVE)?.requestFocus()
    }

    // Minimize: the game stays alive and paused; the launcher comes back (its Play button reads Resume).
    private fun minimize() {
        Log.i(TAG, "minimize: the game stays paused in the background")
        moveTaskToBack(true)
        startActivity(Intent(this, MainActivity::class.java).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK or Intent.FLAG_ACTIVITY_REORDER_TO_FRONT))
    }

    // Close: the normal end (SDL asks the game to quit; it writes its cards and ends the process).
    private fun closeGame() {
        Log.i(TAG, "close: the game ends")
        setPaused(false, "closing")
        finish()
    }

    // The same pause path for Home, the app switcher and the notification shade (A6).
    override fun onPause() {
        setPaused(true, "the activity left the foreground")
        super.onPause()
    }

    override fun onResume() {
        super.onResume()
        if (pauseDialog?.isShowing != true) setPaused(false, "the activity is back in the foreground")
    }

    override fun onNewIntent(intent: Intent) {
        super.onNewIntent(intent)
        // The launch contract stays the first intent's; a Resume only brings the activity back.
        if (intent.getBooleanExtra(EXTRA_RESUME, false)) Log.i(TAG, "resume from the launcher")
    }

    // TASK-180 (owner, 2026-11-05): the F7/F8 performance report without a keyboard. Only while the launcher's
    // performance report setting is on (GT4_PERF=1). A small, faint button on the left edge (clear of the race HUD): a tap
    // starts an interval, the next writes the report. It never takes focus, so the controller keeps all its input.
    private fun addReportButton() {
        val size = TypedValue.applyDimension(TypedValue.COMPLEX_UNIT_DIP, 44f, resources.displayMetrics).toInt()
        val margin = TypedValue.applyDimension(TypedValue.COMPLEX_UNIT_DIP, 8f, resources.displayMetrics).toInt()
        val button = TextView(this)
        button.gravity = Gravity.CENTER
        button.setTextColor(Color.WHITE)
        button.setTextSize(TypedValue.COMPLEX_UNIT_DIP, 11f)
        button.isFocusable = false
        button.isFocusableInTouchMode = false
        button.contentDescription = "Performance report"
        fun paint() {
            val shape = GradientDrawable()
            shape.shape = GradientDrawable.OVAL
            shape.setColor(if (recording) Color.rgb(200, 30, 30) else Color.rgb(60, 60, 60))
            shape.setStroke(2, Color.WHITE)
            button.background = shape
            button.text = if (recording) "F8" else "F7"
            button.alpha = if (recording) 0.7f else 0.35f
        }
        paint()
        button.setOnClickListener {
            recording = !recording
            nativeReportRequest(if (recording) 1 else 2)
            Log.i(TAG, if (recording) "touch button: interval started (F7)" else "touch button: report written (F8)")
            paint()
        }
        val layout = FrameLayout.LayoutParams(size, size, Gravity.START or Gravity.CENTER_VERTICAL)
        layout.leftMargin = margin
        addContentView(button, layout)
        button.visibility = View.VISIBLE
    }

    // TASK-180 test2: the performance HUD's glyphs (native/hw_gs/hud/hw_hud_system_posix.cpp calls this through JNI).
    // The 95 printable ASCII glyphs of the system's monospace font, side by side in one row of cells:
    // {cell width, cell height, coverage 0-255 row-major over the whole strip}.
    @Suppress("unused")
    fun glyphAtlas(pixelHeight: Int): IntArray {
        val paint = Paint(Paint.ANTI_ALIAS_FLAG)
        paint.typeface = Typeface.MONOSPACE
        paint.textSize = pixelHeight.toFloat()
        paint.color = Color.WHITE
        val metrics = paint.fontMetricsInt
        val cellHeight = maxOf(1, metrics.descent - metrics.ascent)
        val cellWidth = maxOf(1, kotlin.math.ceil(paint.measureText("M").toDouble()).toInt())
        val glyphs = 126 - 32 + 1
        val bitmap = Bitmap.createBitmap(cellWidth * glyphs, cellHeight, Bitmap.Config.ARGB_8888)
        val canvas = Canvas(bitmap)
        for (index in 0 until glyphs) {
            canvas.drawText((32 + index).toChar().toString(), (index * cellWidth).toFloat(), (-metrics.ascent).toFloat(), paint)
        }
        val pixels = IntArray(bitmap.width * bitmap.height)
        bitmap.getPixels(pixels, 0, bitmap.width, 0, 0, bitmap.width, bitmap.height)
        bitmap.recycle()
        val out = IntArray(2 + pixels.size)
        out[0] = cellWidth
        out[1] = cellHeight
        for (i in pixels.indices) out[2 + i] = (pixels[i] ushr 24) and 0xFF
        return out
    }

    // The display's current refresh rate (phones change it on the fly), for the HUD.
    @Suppress("unused")
    fun displayRefreshHz(): Float {
        @Suppress("DEPRECATION")
        val current = (if (android.os.Build.VERSION.SDK_INT >= 30) display else null) ?: windowManager.defaultDisplay
        return current?.refreshRate ?: 0f
    }

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
            // Detached and closed at once: no stable provider reference stays (see GameBridgePlugin.openFd).
            "fd:" + (contentResolver.openFileDescriptor(Uri.parse(uri), "r")?.use { it.detachFd() } ?: -1)
        } catch (error: Exception) {
            Log.e(TAG, "cannot open $uri: $error")
            "fd:-1"
        }
    }

    override fun onDestroy() {
        heartbeat.removeCallbacks(beat)
        File(filesDir, HEARTBEAT_FILE).delete()
        Log.i(TAG, "game activity closed; the game process ends")
        super.onDestroy()
        // Fresh native state at every start: the game's globals live as long as this process.
        Process.killProcess(Process.myPid())
    }
}
