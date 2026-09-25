# Start here

1. Unzip this whole folder somewhere you can write to (your Desktop or Documents is fine). Keep every file together in the one folder.
2. Double-click `play.bat`. A black window opens, then your browser opens the game by itself.
3. If the browser does not open, use the address the black window printed, which is usually `http://127.0.0.1:8080` (it moves to 8081, 8082 and so on if 8080 is busy).
4. You are playing Veldoria (`VEL`), the strongest country in the starting scenario. The game starts paused: press `1` to `5` to set the speed, then press `space` to unpause.
5. Click one of your provinces on the map to select it, then use the tabs at the top to look at Production, Research and Military. Close the black window when you are finished; that stops the game.

If you would rather play another country, run the game from a terminal instead: `game --play --player KOR` (any tag from `data/scenarios/1936.json`).

If `play.bat` says `game.exe was not found`, you unzipped only part of the folder. Unzip everything again, keeping the same layout, and double-click `play.bat` from inside the unzipped folder.

If Windows shows a blue "Windows protected your PC" box, click **More info**, then **Run anyway**. This game is not code-signed, so Windows warns the first time.

The black window stays open when the game stops, so an error message cannot flash past you. Press any key in it to close it once you have read the message.

## Linux and macOS

Open a terminal in this folder and run:

    ./play.sh

It does the same thing: picks a free port from 8080 upwards and starts the client, which prints the address to open (usually `http://127.0.0.1:8080`). If it says the `game` binary is missing, build one with `cmake -S . -B build && cmake --build build -j` and copy `build/game` next to `play.sh`.

The controls are the same on every platform: `1` to `5` set the speed, `space` pauses.
