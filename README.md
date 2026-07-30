# readonly 

Guarantee read-only, scope what's visible, hide secrets, don't let it wander or clobber.

*UNDER CONSTRUCTION*

## install

<install instruction pending>


Setup
```
readonly setup
```

Install an AI agent — paste the install command from its official docs.
Quote it if it contains a pipe:
```
readonly install 'curl -fsSL https://claude.ai/install.sh | bash'
```

Run
```
readonly claude 
```
Read only access in current directory. By default mask .env. 

## developer
```
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build/debug-clang
```

Run
```
./build/debug-clang/cli/readonly --help
```

## TODO
g++ gives errors during compilation for debug (sanitizer off). Currently assuming dnf has too modern version. Skipping to clang for now. 

## Future
Add codex, copilot, etc
