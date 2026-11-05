// SPDX-License-Identifier: MIT
// TASK-180: the launcher (Tauri) is the app's main activity. Landscape (AndroidManifest.xml) and full screen: the system
// bars hide and come back with a swipe, so the page keeps the PC layout.
package io.github.moesuito.gt4recomp

import android.os.Bundle
import androidx.core.view.WindowCompat
import androidx.core.view.WindowInsetsCompat
import androidx.core.view.WindowInsetsControllerCompat

class MainActivity : TauriActivity() {
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
  }

  override fun onWindowFocusChanged(hasFocus: Boolean) {
    super.onWindowFocusChanged(hasFocus)
    if (hasFocus) hideSystemBars()
  }

  private fun hideSystemBars() {
    val controller = WindowCompat.getInsetsController(window, window.decorView)
    controller.systemBarsBehavior = WindowInsetsControllerCompat.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE
    controller.hide(WindowInsetsCompat.Type.systemBars())
  }
}
