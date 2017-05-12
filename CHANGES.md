# Changes

Short summary of the main changes. Execute `CHANGES.sh` for a detailed diff.

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

## Changed/Touched Files

compiler/Android.mk  
compiler/driver/compiler_driver.cc  
compiler/oat_writer.cc  
compiler/oat_writer.h  
compiler/optimizing/optimizing_compiler.cc  
dex2oat/dex2oat.cc  
runtime/base/logging.cc  
runtime/base/logging.h  
runtime/class_linker.h  
runtime/parsed_options.cc  
