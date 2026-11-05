// SPDX-License-Identifier: MIT
// TASK-180 test3 (owner: going launcher -> game and back must feel like one app): the app icon's entry.
//
// The launcher (MainActivity) and the game (GameActivity, its own ":game" process) live in ONE task, so recents shows a
// single card and the game sits on top of the launcher while it runs. Tapping the icon, however, always starts the
// activity named by MAIN/LAUNCHER, and a launcher activity that is not at the top of its own task either resets the task
// (singleTask: the game would be destroyed) or pushes a second launcher on top of the game (singleTop). This windowless
// gate is that entry instead: it starts the right screen for the moment — the running game if there is one, else the
// launcher — and finishes at once, so the icon lands exactly where the player left off. It never draws and never starts
// Tauri, so nothing is initialised twice.
package io.github.moesuito.gt4recomp

import android.app.Activity
import android.content.Intent
import android.os.Bundle

class LaunchGateActivity : Activity() {
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        val target = if (gameAlive()) GameActivity::class.java else MainActivity::class.java
        startActivity(
            Intent(this, target)
                .addFlags(Intent.FLAG_ACTIVITY_REORDER_TO_FRONT or Intent.FLAG_ACTIVITY_NO_ANIMATION)
        )
        finish()
        @Suppress("DEPRECATION")
        overridePendingTransition(0, 0)
    }

    /// Whether the game's activity is alive: its heartbeat file (`filesDir/game-heartbeat`, rewritten every 2 s by
    /// GameActivity) is younger than five seconds. No cross-process IPC needed, and nothing here starts Tauri.
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
}
