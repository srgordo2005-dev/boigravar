$audioDir = "audio"
$headerFile = "esp32_boi_touch_interno\audios_progmem.h"

if (!(Test-Path $audioDir)) {
    Write-Host "Directory $audioDir not found."
    exit
}

$files = Get-ChildItem -Path $audioDir -Filter "*.mp3"

$out = [System.IO.StreamWriter]::new($headerFile)
$out.WriteLine("#ifndef AUDIOS_PROGMEM_H")
$out.WriteLine("#define AUDIOS_PROGMEM_H")
$out.WriteLine("")
$out.WriteLine("#include <pgmspace.h>")
$out.WriteLine("")

$arrayNames = @()
$arraySizes = @()
$originalNames = @()

foreach ($file in $files) {
    $arrayName = $file.Name -replace '\.mp3$', '' -replace '[^a-zA-Z0-9]', '_'
    if ($arrayName -match '^[0-9]') {
        $arrayName = "a_" + $arrayName
    }
    
    $bytes = [System.IO.File]::ReadAllBytes($file.FullName)
    
    $out.WriteLine("const unsigned char $arrayName[] PROGMEM = {")
    
    $line = "  "
    for ($i = 0; $i -lt $bytes.Length; $i++) {
        $line += "0x" + $bytes[$i].ToString("X2")
        if ($i -ne $bytes.Length - 1) {
            $line += ", "
        }
        if (($i + 1) % 16 -eq 0 -or $i -eq $bytes.Length - 1) {
            $out.WriteLine($line)
            $line = "  "
        }
    }
    
    $out.WriteLine("};")
    $out.WriteLine("")
    
    $arrayNames += $arrayName
    $arraySizes += $bytes.Length
    $originalNames += $file.Name
}

$out.WriteLine("const int num_progmem_audios = $($arrayNames.Count);")
$out.WriteLine("")

if ($arrayNames.Count -gt 0) {
    $out.WriteLine("const unsigned char* const progmem_audios[] PROGMEM = {")
    foreach ($name in $arrayNames) {
        $out.WriteLine("  $name,")
    }
    $out.WriteLine("};")
    $out.WriteLine("")
    
    $out.WriteLine("const unsigned int progmem_audio_sizes[] = {")
    foreach ($size in $arraySizes) {
        $out.WriteLine("  $size,")
    }
    $out.WriteLine("};")
    $out.WriteLine("")
    
    $out.WriteLine("const char* const progmem_audio_names[] = {")
    foreach ($name in $originalNames) {
        $out.WriteLine("  `"$name`",")
    }
    $out.WriteLine("};")
    $out.WriteLine("")
}

$out.WriteLine("#endif")
$out.Close()

Write-Host "Generated $headerFile with $($arrayNames.Count) audios."
