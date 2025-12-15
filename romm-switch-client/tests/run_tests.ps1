$bash = 'C:\devkitPro\msys2\usr\bin\bash.exe'
$env:CHERE_INVOKING = 'yes'

& $bash -lc @'
  cd /c/Git/SwitchRomM/romm-switch-client/tests &&
  make clean &&
  make &&
  ./romm_tests
'@
