@echo off
rem foma on Windows (Docker Desktop): fm1 build | fmt | shell | image | clean
setlocal
cd /d "%~dp0"
set IMAGE=konsumer/foma-toolchain
set CMD=%1
if "%CMD%"=="" set CMD=build
docker image inspect %IMAGE% >nul 2>&1 || docker pull --platform linux/amd64 %IMAGE%
if "%CMD%"=="build" docker run --rm --init -it --platform linux/amd64 -v "%CD%:/work" %IMAGE% python3 tools/build.py & goto :eof
if "%CMD%"=="fmt" docker run --rm --init -it --platform linux/amd64 -v "%CD%:/work" %IMAGE% sh -c "clang-format -i src/*.c platform/*.c platform/*.h && ruff format tools" & goto :eof
if "%CMD%"=="shell" docker run --rm -it --platform linux/amd64 -v "%CD%:/work" %IMAGE% bash & goto :eof
if "%CMD%"=="clean" rmdir /s /q build 2>nul & goto :eof
echo usage: fm1 build ^| fmt ^| shell ^| image ^| clean
