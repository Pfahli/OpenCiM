package io.github.pfahli.opencim;

import org.libsdl.app.SDLActivity;

// The game: SDL's activity, which loads libmain.so and runs SDL_main (src/app/main.cpp) with the game folder, the
// documents folder and the port's data (Paths). Full screen in landscape (AndroidManifest.xml).
public class GameActivity extends SDLActivity {
    @Override
    protected String[] getLibraries() {
        return new String[] {"main"};  // SDL3 is linked into libmain.so
    }

    @Override
    protected String[] getArguments() {
        return new String[] {
            Paths.game(this).getAbsolutePath(),
            "--documents", Paths.documents(this).getAbsolutePath(),
            "--data", Paths.data(this).getAbsolutePath(),
        };
    }
}
