// SPDX-License-Identifier: MIT
// TASK-180: the launcher (Tauri) is the app's main activity. Landscape (AndroidManifest.xml) and full screen: the system
// bars hide and come back with a swipe, so the page keeps the PC layout.
package io.github.moesuito.gt4recomp

import android.os.Bundle
import androidx.core.view.WindowCompat
import androidx.core.view.WindowInsetsCompat
import androidx.core.view.WindowInsetsControllerCompat

class MainActivity : TauriActivity() {
  private var page: android.webkit.WebView? = null

  // TASK-180 test2: the page holds the input focus from the start (and again when the launcher comes back), so the first
  // controller or key press acts on the page instead of only moving Android's focus to the WebView.
  override fun onWebViewCreate(webView: android.webkit.WebView) {
    page = webView
    webView.isFocusable = true
    webView.isFocusableInTouchMode = true
    webView.post { webView.requestFocus() }
  }

  override fun onResume() {
    super.onResume()
    page?.post { page?.requestFocus() }
  }

  override fun onCreate(savedInstanceState: Bundle?) {
    publishDisplay()
    super.onCreate(savedInstanceState)
    hideSystemBars()
  }

  // TASK-180 test2: the launcher's display model takes the phone's screen as its monitor (100% Render scale = the screen's
  // native output). The current mode, in landscape, goes to the environment before the launcher library starts.
  private fun publishDisplay() {
    try {
      @Suppress("DEPRECATION")
      val display = (if (android.os.Build.VERSION.SDK_INT >= 30) display else null) ?: windowManager.defaultDisplay
      val mode = display.mode
      val wide = maxOf(mode.physicalWidth, mode.physicalHeight)
      val tall = minOf(mode.physicalWidth, mode.physicalHeight)
      android.system.Os.setenv("GT4_ANDROID_DISPLAY", "${wide}x${tall}@${mode.refreshRate}", true)
    } catch (error: Exception) {
      android.util.Log.w("GT4Recomp-launcher", "display mode not known: $error")
    }
    // TASK-180 test3 (owner's phone): the device's language for the launcher's "Auto" setting. Rust reads no locale on
    // Android; without this the launcher fell back to English while the page was Portuguese (the pause dialog was English).
    try {
      val tag = java.util.Locale.getDefault().toLanguageTag()
      android.system.Os.setenv("GT4_ANDROID_LOCALE", tag, true)
    } catch (error: Exception) {
      android.util.Log.w("GT4Recomp-launcher", "locale not known: $error")
    }
  }

  override fun onWindowFocusChanged(hasFocus: Boolean) {
    super.onWindowFocusChanged(hasFocus)
    if (hasFocus) hideSystemBars()
  }

  // TASK-180 test3 (owner: one app): the game runs in this launcher's own task, below the launcher after Minimize. Back
  // on the launcher would finish this activity and take the whole task (and the live game) with it, so while a game is
  // alive Back leaves the app in the background instead, exactly as it does when the game is on top.
  override fun onBackPressed() {
    if (gameAlive()) {
      moveTaskToBack(true)
      return
    }
    @Suppress("DEPRECATION")
    super.onBackPressed()
  }

  /// Whether the game's activity is alive: its heartbeat file (`filesDir/game-heartbeat`, rewritten every 2 s by
  /// GameActivity) is younger than five seconds. No cross-process IPC needed.
  private fun gameAlive(): Boolean {
    return try {
      val file = java.io.File(filesDir, "game-heartbeat")
      if (!file.isFile) {
        false
      } else {
        val stamp = file.readText().trim().split(' ').lastOrNull()?.toLongOrNull() ?: return false
        System.currentTimeMillis() - stamp < 5000
      }
    } catch (error: Exception) {
      false
    }
  }

  private fun hideSystemBars() {
    val controller = WindowCompat.getInsetsController(window, window.decorView)
    controller.systemBarsBehavior = WindowInsetsControllerCompat.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE
    controller.hide(WindowInsetsCompat.Type.systemBars())
  }
}
