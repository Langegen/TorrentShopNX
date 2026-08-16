$magnet = 'magnet:?xt=urn:btih:9B71DA1883526009A2BEDEAA1A85C1007D313BD1&tr=http%3A%2F%2Fbt3.t-ru.org%2Fann%3Fmagnet&dn=%5BNintendo%20Switch%5D%20Divinity%20Original%20Sin%202%20Definitive%20edition%20%5BNSP%5D%5BRUS%2FMulti7%5D'
& "C:\devkitPro\msys2\usr\bin\gdb.exe" --batch -ex "run" -ex "bt full" -ex "thread apply all bt" --args .\pctest\apptest.exe 300 $magnet 0
