// SPDX-License-Identifier: MIT
// TASK-180 (Android plan A2): the game activity. SDL3's SDLActivity creates the surface, forwards input, audio and the
// lifecycle, and calls SDL_main in libgt4.so (native/android_main.cpp) on its own thread. The game reads its files from
// the app-specific external folder (getExternalFilesDir), where adb pushes them for now (docs/ANDROID-EMULATOR.md).
package io.github.moesuito.gt4recomp;

import org.libsdl.app.SDLActivity;

public class GameActivity extends SDLActivity {
    // Load order: the C++ runtime, SDL3, then the game; the last one holds SDL_main.
    @Override
    protected String[] getLibraries() {
        return new String[] {"c++_shared", "SDL3", "gt4"};
    }

    // The game makes its own command line from the files folder (android_main.cpp); none is passed from here yet.
    @Override
    protected String[] getArguments() {
        return new String[0];
    }
}
