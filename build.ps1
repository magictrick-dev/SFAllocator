cmake -B ./comp
cmake --build ./comp

if (-Not (Test-Path -Path "bin"))
{
    mkdir bin > $null
}

cp ./comp/Debug/sfalloc.exe ./bin
