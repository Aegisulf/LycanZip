# Prints the language codes of the program, one per line, read from src\core\include\lycan\languages.h (the one list: see the design notes "Adding a language").
$root = Split-Path $PSScriptRoot -Parent
$text = Get-Content (Join-Path $root "src\core\include\lycan\languages.h") -Raw -Encoding UTF8
[regex]::Matches($text, '\{L"([A-Za-z]{2,3}(?:-[A-Za-z0-9]+)*)", L"lang\.[a-z0-9_]+"\}') | ForEach-Object { $_.Groups[1].Value }
