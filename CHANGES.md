# Changes

Short summary of the main changes. Execute `CHANGES.sh` for a detailed diff.

The following files were modified initially with the changes of artist.

The changes are designed to be very minimal and consist of an added command-line flag to `dex2oat` and artists
initialization functions.

We also do a check-sum rewriting in oat files (there's the command-line flag for) in order to compile the oat files
from a dex-file that's not in the apk's original location.

## /compiler/oat_writer.cc

- OatWriter::OatWriter
    - Probe DexCheckSums
    - Rewrite DexChecksums

## /compiler/optimizing/optimizing_compiler.cc 

- includes
- adding HArtist Optimization passes

## /dex2oat/dex2oat.cc

Probing if DexLocationChecksum should get rewritten

## runtime/base/logging.cc

Replaced LogTag with dex2artist.gitignore

## All Changed/Touched Files

- build/Android.bp
- compiler/Android.bp
- compiler/oat_writer.cc
- compiler/oat_writer.h
- compiler/optimizing/artist
- compiler/optimizing/optimizing_compiler.cc
- dex2oat/dex2oat.cc
- runtime/base/arena_object.h
- runtime/base/logging.cc
- runtime/base/logging.h
- runtime/base/macros.h
- runtime/class_linker.h
- runtime/parsed_options.cc
- .gitignore
- .gitmodules
- CHANGES.md
- NOTICE
- README.md