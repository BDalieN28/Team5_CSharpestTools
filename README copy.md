# Sample Student Engine (C) - UCI

This is a sample UCI chess engine with minimal requirements/functionalities.

- Implements UCI over stdin/stdout
- Parses `position startpos ...` and `position fen ...`
- Generates legal moves (basic rules). Promotions -> queen only.
- No opening book, no advanced search: picks the first legal move.

## Build
./build.sh

## Run
./run.sh
