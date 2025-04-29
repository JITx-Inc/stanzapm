#!/usr/bin/bash
# assumption:
# folders are arranged like this
#  programming -+- stanza
#               |- stanzapm
#               |- jitx-client - stanza
cd ../stanza
./stanza.exe install -platform windows -path .
export STANZA_CONFIG=/c/programming/stanza
cd ../stanzapm
ci/bootstrap-stanza.sh
# (If not built yet, perform the two steps below)
scripts/make-asmjit.bat
cp bin/libasmjit-windows.a bin/libasmjit.a
scripts/make.sh ./stanzatemp windows compile-clean-without-finish
scripts/wfinish.bat
cp wstanza.exe stanza.exe
rm -rf pkgs
mv wpkgs pkgs
./stanza.exe install -platform windows -path .
export STANZA_CONFIG=/c/programming/stanzapm
cd ..
rm -rf jitx-client/stanza
cp -r stanzapm jitx-client/stanza