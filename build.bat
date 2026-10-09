@echo off
rem Build the PolHook.dll proxy (32-bit -- the POL Viewer process is x86) and the
rem one-file Windows installer PolShimSetup.exe that embeds it.
setlocal

rem Find Visual Studio instead of assuming an edition. This used to hardcode
rem ...\2022\Community\..., which is right on a developer PC and wrong on a
rem GitHub runner (Enterprise), so the CI build failed before compiling a line.
rem Order: a VCVARS you set yourself, then vswhere (ships with every VS 2017+
rem installer and with the Build Tools), then the old Community path.
if defined VCVARS if exist "%VCVARS%" goto :have_vcvars
set "VCVARS="
set "VSROOT="
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" goto :try_default
for /f "usebackq delims=" %%I in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSROOT=%%I"
if defined VSROOT set "VCVARS=%VSROOT%\VC\Auxiliary\Build\vcvarsall.bat"
if defined VCVARS if exist "%VCVARS%" goto :have_vcvars
:try_default
set "VCVARS=C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat"
if exist "%VCVARS%" goto :have_vcvars
echo [!] No Visual Studio with the C++ x86 tools was found.
echo     Install "Desktop development with C++" (or the Build Tools), or run
echo     set VCVARS=^<path to vcvarsall.bat^>   before build.bat.
exit /b 1
:have_vcvars
echo [+] using "%VCVARS%"

rem `build.bat xp` makes the legacy build for Windows XP into build-xp\. It needs the
rem VS 2017 (v141) x86 tools and "C++ Windows XP Support" from the VS installer. The
rem headers come from the 7.1A SDK, and the static CRT from the 10.0.10240 UCRT,
rem the last one that does not import Vista-only kernel32 functions directly. Newer
rem UCRTs do, and pol.exe statically imports PolHook.dll, so a single unresolved
rem import on XP stops the Viewer from starting at all. /arch:IA32 because the
rem default SSE2 code faults on a Pentium III or Athlon XP.
set "OUTDIR=build"
set "XPDEFS="
set "XPLINK="
set "XPEXE="
set "XPSRC="
if /i not "%~1"=="xp" goto :vcvars_default
call "%VCVARS%" x86 -vcvars_ver=14.16 >nul
if errorlevel 1 ( echo [!] the v141 x86 tools are not installed & exit /b 1 )
set "KITS=%ProgramFiles(x86)%\Windows Kits\10"
set "SDK71=%ProgramFiles(x86)%\Microsoft SDKs\Windows\v7.1A"
if not exist "%SDK71%\Include\Windows.h" ( echo [!] the 7.1A SDK ^(C++ Windows XP Support^) is not installed & exit /b 1 )
if not exist "%KITS%\Lib\10.0.10240.0\ucrt\x86\libucrt.lib" ( echo [!] the 10.0.10240 UCRT is not installed & exit /b 1 )
set "INCLUDE=%VCToolsInstallDir%include;%KITS%\Include\10.0.10240.0\ucrt;%SDK71%\Include"
rem The current SDK's um dir goes LAST, only for libraries 7.1A lacks (dxguid.lib is
rem GUID data and imports nothing). Every import library 7.1A has is found first.
set "LIB=%VCToolsInstallDir%lib\x86;%KITS%\Lib\10.0.10240.0\ucrt\x86;%SDK71%\Lib;%KITS%\Lib\%WindowsSDKLibVersion%um\x86"
set "OUTDIR=build-xp"
set "XPDEFS=/arch:IA32 /Zc:threadSafeInit- /D_USING_V110_SDK71_ /D_WIN32_WINNT=0x0501 /DWINVER=0x0501 /DPOLSHIM_XP=1 /I..\third_party\bearssl\inc"
set "XPLINK=/SUBSYSTEM:WINDOWS,5.01 psapi.lib"
set "XPEXE=/SUBSYSTEM:CONSOLE,5.01 ws2_32.lib"
rem XP's WinINet cannot do TLS 1.2, so both binaries carry BearSSL for https://
rem (src\tlsget.cpp, trusted roots in src\tlsroots.c). Built once into bearssl.lib.
set "XPSRC=..\src\tlsget.cpp ..\src\tlsroots.c bearssl.lib"
goto :vcvars_done
:vcvars_default
call "%VCVARS%" x86 >nul
if errorlevel 1 exit /b 1
:vcvars_done

if not exist "%~dp0%OUTDIR%" mkdir "%~dp0%OUTDIR%"
pushd "%~dp0%OUTDIR%"

if not defined XPSRC goto :no_bearssl
if exist bearssl.lib goto :no_bearssl
echo [+] building BearSSL ^(once; delete build-xp\bearssl.lib to rebuild^)
if not exist bearssl-obj mkdir bearssl-obj
(for /r "%~dp0third_party\bearssl\src" %%F in (*.c) do @echo "%%F") > bearssl-src.txt
cl /nologo /c /MT /O2 /W1 %XPDEFS% /I..\third_party\bearssl\src /Fo:bearssl-obj\ @bearssl-src.txt >bearssl-build.txt
if errorlevel 1 ( type bearssl-build.txt & popd & echo [!] BearSSL build failed & exit /b 1 )
lib /nologo /out:bearssl.lib bearssl-obj\*.obj
if errorlevel 1 ( popd & echo [!] BearSSL lib failed & exit /b 1 )
:no_bearssl

rem PolHook.dll is a PROXY of the Viewer's own PolHook.dll, which pol.exe STATICALLY
rem imports -- so pol.exe loads the whole shim itself, no launcher and no admin.
rem PolHook.def forwards the original's 6 exports to the renamed PolHook_orig.dll.
rem dxhook.cpp adds the DirectDraw/DirectSound layer; dxguid.lib supplies
rem IID_IDirectDraw7 / IID_IDirectSound8 (referenced, never called).
set "SHIMSRC=..\src\inject.cpp ..\src\proxy.cpp ..\src\log.cpp ..\src\patches.cpp ..\src\fepatch.cpp ..\src\tmpathfix.cpp ..\src\authkey.cpp ..\src\dxhook.cpp ..\src\mailstore.cpp ..\src\comtrace.cpp ..\src\gamestart.cpp ..\src\d3d8hook.cpp ..\src\d3d9hook.cpp ..\src\vidfit.cpp ..\src\fmvskip.cpp ..\src\wndguard.cpp ..\src\vmrfix.cpp ..\src\dinputhook.cpp ..\src\hookspy.cpp ..\src\inputgate.cpp ..\src\cursorlock.cpp ..\src\secondlaunch.cpp ..\src\keystate.cpp ..\src\exitprompt.cpp ..\src\netredir.cpp ..\src\profiles.cpp ..\src\polfetch.cpp ..\src\fmokey.cpp ..\src\fmoime.cpp ..\src\msgxlate.cpp ..\src\uitrace.cpp ..\src\regredir.cpp ..\src\regfix.cpp ..\src\fmoiid.cpp ..\src\polfiletxt.cpp ..\src\iniheal.cpp ..\src\polsettings.cpp ..\src\gamecfg.cpp ..\src\logprune.cpp ..\src\patchver.cpp ..\src\inputmode.cpp ..\src\shortcut.cpp ..\src\padmap.cpp ..\src\padoverlay.cpp ..\src\titletag.cpp ..\src\maskguard.cpp ..\src\protondxvk.cpp ..\src\sessionwatch.cpp ..\src\autoupdate.cpp ..\src\poltoken.cpp ..\src\ffxiplug.cpp ..\src\ffxicfg.cpp ..\src\ffxi3dview.cpp ..\src\crashlog.cpp ..\src\reslock.cpp ..\src\flwindow.cpp ..\src\wakerecover.cpp ..\src\fecfg.cpp ..\src\logship.cpp ..\src\polreport.cpp ..\src\sysdiag.cpp"
set "SHIMLIBS=kernel32.lib user32.lib advapi32.lib ole32.lib dxguid.lib strmiids.lib ws2_32.lib shlwapi.lib gdi32.lib shell32.lib crypt32.lib wininet.lib bcrypt.lib comctl32.lib comdlg32.lib version.lib"
rem RELEASE VERSION. The release workflow sets POLSHIM_RELEASE_VERSION from the tag
rem (v0.1.9 -> 0.1.9), so the title bar names the same version as the Releases page.
rem Unset, as in any local build, the source's own version and the build number show.
set "RELDEFS="
if defined POLSHIM_RELEASE_VERSION set "RELDEFS=/DPOLSHIM_VERSION=\"%POLSHIM_RELEASE_VERSION%\" /DPOLSHIM_RELEASE=1"

rem PolHook.rc replicates the original PolHook.dll VS_VERSION_INFO: pol.exe
rem version-checks its DLLs, and a proxy with no version resource fails that check.
rem The forwarders name PolHook_orig.dll but do NOT need it present to LINK (a
rem forwarder is a string resolved at load time), so this builds standalone.
rc /nologo /fo PolHook.res ..\PolHook.rc
if errorlevel 1 ( popd & echo [!] PolHook.rc compile failed & exit /b 1 )
cl /nologo /LD /MT /O2 /W3 /EHsc /DWIN32 /D_CRT_SECURE_NO_WARNINGS %RELDEFS% %XPDEFS% ^
   %SHIMSRC% %XPSRC% PolHook.res /Fe:PolHook.dll ^
   /link /DEF:..\PolHook.def %SHIMLIBS% %XPLINK%

set RC=%ERRORLEVEL%
if %RC% NEQ 0 ( popd & echo [!] PolHook proxy build failed & exit /b %RC% )
echo [+] built %~dp0%OUTDIR%\PolHook.dll

rem One-file Windows installer. setup.rc embeds PolHook.dll (from THIS build dir --
rem hence built last, after the proxy exists) plus the shipping ini template
rem (dist\polshim.ini), so the download is a single file that cannot go stale
rem against its payload. The manifest is requireAdministrator: the swap writes into
rem Program Files. /I ..\src lets rc find setup.manifest and buildnum.h.
rc /nologo /I ..\src /fo setup.res ..\setup.rc
if errorlevel 1 ( popd & echo [!] setup.rc compile failed & exit /b 1 )
cl /nologo /MT /O2 /W3 /EHsc /DWIN32 /D_CRT_SECURE_NO_WARNINGS %RELDEFS% %XPDEFS% ^
   ..\src\setup.cpp ..\src\polfiletxt.cpp %XPSRC% setup.res /Fe:PolShimSetup.exe ^
   /link kernel32.lib user32.lib advapi32.lib wininet.lib bcrypt.lib /MANIFEST:NO %XPEXE%

set RC=%ERRORLEVEL%
popd
if %RC% NEQ 0 ( echo [!] PolShimSetup build failed & exit /b %RC% )
echo [+] built %~dp0%OUTDIR%\PolShimSetup.exe
exit /b 0
