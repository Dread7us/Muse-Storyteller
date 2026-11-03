"""Preview the optional Windows helper; --register builds/registers it locally.

Run from a Visual Studio C/C++ developer shell. This never loads a Chrome
extension automatically or starts/restarts ADB, Chrome, or SuperDisplay.
"""
import argparse
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys

ROOT=Path(__file__).resolve().parents[1]

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--extension-id',required=True,help='ID of your unpacked integrations/chrome extension')
    parser.add_argument('--adb',required=True,type=Path,help='The adb.exe already used by your existing server')
    parser.add_argument('--register',action='store_true')
    args=parser.parse_args()
    if not re.fullmatch('[a-p]{32}',args.extension_id): raise SystemExit('Invalid Chrome extension ID.')
    if not args.register:
        print('Preview only: would compile a hidden native launcher and register one HKCU native host. Your local paths/extension ID stay in private/.');return
    if os.name!='nt' or not shutil.which('cl'): raise SystemExit('Use a Windows Visual Studio developer shell with cl.exe.')
    if not args.adb.is_file(): raise SystemExit('ADB path does not exist.')
    import winreg
    private=ROOT/'private'; private.mkdir(exist_ok=True)
    origin='chrome-extension://'+args.extension_id+'/'
    config={'workspace':str(ROOT),'adb':str(args.adb.resolve()),'python':sys.executable,'origin':origin}
    config_path=private/'usage-host.json'
    if config_path.exists() and json.loads(config_path.read_text())!=config: raise SystemExit('Existing helper configuration differs; review it before replacing.')
    config_path.write_text(json.dumps(config,indent=2))
    def wide(value): return 'L'+json.dumps(value)
    code='''#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <wchar.h>
int wmain(int argc,wchar_t **argv){
 if(argc<2||wcscmp(argv[1],ORIGIN))return 4;
 wchar_t command[8192];
 if(_snwprintf_s(command,8192,_TRUNCATE,L"\\\"%s\\\" -I -B \\\"%s\\\" \\\"%s\\\"",PYTHON,SCRIPT,ORIGIN)<0)return 5;
 STARTUPINFOW si={0};PROCESS_INFORMATION pi={0};si.cb=sizeof(si);si.dwFlags=STARTF_USESTDHANDLES;
 si.hStdInput=GetStdHandle(STD_INPUT_HANDLE);si.hStdOutput=GetStdHandle(STD_OUTPUT_HANDLE);si.hStdError=GetStdHandle(STD_ERROR_HANDLE);
 if(!CreateProcessW(PYTHON,command,NULL,NULL,TRUE,CREATE_NO_WINDOW,NULL,NULL,&si,&pi))return 6;
 WaitForSingleObject(pi.hProcess,INFINITE);DWORD result=7;GetExitCodeProcess(pi.hProcess,&result);CloseHandle(pi.hThread);CloseHandle(pi.hProcess);return (int)result;
}'''
    source=private/'usage_launcher.c'
    source.write_text('#define ORIGIN '+wide(origin)+'\n#define PYTHON '+wide(sys.executable)+'\n#define SCRIPT '+wide(str(ROOT/'integrations/windows/native_host.py'))+'\n'+code)
    executable=private/'muse_usage_host.exe'
    subprocess.run(['cl','/nologo','/W4','/O2',str(source),'/Fe:'+str(executable),'/Fo:'+str(private/'usage_launcher.obj')],check=True,cwd=private)
    manifest=private/'native-host.json'
    manifest.write_text(json.dumps({'name':'com.dread7us.muse_storyteller','description':'Local Muse Storyteller usage check','path':str(executable),'type':'stdio','allowed_origins':[origin]},indent=2))
    key=r'Software\Google\Chrome\NativeMessagingHosts\com.dread7us.muse_storyteller'
    with winreg.CreateKey(winreg.HKEY_CURRENT_USER,key) as handle: winreg.SetValueEx(handle,'',0,winreg.REG_SZ,str(manifest))
    print('Registered for this Windows user. Reload your unpacked extension manually. No device or browser services were restarted.')

if __name__=='__main__': main()
