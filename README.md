# art (dex2oat)

Using android Tag: android-7.1.1_r6 Variant: arm-eng-64
with art branch: `nougat-mr1-release`

```
commit 12eb0c532e33ca5d5e98addd580b5ad0a4b71be4

Author: gitbuildkicker <android-build@google.com>  2016-10-18 09:05:33
Committer: gitbuildkicker <android-build@google.com>  2016-10-18 09:05:33
Tags: android-7.1.1_r1 android-7.1.1_r2 android-7.1.1_r3 android-7.1.1_r4 android-7.1.1_r6
Parent: 645e477c2ce7a59ac98bc86463826cf082a13ad8 (Ensure OpenDexFilesFromImage closes file to prevent file descriptor leak)
Parent: eabb3d3c84d564c0c02da5077860dcd6afbd99a1 (merge in nyc-mr1-release history after reset to nyc-mr1-dev)
Branch: remotes/aosp/nougat-mr1-release
Follows: android-n-preview-2
Precedes: 
```

$ :~/aosp/aosp_7.1.1_r6_arm-eng/art$ hmm
Invoke ". build/envsetup.sh" from your shell to add the following functions to your environment:
- lunch:     lunch <product_name>-<build_variant>
- tapas:     tapas [<App1> <App2> ...] [arm|x86|mips|armv5|arm64|x86_64|mips64] [eng|userdebug|user]
- croot:     Changes directory to the top of the tree.
- m:         Makes from the top of the tree.
- mm:        Builds all of the modules in the current directory, but not their dependencies.
- mmm:       Builds all of the modules in the supplied directories, but not their dependencies.
             To limit the modules being built use the syntax: mmm dir/:target1,target2.
- mma:       Builds all of the modules in the current directory, and their dependencies.
- mmma:      Builds all of the modules in the supplied directories, and their dependencies.
- provision: Flash device with all required partitions. Options will be passed on to fastboot.
- cgrep:     Greps on all local C/C++ files.
- ggrep:     Greps on all local Gradle files.
- jgrep:     Greps on all local Java files.
- resgrep:   Greps on all local res/*.xml files.
- mangrep:   Greps on all local AndroidManifest.xml files.
- mgrep:     Greps on all local Makefiles files.
- sepgrep:   Greps on all local sepolicy files.
- sgrep:     Greps on all local source files.
- godir:     Go to the directory containing a file.

Environment options:
- SANITIZE_HOST: Set to 'true' to use ASAN for all host modules. Note that
                 ASAN_OPTIONS=detect_leaks=0 will be set by default until the
                 build is leak-check clean.

Look at the source to view more functions. The complete list is:
addcompletions add_lunch_combo build_build_var_cache cgrep check_product check_variant choosecombo chooseproduct choosetype choosevariant core coredump_enable coredump_setup cproj croot destroy_build_var_cache findmakefile get_abs_build_var getbugreports get_build_var getdriver getlastscreenshot get_make_command getprebuilt getscreenshotpath getsdcardpath gettargetarch gettop ggrep godir hmm is isviewserverstarted jgrep key_back key_home key_menu lunch _lunch m make mangrep mgrep mm mma mmm mmma pez pid printconfig print_lunch_menu provision qpid rcgrep resgrep runhat runtest sepgrep set_java_home setpaths set_sequence_number set_stuff_for_environment settitle sgrep smoketest stacks startviewserver stopviewserver systemstack tapas tracedmdump treegrep