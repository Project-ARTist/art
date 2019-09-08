/*
 * Copyright (C) 2014 The Android Open Source Project
 *
 * Changes Copyright (C) 2017 CISPA (https://cispa.saarland), Saarland University
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include "base/memory_tool.h"

#include <fstream>
#include <iostream>
#include <limits>
#include <sstream>
#include <string>
#include <unordered_set>
#include <vector>
#include <cstring>

#if defined(__linux__) && defined(__arm__)
#include <sys/personality.h>
#include <sys/utsname.h>
#endif

#include "android-base/stringprintf.h"
#include "android-base/strings.h"

#include "arch/instruction_set_features.h"
#include "arch/mips/instruction_set_features_mips.h"
#include "art_method-inl.h"
#include "base/callee_save_type.h"
#include "base/dumpable.h"
#include "base/macros.h"
#include "base/scoped_flock.h"
#include "base/stl_util.h"
#include "base/stringpiece.h"
#include "base/time_utils.h"
#include "base/timing_logger.h"
#include "base/unix_file/fd_file.h"
#include "class_linker.h"
#include "class_loader_context.h"
#include "compiler.h"
#include "compiler_callbacks.h"
#include "debug/elf_debug_writer.h"
#include "debug/method_debug_info.h"
#include "dex/quick_compiler_callbacks.h"
#include "dex/verification_results.h"
#include "dex2oat_return_codes.h"
#include "dex_file-inl.h"
#include "driver/compiler_driver.h"
#include "driver/compiler_options.h"
#include "elf_file.h"
#include "elf_writer.h"
#include "elf_writer_quick.h"
#include "gc/space/image_space.h"
#include "gc/space/space-inl.h"
#include "gc/verification.h"
#include "image_writer.h"
#include "interpreter/unstarted_runtime.h"
#include "java_vm_ext.h"
#include "jit/profile_compilation_info.h"
#include "leb128.h"
#include "linker/buffered_output_stream.h"
#include "linker/file_output_stream.h"
#include "linker/multi_oat_relative_patcher.h"
#include "mirror/class-inl.h"
#include "mirror/class_loader.h"
#include "mirror/object-inl.h"
#include "mirror/object_array-inl.h"
#include "nativehelper/ScopedLocalRef.h"
#include "oat_file.h"
#include "oat_file_assistant.h"
#include "oat_writer.h"
#include "os.h"
#include "runtime.h"
#include "runtime_options.h"
#include "scoped_thread_state_change-inl.h"
#include "utils.h"
#include "vdex_file.h"
#include "verifier/verifier_deps.h"
#include "well_known_classes.h"
#include "zip_archive.h"

namespace art {

using android::base::StringAppendV;
using android::base::StringPrintf;

static constexpr size_t kDefaultMinDexFilesForSwap = 2;
static constexpr size_t kDefaultMinDexFileCumulativeSizeForSwap = 20 * MB;

// Compiler filter override for very large apps.
static constexpr CompilerFilter::Filter kLargeAppFilter = CompilerFilter::kVerify;

static int original_argc;
static char** original_argv;

static std::string CommandLine() {
  std::vector<std::string> command;
  for (int i = 0; i < original_argc; ++i) {
    command.push_back(original_argv[i]);
  }
  return android::base::Join(command, ' ');
}

// A stripped version. Remove some less essential parameters. If we see a "--zip-fd=" parameter, be
// even more aggressive. There won't be much reasonable data here for us in that case anyways (the
// locations are all staged).
static std::string StrippedCommandLine() {
  std::vector<std::string> command;

  // Do a pre-pass to look for zip-fd and the compiler filter.
  bool saw_zip_fd = false;
  bool saw_compiler_filter = false;
  for (int i = 0; i < original_argc; ++i) {
    if (android::base::StartsWith(original_argv[i], "--zip-fd=")) {
      saw_zip_fd = true;
    }
    if (android::base::StartsWith(original_argv[i], "--compiler-filter=")) {
      saw_compiler_filter = true;
    }
  }

  // Now filter out things.
  for (int i = 0; i < original_argc; ++i) {
    // All runtime-arg parameters are dropped.
    if (strcmp(original_argv[i], "--runtime-arg") == 0) {
      i++;  // Drop the next part, too.
      continue;
    }

    // Any instruction-setXXX is dropped.
    if (android::base::StartsWith(original_argv[i], "--instruction-set")) {
      continue;
    }

    // The boot image is dropped.
    if (android::base::StartsWith(original_argv[i], "--boot-image=")) {
      continue;
    }

    // The image format is dropped.
    if (android::base::StartsWith(original_argv[i], "--image-format=")) {
      continue;
    }

    // This should leave any dex-file and oat-file options, describing what we compiled.

    // However, we prefer to drop this when we saw --zip-fd.
    if (saw_zip_fd) {
      // Drop anything --zip-X, --dex-X, --oat-X, --swap-X, or --app-image-X
      if (android::base::StartsWith(original_argv[i], "--zip-") ||
          android::base::StartsWith(original_argv[i], "--dex-") ||
          android::base::StartsWith(original_argv[i], "--oat-") ||
          android::base::StartsWith(original_argv[i], "--swap-") ||
          android::base::StartsWith(original_argv[i], "--app-image-")) {
        continue;
      }
    }

    command.push_back(original_argv[i]);
  }

  if (!saw_compiler_filter) {
    command.push_back("--compiler-filter=" +
        CompilerFilter::NameOfFilter(CompilerFilter::kDefaultCompilerFilter));
  }

  // Construct the final output.
  if (command.size() <= 1U) {
    // It seems only "/system/bin/dex2oat" is left, or not even that. Use a pretty line.
    return "Starting dex2oat.";
  }
  return android::base::Join(command, ' ');
}

static void UsageErrorV(const char* fmt, va_list ap) {
  std::string error;
  StringAppendV(&error, fmt, ap);
  LOG(ERROR) << error;
}

static void UsageError(const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  UsageErrorV(fmt, ap);
  va_end(ap);
}

NO_RETURN static void Usage(const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  UsageErrorV(fmt, ap);
  va_end(ap);

  UsageError("Command: %s", CommandLine().c_str());

  UsageError("Usage: dex2oat [options]...");
  UsageError("");
  UsageError("  -j<number>: specifies the number of threads used for compilation.");
  UsageError("       Default is the number of detected hardware threads available on the");
  UsageError("       host system.");
  UsageError("      Example: -j12");
  UsageError("");
  UsageError("  --dex-file=<dex-file>: specifies a .dex, .jar, or .apk file to compile.");
  UsageError("      Example: --dex-file=/system/framework/core.jar");
  UsageError("");
  UsageError("  --dex-location=<dex-location>: specifies an alternative dex location to");
  UsageError("      encode in the oat file for the corresponding --dex-file argument.");
  UsageError("      Example: --dex-file=/home/build/out/system/framework/core.jar");
  UsageError("               --dex-location=/system/framework/core.jar");
  UsageError("");
  UsageError("  --zip-fd=<file-descriptor>: specifies a file descriptor of a zip file");
  UsageError("      containing a classes.dex file to compile.");
  UsageError("      Example: --zip-fd=5");
  UsageError("");
  UsageError("  --zip-location=<zip-location>: specifies a symbolic name for the file");
  UsageError("      corresponding to the file descriptor specified by --zip-fd.");
  UsageError("      Example: --zip-location=/system/app/Calculator.apk");
  UsageError("");
  UsageError("  --oat-file=<file.oat>: specifies an oat output destination via a filename.");
  UsageError("      Example: --oat-file=/system/framework/boot.oat");
  UsageError("");
  UsageError("  --oat-fd=<number>: specifies the oat output destination via a file descriptor.");
  UsageError("      Example: --oat-fd=6");
  UsageError("");
  UsageError("  --oat-location=<oat-name>: specifies a symbolic name for the file corresponding");
  UsageError("      to the file descriptor specified by --oat-fd.");
  UsageError("      Example: --oat-location=/data/dalvik-cache/system@app@Calculator.apk.oat");
  UsageError("");
  UsageError("  --oat-symbols=<file.oat>: specifies an oat output destination with full symbols.");
  UsageError("      Example: --oat-symbols=/symbols/system/framework/boot.oat");
  UsageError("");
  UsageError("  --image=<file.art>: specifies an output image filename.");
  UsageError("      Example: --image=/system/framework/boot.art");
  UsageError("");
  UsageError("  --image-format=(uncompressed|lz4|lz4hc):");
  UsageError("      Which format to store the image.");
  UsageError("      Example: --image-format=lz4");
  UsageError("      Default: uncompressed");
  UsageError("");
  UsageError("  --image-classes=<classname-file>: specifies classes to include in an image.");
  UsageError("      Example: --image=frameworks/base/preloaded-classes");
  UsageError("");
  UsageError("  --base=<hex-address>: specifies the base address when creating a boot image.");
  UsageError("      Example: --base=0x50000000");
  UsageError("");
  UsageError("  --boot-image=<file.art>: provide the image file for the boot class path.");
  UsageError("      Do not include the arch as part of the name, it is added automatically.");
  UsageError("      Example: --boot-image=/system/framework/boot.art");
  UsageError("               (specifies /system/framework/<arch>/boot.art as the image file)");
  UsageError("      Default: $ANDROID_ROOT/system/framework/boot.art");
  UsageError("");
  UsageError("  --android-root=<path>: used to locate libraries for portable linking.");
  UsageError("      Example: --android-root=out/host/linux-x86");
  UsageError("      Default: $ANDROID_ROOT");
  UsageError("");
  UsageError("  --instruction-set=(arm|arm64|mips|mips64|x86|x86_64): compile for a particular");
  UsageError("      instruction set.");
  UsageError("      Example: --instruction-set=x86");
  UsageError("      Default: arm");
  UsageError("");
  UsageError("  --instruction-set-features=...,: Specify instruction set features");
  UsageError("      Example: --instruction-set-features=div");
  UsageError("      Default: default");
  UsageError("");
  UsageError("  --compile-pic: Force indirect use of code, methods, and classes");
  UsageError("      Default: disabled");
  UsageError("");
  UsageError("  --checksum-rewriting: Force dex2oat to write a fake location checksum (write the original");
  UsageError("      checksum instead of the correct location checksum of the modified APK file)");
  UsageError("      Default: disabled");
  UsageError("");
  UsageError("  --compiler-backend=(Quick|Optimizing): select compiler backend");
  UsageError("      set.");
  UsageError("      Example: --compiler-backend=Optimizing");
  UsageError("      Default: Optimizing");
  UsageError("");
  UsageError("  --compiler-filter="
                "(assume-verified"
                "|extract"
                "|verify"
                "|quicken"
                "|space-profile"
                "|space"
                "|speed-profile"
                "|speed"
                "|everything-profile"
                "|everything):");
  UsageError("      select compiler filter.");
  UsageError("      Example: --compiler-filter=everything");
  UsageError("      Default: speed");
  UsageError("");
  UsageError("  --huge-method-max=<method-instruction-count>: threshold size for a huge");
  UsageError("      method for compiler filter tuning.");
  UsageError("      Example: --huge-method-max=%d", CompilerOptions::kDefaultHugeMethodThreshold);
  UsageError("      Default: %d", CompilerOptions::kDefaultHugeMethodThreshold);
  UsageError("");
  UsageError("  --large-method-max=<method-instruction-count>: threshold size for a large");
  UsageError("      method for compiler filter tuning.");
  UsageError("      Example: --large-method-max=%d", CompilerOptions::kDefaultLargeMethodThreshold);
  UsageError("      Default: %d", CompilerOptions::kDefaultLargeMethodThreshold);
  UsageError("");
  UsageError("  --small-method-max=<method-instruction-count>: threshold size for a small");
  UsageError("      method for compiler filter tuning.");
  UsageError("      Example: --small-method-max=%d", CompilerOptions::kDefaultSmallMethodThreshold);
  UsageError("      Default: %d", CompilerOptions::kDefaultSmallMethodThreshold);
  UsageError("");
  UsageError("  --tiny-method-max=<method-instruction-count>: threshold size for a tiny");
  UsageError("      method for compiler filter tuning.");
  UsageError("      Example: --tiny-method-max=%d", CompilerOptions::kDefaultTinyMethodThreshold);
  UsageError("      Default: %d", CompilerOptions::kDefaultTinyMethodThreshold);
  UsageError("");
  UsageError("  --num-dex-methods=<method-count>: threshold size for a small dex file for");
  UsageError("      compiler filter tuning. If the input has fewer than this many methods");
  UsageError("      and the filter is not interpret-only or verify-none or verify-at-runtime, ");
  UsageError("      overrides the filter to use speed");
  UsageError("      Example: --num-dex-method=%d", CompilerOptions::kDefaultNumDexMethodsThreshold);
  UsageError("      Default: %d", CompilerOptions::kDefaultNumDexMethodsThreshold);
  UsageError("");
  UsageError("  --inline-max-code-units=<code-units-count>: the maximum code units that a method");
  UsageError("      can have to be considered for inlining. A zero value will disable inlining.");
  UsageError("      Honored only by Optimizing. Has priority over the --compiler-filter option.");
  UsageError("      Intended for development/experimental use.");
  UsageError("      Example: --inline-max-code-units=%d",
             CompilerOptions::kDefaultInlineMaxCodeUnits);
  UsageError("      Default: %d", CompilerOptions::kDefaultInlineMaxCodeUnits);
  UsageError("");
  UsageError("  --dump-timing: display a breakdown of where time was spent");
  UsageError("");
  UsageError("  -g");
  UsageError("  --generate-debug-info: Generate debug information for native debugging,");
  UsageError("      such as stack unwinding information, ELF symbols and DWARF sections.");
  UsageError("      If used without --debuggable, it will be best-effort only.");
  UsageError("      This option does not affect the generated code. (disabled by default)");
  UsageError("");
  UsageError("  --no-generate-debug-info: Do not generate debug information for native debugging.");
  UsageError("");
  UsageError("  --generate-mini-debug-info: Generate minimal amount of LZMA-compressed");
  UsageError("      debug information necessary to print backtraces. (disabled by default)");
  UsageError("");
  UsageError("  --no-generate-mini-debug-info: Do not generate backtrace info.");
  UsageError("");
  UsageError("  --generate-build-id: Generate GNU-compatible linker build ID ELF section with");
  UsageError("      SHA-1 of the file content (and thus stable across identical builds)");
  UsageError("");
  UsageError("  --no-generate-build-id: Do not generate the build ID ELF section.");
  UsageError("");
  UsageError("  --debuggable: Produce code debuggable with Java debugger.");
  UsageError("");
  UsageError("  --avoid-storing-invocation: Avoid storing the invocation args in the key value");
  UsageError("      store. Used to test determinism with different args.");
  UsageError("");
  UsageError("  --runtime-arg <argument>: used to specify various arguments for the runtime,");
  UsageError("      such as initial heap size, maximum heap size, and verbose output.");
  UsageError("      Use a separate --runtime-arg switch for each argument.");
  UsageError("      Example: --runtime-arg -Xms256m");
  UsageError("");
  UsageError("  --profile-file=<filename>: specify profiler output file to use for compilation.");
  UsageError("");
  UsageError("  --profile-file-fd=<number>: same as --profile-file but accepts a file descriptor.");
  UsageError("      Cannot be used together with --profile-file.");
  UsageError("");
  UsageError("  --swap-file=<file-name>: specifies a file to use for swap.");
  UsageError("      Example: --swap-file=/data/tmp/swap.001");
  UsageError("");
  UsageError("  --swap-fd=<file-descriptor>: specifies a file to use for swap (by descriptor).");
  UsageError("      Example: --swap-fd=10");
  UsageError("");
  UsageError("  --swap-dex-size-threshold=<size>: specifies the minimum total dex file size in");
  UsageError("      bytes to allow the use of swap.");
  UsageError("      Example: --swap-dex-size-threshold=1000000");
  UsageError("      Default: %zu", kDefaultMinDexFileCumulativeSizeForSwap);
  UsageError("");
  UsageError("  --swap-dex-count-threshold=<count>: specifies the minimum number of dex files to");
  UsageError("      allow the use of swap.");
  UsageError("      Example: --swap-dex-count-threshold=10");
  UsageError("      Default: %zu", kDefaultMinDexFilesForSwap);
  UsageError("");
  UsageError("  --very-large-app-threshold=<size>: specifies the minimum total dex file size in");
  UsageError("      bytes to consider the input \"very large\" and reduce compilation done.");
  UsageError("      Example: --very-large-app-threshold=100000000");
  UsageError("");
  UsageError("  --app-image-fd=<file-descriptor>: specify output file descriptor for app image.");
  UsageError("      Example: --app-image-fd=10");
  UsageError("");
  UsageError("  --app-image-file=<file-name>: specify a file name for app image.");
  UsageError("      Example: --app-image-file=/data/dalvik-cache/system@app@Calculator.apk.art");
  UsageError("");
  UsageError("  --multi-image: specify that separate oat and image files be generated for each "
             "input dex file.");
  UsageError("");
  UsageError("  --force-determinism: force the compiler to emit a deterministic output.");
  UsageError("");
  UsageError("  --dump-cfg=<cfg-file>: dump control-flow graphs (CFGs) to specified file.");
  UsageError("      Example: --dump-cfg=output.cfg");
  UsageError("");
  UsageError("  --dump-cfg-append: when dumping CFGs to an existing file, append new CFG data to");
  UsageError("      existing data (instead of overwriting existing data with new data, which is");
  UsageError("      the default behavior). This option is only meaningful when used with");
  UsageError("      --dump-cfg.");
  UsageError("");
  UsageError("  --classpath-dir=<directory-path>: directory used to resolve relative class paths.");
  UsageError("");
  UsageError("  --class-loader-context=<string spec>: a string specifying the intended");
  UsageError("      runtime loading context for the compiled dex files.");
  UsageError("");
  UsageError("      It describes how the class loader chain should be built in order to ensure");
  UsageError("      classes are resolved during dex2aot as they would be resolved at runtime.");
  UsageError("      This spec will be encoded in the oat file. If at runtime the dex file is");
  UsageError("      loaded in a different context, the oat file will be rejected.");
  UsageError("");
  UsageError("      The chain is interpreted in the natural 'parent order', meaning that class");
  UsageError("      loader 'i+1' will be the parent of class loader 'i'.");
  UsageError("      The compilation sources will be appended to the classpath of the first class");
  UsageError("      loader.");
  UsageError("");
  UsageError("      E.g. if the context is 'PCL[lib1.dex];DLC[lib2.dex]' and ");
  UsageError("      --dex-file=src.dex then dex2oat will setup a PathClassLoader with classpath ");
  UsageError("      'lib1.dex:src.dex' and set its parent to a DelegateLastClassLoader with ");
  UsageError("      classpath 'lib2.dex'.");
  UsageError("      ");
  UsageError("      Note that the compiler will be tolerant if the source dex files specified");
  UsageError("      with --dex-file are found in the classpath. The source dex files will be");
  UsageError("      removed from any class loader's classpath possibly resulting in empty");
  UsageError("      class loaders.");
  UsageError("");
  UsageError("      Example: --class-loader-context=PCL[lib1.dex:lib2.dex];DLC[lib3.dex]");
  UsageError("");
  UsageError("  --dirty-image-objects=<directory-path>: list of known dirty objects in the image.");
  UsageError("      The image writer will group them together.");
  UsageError("");
  std::cerr << "See log for usage error information\n";
  exit(EXIT_FAILURE);
}

// The primary goal of the watchdog is to prevent stuck build servers
// during development when fatal aborts lead to a cascade of failures
// that result in a deadlock.
class WatchDog {
// WatchDog defines its own CHECK_PTHREAD_CALL to avoid using LOG which uses locks
#undef CHECK_PTHREAD_CALL
#define CHECK_WATCH_DOG_PTHREAD_CALL(call, args, what) \
  do { \
    int rc = call args; \
    if (rc != 0) { \
      errno = rc; \
      std::string message(# call); \
      message += " failed for "; \
      message += reason; \
      Fatal(message); \
    } \
  } while (false)

 public:
  explicit WatchDog(int64_t timeout_in_milliseconds)
      : timeout_in_milliseconds_(timeout_in_milliseconds),
        shutting_down_(false) {
    const char* reason = "dex2oat watch dog thread startup";
    CHECK_WATCH_DOG_PTHREAD_CALL(pthread_mutex_init, (&mutex_, nullptr), reason);
#ifndef __APPLE__
    pthread_condattr_t condattr;
    CHECK_WATCH_DOG_PTHREAD_CALL(pthread_condattr_init, (&condattr), reason);
    CHECK_WATCH_DOG_PTHREAD_CALL(pthread_condattr_setclock, (&condattr, CLOCK_MONOTONIC), reason);
    CHECK_WATCH_DOG_PTHREAD_CALL(pthread_cond_init, (&cond_, &condattr), reason);
    CHECK_WATCH_DOG_PTHREAD_CALL(pthread_condattr_destroy, (&condattr), reason);
#endif
    CHECK_WATCH_DOG_PTHREAD_CALL(pthread_attr_init, (&attr_), reason);
    CHECK_WATCH_DOG_PTHREAD_CALL(pthread_create, (&pthread_, &attr_, &CallBack, this), reason);
    CHECK_WATCH_DOG_PTHREAD_CALL(pthread_attr_destroy, (&attr_), reason);
  }
  ~WatchDog() {
    const char* reason = "dex2oat watch dog thread shutdown";
    CHECK_WATCH_DOG_PTHREAD_CALL(pthread_mutex_lock, (&mutex_), reason);
    shutting_down_ = true;
    CHECK_WATCH_DOG_PTHREAD_CALL(pthread_cond_signal, (&cond_), reason);
    CHECK_WATCH_DOG_PTHREAD_CALL(pthread_mutex_unlock, (&mutex_), reason);

    CHECK_WATCH_DOG_PTHREAD_CALL(pthread_join, (pthread_, nullptr), reason);

    CHECK_WATCH_DOG_PTHREAD_CALL(pthread_cond_destroy, (&cond_), reason);
    CHECK_WATCH_DOG_PTHREAD_CALL(pthread_mutex_destroy, (&mutex_), reason);
  }

  // TODO: tune the multiplier for GC verification, the following is just to make the timeout
  //       large.
  static constexpr int64_t kWatchdogVerifyMultiplier =
      kVerifyObjectSupport > kVerifyObjectModeFast ? 100 : 1;

  // When setting timeouts, keep in mind that the build server may not be as fast as your
  // desktop. Debug builds are slower so they have larger timeouts.
  static constexpr int64_t kWatchdogSlowdownFactor = kIsDebugBuild ? 5U : 1U;

  // 9.5 minutes scaled by kSlowdownFactor. This is slightly smaller than the Package Manager
  // watchdog (PackageManagerService.WATCHDOG_TIMEOUT, 10 minutes), so that dex2oat will abort
  // itself before that watchdog would take down the system server.
  static constexpr int64_t kWatchDogTimeoutSeconds = kWatchdogSlowdownFactor * (9 * 60 + 30);

  static constexpr int64_t kDefaultWatchdogTimeoutInMS =
      kWatchdogVerifyMultiplier * kWatchDogTimeoutSeconds * 1000;

 private:
  static void* CallBack(void* arg) {
    WatchDog* self = reinterpret_cast<WatchDog*>(arg);
    ::art::SetThreadName("dex2oat watch dog");
    self->Wait();
    return nullptr;
  }

  NO_RETURN static void Fatal(const std::string& message) {
    // TODO: When we can guarantee it won't prevent shutdown in error cases, move to LOG. However,
    //       it's rather easy to hang in unwinding.
    //       LogLine also avoids ART logging lock issues, as it's really only a wrapper around
    //       logcat logging or stderr output.
    android::base::LogMessage::LogLine(__FILE__,
                                       __LINE__,
                                       android::base::LogId::DEFAULT,
                                       LogSeverity::FATAL,
                                       message.c_str());
    // If we're on the host, try to dump all threads to get a sense of what's going on. This is
    // restricted to the host as the dump may itself go bad.
    // TODO: Use a double watchdog timeout, so we can enable this on-device.
    if (!kIsTargetBuild && Runtime::Current() != nullptr) {
      Runtime::Current()->AttachCurrentThread("Watchdog thread attached for dumping",
                                              true,
                                              nullptr,
                                              false);
      Runtime::Current()->DumpForSigQuit(std::cerr);
    }
    exit(1);
  }

  void Wait() {
    timespec timeout_ts;
#if defined(__APPLE__)
    InitTimeSpec(true, CLOCK_REALTIME, timeout_in_milliseconds_, 0, &timeout_ts);
#else
    InitTimeSpec(true, CLOCK_MONOTONIC, timeout_in_milliseconds_, 0, &timeout_ts);
#endif
    const char* reason = "dex2oat watch dog thread waiting";
    CHECK_WATCH_DOG_PTHREAD_CALL(pthread_mutex_lock, (&mutex_), reason);
    while (!shutting_down_) {
      int rc = TEMP_FAILURE_RETRY(pthread_cond_timedwait(&cond_, &mutex_, &timeout_ts));
      if (rc == ETIMEDOUT) {
        Fatal(StringPrintf("dex2oat did not finish after %" PRId64 " seconds",
                           timeout_in_milliseconds_/1000));
      } else if (rc != 0) {
        std::string message(StringPrintf("pthread_cond_timedwait failed: %s",
                                         strerror(errno)));
        Fatal(message.c_str());
      }
    }
    CHECK_WATCH_DOG_PTHREAD_CALL(pthread_mutex_unlock, (&mutex_), reason);
  }

  // TODO: Switch to Mutex when we can guarantee it won't prevent shutdown in error cases.
  pthread_mutex_t mutex_;
  pthread_cond_t cond_;
  pthread_attr_t attr_;
  pthread_t pthread_;

  const int64_t timeout_in_milliseconds_;
  bool shutting_down_;
};

class Dex2Oat FINAL {
 public:
  explicit Dex2Oat(TimingLogger* timings) :
      compiler_kind_(Compiler::kOptimizing),
      instruction_set_(kRuntimeISA == kArm ? kThumb2 : kRuntimeISA),
      // Take the default set of instruction features from the build.
      image_file_location_oat_checksum_(0),
      image_file_location_oat_data_begin_(0),
      image_patch_delta_(0),
      key_value_store_(nullptr),
      verification_results_(nullptr),
      runtime_(nullptr),
      thread_count_(sysconf(_SC_NPROCESSORS_CONF)),
      start_ns_(NanoTime()),
      start_cputime_ns_(ProcessCpuNanoTime()),
      oat_fd_(-1),
      input_vdex_fd_(-1),
      output_vdex_fd_(-1),
      input_vdex_file_(nullptr),
      zip_fd_(-1),
      image_base_(0U),
      image_classes_zip_filename_(nullptr),
      image_classes_filename_(nullptr),
      image_storage_mode_(ImageHeader::kStorageModeUncompressed),
      compiled_classes_zip_filename_(nullptr),
      compiled_classes_filename_(nullptr),
      compiled_methods_zip_filename_(nullptr),
      compiled_methods_filename_(nullptr),
      passes_to_run_filename_(nullptr),
      dirty_image_objects_filename_(nullptr),
      multi_image_(false),
      is_host_(false),
      elf_writers_(),
      oat_writers_(),
      rodata_(),
      image_writer_(nullptr),
      driver_(nullptr),
      rewrite_dex_location_checksums_(false),
      opened_dex_files_maps_(),
      opened_dex_files_(),
      no_inline_from_dex_files_(),
      dump_stats_(false),
      dump_passes_(false),
      dump_timing_(false),
      dump_slow_timing_(kIsDebugBuild),
      avoid_storing_invocation_(false),
      swap_fd_(kInvalidFd),
      app_image_fd_(kInvalidFd),
      profile_file_fd_(kInvalidFd),
      timings_(timings),
      force_determinism_(false)
      {}

  ~Dex2Oat() {
    // Log completion time before deleting the runtime_, because this accesses
    // the runtime.
    LogCompletionTime();

    if (!kIsDebugBuild && !(RUNNING_ON_MEMORY_TOOL && kMemoryToolDetectsLeaks)) {
      // We want to just exit on non-debug builds, not bringing the runtime down
      // in an orderly fashion. So release the following fields.
      driver_.release();
      image_writer_.release();
      for (std::unique_ptr<const DexFile>& dex_file : opened_dex_files_) {
        dex_file.release();
      }
      for (std::unique_ptr<MemMap>& map : opened_dex_files_maps_) {
        map.release();
      }
      for (std::unique_ptr<File>& vdex_file : vdex_files_) {
        vdex_file.release();
      }
      for (std::unique_ptr<File>& oat_file : oat_files_) {
        oat_file.release();
      }
      runtime_.release();
      verification_results_.release();
      key_value_store_.release();
    }
  }

  struct ParserOptions {
    std::vector<const char*> oat_symbols;
    std::string boot_image_filename;
    int64_t watch_dog_timeout_in_ms = -1;
    bool watch_dog_enabled = true;
    bool requested_specific_compiler = false;
    std::string error_msg;
  };

  void ParseZipFd(const StringPiece& option) {
    ParseUintOption(option, "--zip-fd", &zip_fd_, Usage);
  }

  void ParseInputVdexFd(const StringPiece& option) {
    // Note that the input vdex fd might be -1.
    ParseIntOption(option, "--input-vdex-fd", &input_vdex_fd_, Usage);
  }

  void ParseOutputVdexFd(const StringPiece& option) {
    ParseUintOption(option, "--output-vdex-fd", &output_vdex_fd_, Usage);
  }

  void ParseOatFd(const StringPiece& option) {
    ParseUintOption(option, "--oat-fd", &oat_fd_, Usage);
  }

  void ParseFdForCollection(const StringPiece& option,
                            const char* arg_name,
                            std::vector<uint32_t>* fds) {
    uint32_t fd;
    ParseUintOption(option, arg_name, &fd, Usage);
    fds->push_back(fd);
  }

  void ParseJ(const StringPiece& option) {
    ParseUintOption(option, "-j", &thread_count_, Usage, /* is_long_option */ false);
  }

  void ParseBase(const StringPiece& option) {
    DCHECK(option.starts_with("--base="));
    const char* image_base_str = option.substr(strlen("--base=")).data();
    char* end;
    image_base_ = strtoul(image_base_str, &end, 16);
    if (end == image_base_str || *end != '\0') {
      Usage("Failed to parse hexadecimal value for option %s", option.data());
    }
  }

  void ParseInstructionSet(const StringPiece& option) {
    DCHECK(option.starts_with("--instruction-set="));
    StringPiece instruction_set_str = option.substr(strlen("--instruction-set=")).data();
    // StringPiece is not necessarily zero-terminated, so need to make a copy and ensure it.
    std::unique_ptr<char[]> buf(new char[instruction_set_str.length() + 1]);
    strncpy(buf.get(), instruction_set_str.data(), instruction_set_str.length());
    buf.get()[instruction_set_str.length()] = 0;
    instruction_set_ = GetInstructionSetFromString(buf.get());
    // arm actually means thumb2.
    if (instruction_set_ == InstructionSet::kArm) {
      instruction_set_ = InstructionSet::kThumb2;
    }
  }

  void ParseInstructionSetVariant(const StringPiece& option, ParserOptions* parser_options) {
    DCHECK(option.starts_with("--instruction-set-variant="));
    StringPiece str = option.substr(strlen("--instruction-set-variant=")).data();
    instruction_set_features_ = InstructionSetFeatures::FromVariant(
        instruction_set_, str.as_string(), &parser_options->error_msg);
    if (instruction_set_features_.get() == nullptr) {
      Usage("%s", parser_options->error_msg.c_str());
    }
  }

  void ParseInstructionSetFeatures(const StringPiece& option, ParserOptions* parser_options) {
    DCHECK(option.starts_with("--instruction-set-features="));
    StringPiece str = option.substr(strlen("--instruction-set-features=")).data();
    if (instruction_set_features_ == nullptr) {
      instruction_set_features_ = InstructionSetFeatures::FromVariant(
          instruction_set_, "default", &parser_options->error_msg);
      if (instruction_set_features_.get() == nullptr) {
        Usage("Problem initializing default instruction set features variant: %s",
              parser_options->error_msg.c_str());
      }
    }
    instruction_set_features_ =
        instruction_set_features_->AddFeaturesFromString(str.as_string(),
                                                         &parser_options->error_msg);
    if (instruction_set_features_ == nullptr) {
      Usage("Error parsing '%s': %s", option.data(), parser_options->error_msg.c_str());
    }
  }

  void ParseCompilerBackend(const StringPiece& option, ParserOptions* parser_options) {
    DCHECK(option.starts_with("--compiler-backend="));
    parser_options->requested_specific_compiler = true;
    StringPiece backend_str = option.substr(strlen("--compiler-backend=")).data();
    if (backend_str == "Quick") {
      compiler_kind_ = Compiler::kQuick;
    } else if (backend_str == "Optimizing") {
      compiler_kind_ = Compiler::kOptimizing;
    } else {
      Usage("Unknown compiler backend: %s", backend_str.data());
    }
  }

  void ParseImageFormat(const StringPiece& option) {
    const StringPiece substr("--image-format=");
    DCHECK(option.starts_with(substr));
    const StringPiece format_str = option.substr(substr.length());
    if (format_str == "lz4") {
      image_storage_mode_ = ImageHeader::kStorageModeLZ4;
    } else if (format_str == "lz4hc") {
      image_storage_mode_ = ImageHeader::kStorageModeLZ4HC;
    } else if (format_str == "uncompressed") {
      image_storage_mode_ = ImageHeader::kStorageModeUncompressed;
    } else {
      Usage("Unknown image format: %s", format_str.data());
    }
  }

  void ProcessOptions(ParserOptions* parser_options) {
    compiler_options_->boot_image_ = !image_filenames_.empty();
    compiler_options_->app_image_ = app_image_fd_ != -1 || !app_image_file_name_.empty();

    if (IsAppImage() && IsBootImage()) {
      Usage("Can't have both --image and (--app-image-fd or --app-image-file)");
    }

    if (oat_filenames_.empty() && oat_fd_ == -1) {
      Usage("Output must be supplied with either --oat-file or --oat-fd");
    }

    if (input_vdex_fd_ != -1 && !input_vdex_.empty()) {
      Usage("Can't have both --input-vdex-fd and --input-vdex");
    }

    if (output_vdex_fd_ != -1 && !output_vdex_.empty()) {
      Usage("Can't have both --output-vdex-fd and --output-vdex");
    }

    if (!oat_filenames_.empty() && oat_fd_ != -1) {
      Usage("--oat-file should not be used with --oat-fd");
    }

    if ((output_vdex_fd_ == -1) != (oat_fd_ == -1)) {
      Usage("VDEX and OAT output must be specified either with one --oat-filename "
            "or with --oat-fd and --output-vdex-fd file descriptors");
    }

    if (!parser_options->oat_symbols.empty() && oat_fd_ != -1) {
      Usage("--oat-symbols should not be used with --oat-fd");
    }

    if (!parser_options->oat_symbols.empty() && is_host_) {
      Usage("--oat-symbols should not be used with --host");
    }

    if (output_vdex_fd_ != -1 && !image_filenames_.empty()) {
      Usage("--output-vdex-fd should not be used with --image");
    }

    if (oat_fd_ != -1 && !image_filenames_.empty()) {
      Usage("--oat-fd should not be used with --image");
    }

    if (!parser_options->oat_symbols.empty() &&
        parser_options->oat_symbols.size() != oat_filenames_.size()) {
      Usage("--oat-file arguments do not match --oat-symbols arguments");
    }

    if (!image_filenames_.empty() && image_filenames_.size() != oat_filenames_.size()) {
      Usage("--oat-file arguments do not match --image arguments");
    }

    if (android_root_.empty()) {
      const char* android_root_env_var = getenv("ANDROID_ROOT");
      if (android_root_env_var == nullptr) {
        Usage("--android-root unspecified and ANDROID_ROOT not set");
      }
      android_root_ += android_root_env_var;
    }

    if (!IsBootImage() && parser_options->boot_image_filename.empty()) {
      parser_options->boot_image_filename += android_root_;
      parser_options->boot_image_filename += "/framework/boot.art";
    }
    if (!parser_options->boot_image_filename.empty()) {
      boot_image_filename_ = parser_options->boot_image_filename;
    }

    if (image_classes_filename_ != nullptr && !IsBootImage()) {
      Usage("--image-classes should only be used with --image");
    }

    if (image_classes_filename_ != nullptr && !boot_image_filename_.empty()) {
      Usage("--image-classes should not be used with --boot-image");
    }

    if (image_classes_zip_filename_ != nullptr && image_classes_filename_ == nullptr) {
      Usage("--image-classes-zip should be used with --image-classes");
    }

    if (compiled_classes_filename_ != nullptr && !IsBootImage()) {
      Usage("--compiled-classes should only be used with --image");
    }

    if (compiled_classes_filename_ != nullptr && !boot_image_filename_.empty()) {
      Usage("--compiled-classes should not be used with --boot-image");
    }

    if (compiled_classes_zip_filename_ != nullptr && compiled_classes_filename_ == nullptr) {
      Usage("--compiled-classes-zip should be used with --compiled-classes");
    }

    if (dex_filenames_.empty() && zip_fd_ == -1) {
      Usage("Input must be supplied with either --dex-file or --zip-fd");
    }

    if (!dex_filenames_.empty() && zip_fd_ != -1) {
      Usage("--dex-file should not be used with --zip-fd");
    }

    if (!dex_filenames_.empty() && !zip_location_.empty()) {
      Usage("--dex-file should not be used with --zip-location");
    }

    if (dex_locations_.empty()) {
      for (const char* dex_file_name : dex_filenames_) {
        dex_locations_.push_back(dex_file_name);
      }
    } else if (dex_locations_.size() != dex_filenames_.size()) {
      Usage("--dex-location arguments do not match --dex-file arguments");
    }

    if (!dex_filenames_.empty() && !oat_filenames_.empty()) {
      if (oat_filenames_.size() != 1 && oat_filenames_.size() != dex_filenames_.size()) {
        Usage("--oat-file arguments must be singular or match --dex-file arguments");
      }
    }

    if (zip_fd_ != -1 && zip_location_.empty()) {
      Usage("--zip-location should be supplied with --zip-fd");
    }

    if (boot_image_filename_.empty()) {
      if (image_base_ == 0) {
        Usage("Non-zero --base not specified");
      }
    }

    const bool have_profile_file = !profile_file_.empty();
    const bool have_profile_fd = profile_file_fd_ != kInvalidFd;
    if (have_profile_file && have_profile_fd) {
      Usage("Profile file should not be specified with both --profile-file-fd and --profile-file");
    }

    if (have_profile_file || have_profile_fd) {
      if (compiled_classes_filename_ != nullptr ||
          compiled_classes_zip_filename_ != nullptr ||
          image_classes_filename_ != nullptr ||
          image_classes_zip_filename_ != nullptr) {
        Usage("Profile based image creation is not supported with image or compiled classes");
      }
    }

    if (!parser_options->oat_symbols.empty()) {
      oat_unstripped_ = std::move(parser_options->oat_symbols);
    }

    // If no instruction set feature was given, use the default one for the target
    // instruction set.
    if (instruction_set_features_.get() == nullptr) {
      instruction_set_features_ = InstructionSetFeatures::FromVariant(
         instruction_set_, "default", &parser_options->error_msg);
      if (instruction_set_features_.get() == nullptr) {
        Usage("Problem initializing default instruction set features variant: %s",
              parser_options->error_msg.c_str());
      }
    }

    if (instruction_set_ == kRuntimeISA) {
      std::unique_ptr<const InstructionSetFeatures> runtime_features(
          InstructionSetFeatures::FromCppDefines());
      if (!instruction_set_features_->Equals(runtime_features.get())) {
        LOG(WARNING) << "Mismatch between dex2oat instruction set features ("
            << *instruction_set_features_ << ") and those of dex2oat executable ("
            << *runtime_features <<") for the command line:\n"
            << CommandLine();
      }
    }

    if (compiler_options_->inline_max_code_units_ == CompilerOptions::kUnsetInlineMaxCodeUnits) {
      compiler_options_->inline_max_code_units_ = CompilerOptions::kDefaultInlineMaxCodeUnits;
    }

    // Checks are all explicit until we know the architecture.
    // Set the compilation target's implicit checks options.
    switch (instruction_set_) {
      case kArm:
      case kThumb2:
      case kArm64:
      case kX86:
      case kX86_64:
      case kMips:
      case kMips64:
        compiler_options_->implicit_null_checks_ = true;
        compiler_options_->implicit_so_checks_ = true;
        break;

      default:
        // Defaults are correct.
        break;
    }

    if (!IsBootImage() && multi_image_) {
      Usage("--multi-image can only be used when creating boot images");
    }
    if (IsBootImage() && multi_image_ && image_filenames_.size() > 1) {
      Usage("--multi-image cannot be used with multiple image names");
    }

    // For now, if we're on the host and compile the boot image, *always* use multiple image files.
    if (!kIsTargetBuild && IsBootImage()) {
      if (image_filenames_.size() == 1) {
        multi_image_ = true;
      }
    }

    // Done with usage checks, enable watchdog if requested
    if (parser_options->watch_dog_enabled) {
      int64_t timeout = parser_options->watch_dog_timeout_in_ms > 0
                            ? parser_options->watch_dog_timeout_in_ms
                            : WatchDog::kDefaultWatchdogTimeoutInMS;
      watchdog_.reset(new WatchDog(timeout));
    }

    // Fill some values into the key-value store for the oat header.
    key_value_store_.reset(new SafeMap<std::string, std::string>());

    // Automatically force determinism for the boot image in a host build if read barriers
    // are enabled, or if the default GC is CMS or MS. When the default GC is CMS
    // (Concurrent Mark-Sweep), the GC is switched to a non-concurrent one by passing the
    // option `-Xgc:nonconcurrent` (see below).
    if (!kIsTargetBuild && IsBootImage()) {
      if (SupportsDeterministicCompilation()) {
        force_determinism_ = true;
      } else {
        LOG(WARNING) << "Deterministic compilation is disabled.";
      }
    }
    compiler_options_->force_determinism_ = force_determinism_;

    if (passes_to_run_filename_ != nullptr) {
      passes_to_run_.reset(ReadCommentedInputFromFile<std::vector<std::string>>(
          passes_to_run_filename_,
          nullptr));         // No post-processing.
      if (passes_to_run_.get() == nullptr) {
        Usage("Failed to read list of passes to run.");
      }
    }
    compiler_options_->passes_to_run_ = passes_to_run_.get();
  }

  static bool SupportsDeterministicCompilation() {
    return (kUseReadBarrier ||
            gc::kCollectorTypeDefault == gc::kCollectorTypeCMS ||
            gc::kCollectorTypeDefault == gc::kCollectorTypeMS);
  }

  void ExpandOatAndImageFilenames() {
    std::string base_oat = oat_filenames_[0];
    size_t last_oat_slash = base_oat.rfind('/');
    if (last_oat_slash == std::string::npos) {
      Usage("--multi-image used with unusable oat filename %s", base_oat.c_str());
    }
    // We also need to honor path components that were encoded through '@'. Otherwise the loading
    // code won't be able to find the images.
    if (base_oat.find('@', last_oat_slash) != std::string::npos) {
      last_oat_slash = base_oat.rfind('@');
    }
    base_oat = base_oat.substr(0, last_oat_slash + 1);

    std::string base_img = image_filenames_[0];
    size_t last_img_slash = base_img.rfind('/');
    if (last_img_slash == std::string::npos) {
      Usage("--multi-image used with unusable image filename %s", base_img.c_str());
    }
    // We also need to honor path components that were encoded through '@'. Otherwise the loading
    // code won't be able to find the images.
    if (base_img.find('@', last_img_slash) != std::string::npos) {
      last_img_slash = base_img.rfind('@');
    }

    // Get the prefix, which is the primary image name (without path components). Strip the
    // extension.
    std::string prefix = base_img.substr(last_img_slash + 1);
    if (prefix.rfind('.') != std::string::npos) {
      prefix = prefix.substr(0, prefix.rfind('.'));
    }
    if (!prefix.empty()) {
      prefix = prefix + "-";
    }

    base_img = base_img.substr(0, last_img_slash + 1);

    // Note: we have some special case here for our testing. We have to inject the differentiating
    //       parts for the different core images.
    std::string infix;  // Empty infix by default.
    {
      // Check the first name.
      std::string dex_file = oat_filenames_[0];
      size_t last_dex_slash = dex_file.rfind('/');
      if (last_dex_slash != std::string::npos) {
        dex_file = dex_file.substr(last_dex_slash + 1);
      }
      size_t last_dex_dot = dex_file.rfind('.');
      if (last_dex_dot != std::string::npos) {
        dex_file = dex_file.substr(0, last_dex_dot);
      }
      if (android::base::StartsWith(dex_file, "core-")) {
        infix = dex_file.substr(strlen("core"));
      }
    }

    std::string base_symbol_oat;
    if (!oat_unstripped_.empty()) {
      base_symbol_oat = oat_unstripped_[0];
      size_t last_symbol_oat_slash = base_symbol_oat.rfind('/');
      if (last_symbol_oat_slash == std::string::npos) {
        Usage("--multi-image used with unusable symbol filename %s", base_symbol_oat.c_str());
      }
      base_symbol_oat = base_symbol_oat.substr(0, last_symbol_oat_slash + 1);
    }

    const size_t num_expanded_files = 2 + (base_symbol_oat.empty() ? 0 : 1);
    char_backing_storage_.reserve((dex_locations_.size() - 1) * num_expanded_files);

    // Now create the other names. Use a counted loop to skip the first one.
    for (size_t i = 1; i < dex_locations_.size(); ++i) {
      // TODO: Make everything properly std::string.
      std::string image_name = CreateMultiImageName(dex_locations_[i], prefix, infix, ".art");
      char_backing_storage_.push_back(base_img + image_name);
      image_filenames_.push_back((char_backing_storage_.end() - 1)->c_str());

      std::string oat_name = CreateMultiImageName(dex_locations_[i], prefix, infix, ".oat");
      char_backing_storage_.push_back(base_oat + oat_name);
      oat_filenames_.push_back((char_backing_storage_.end() - 1)->c_str());

      if (!base_symbol_oat.empty()) {
        char_backing_storage_.push_back(base_symbol_oat + oat_name);
        oat_unstripped_.push_back((char_backing_storage_.end() - 1)->c_str());
      }
    }
  }

  // Modify the input string in the following way:
  //   0) Assume input is /a/b/c.d
  //   1) Strip the path  -> c.d
  //   2) Inject prefix p -> pc.d
  //   3) Inject infix i  -> pci.d
  //   4) Replace suffix with s if it's "jar"  -> d == "jar" -> pci.s
  static std::string CreateMultiImageName(std::string in,
                                          const std::string& prefix,
                                          const std::string& infix,
                                          const char* replace_suffix) {
    size_t last_dex_slash = in.rfind('/');
    if (last_dex_slash != std::string::npos) {
      in = in.substr(last_dex_slash + 1);
    }
    if (!prefix.empty()) {
      in = prefix + in;
    }
    if (!infix.empty()) {
      // Inject infix.
      size_t last_dot = in.rfind('.');
      if (last_dot != std::string::npos) {
        in.insert(last_dot, infix);
      }
    }
    if (android::base::EndsWith(in, ".jar")) {
      in = in.substr(0, in.length() - strlen(".jar")) +
          (replace_suffix != nullptr ? replace_suffix : "");
    }
    return in;
  }

  void InsertCompileOptions(int argc, char** argv) {
    std::ostringstream oss;
    if (!avoid_storing_invocation_) {
      for (int i = 0; i < argc; ++i) {
        if (i > 0) {
          oss << ' ';
        }
        oss << argv[i];
      }
      key_value_store_->Put(OatHeader::kDex2OatCmdLineKey, oss.str());
      oss.str("");  // Reset.
    }
    oss << kRuntimeISA;
    key_value_store_->Put(OatHeader::kDex2OatHostKey, oss.str());
    key_value_store_->Put(
        OatHeader::kPicKey,
        compiler_options_->compile_pic_ ? OatHeader::kTrueValue : OatHeader::kFalseValue);
    key_value_store_->Put(
        OatHeader::kDebuggableKey,
        compiler_options_->debuggable_ ? OatHeader::kTrueValue : OatHeader::kFalseValue);
    key_value_store_->Put(
        OatHeader::kNativeDebuggableKey,
        compiler_options_->GetNativeDebuggable() ? OatHeader::kTrueValue : OatHeader::kFalseValue);
    key_value_store_->Put(OatHeader::kCompilerFilter,
        CompilerFilter::NameOfFilter(compiler_options_->GetCompilerFilter()));
    key_value_store_->Put(OatHeader::kConcurrentCopying,
                          kUseReadBarrier ? OatHeader::kTrueValue : OatHeader::kFalseValue);
  }

  // Parse the arguments from the command line. In case of an unrecognized option or impossible
  // values/combinations, a usage error will be displayed and exit() is called. Thus, if the method
  // returns, arguments have been successfully parsed.
  void ParseArgs(int argc, char** argv) {
    original_argc = argc;
    original_argv = argv;

    InitLogging(argv, Runtime::Abort);

    // Skip over argv[0].
    argv++;
    argc--;

    if (argc == 0) {
      Usage("No arguments specified");
    }

    std::unique_ptr<ParserOptions> parser_options(new ParserOptions());
    compiler_options_.reset(new CompilerOptions());

    for (int i = 0; i < argc; i++) {
      const StringPiece option(argv[i]);
      const bool log_options = false;
      if (log_options) {
        LOG(INFO) << "dex2oat: option[" << i << "]=" << argv[i];
      }
      if (option.starts_with("--dex-file=")) {
        dex_filenames_.push_back(option.substr(strlen("--dex-file=")).data());
      } else if (option.starts_with("--dex-location=")) {
        dex_locations_.push_back(option.substr(strlen("--dex-location=")).data());
      } else if (option.starts_with("--zip-fd=")) {
        ParseZipFd(option);
      } else if (option.starts_with("--zip-location=")) {
        zip_location_ = option.substr(strlen("--zip-location=")).data();
      } else if (option.starts_with("--input-vdex-fd=")) {
        ParseInputVdexFd(option);
      } else if (option.starts_with("--input-vdex=")) {
        input_vdex_ = option.substr(strlen("--input-vdex=")).data();
      } else if (option.starts_with("--output-vdex=")) {
        output_vdex_ = option.substr(strlen("--output-vdex=")).data();
      } else if (option.starts_with("--output-vdex-fd=")) {
        ParseOutputVdexFd(option);
      } else if (option.starts_with("--oat-file=")) {
        oat_filenames_.push_back(option.substr(strlen("--oat-file=")).data());
      } else if (option.starts_with("--oat-symbols=")) {
        parser_options->oat_symbols.push_back(option.substr(strlen("--oat-symbols=")).data());
      } else if (option.starts_with("--oat-fd=")) {
        ParseOatFd(option);
      } else if (option.starts_with("--oat-location=")) {
        oat_location_ = option.substr(strlen("--oat-location=")).data();
      } else if (option == "--watch-dog") {
        parser_options->watch_dog_enabled = true;
      } else if (option == "--no-watch-dog") {
        parser_options->watch_dog_enabled = false;
      } else if (option.starts_with("--watchdog-timeout=")) {
        ParseIntOption(option,
                       "--watchdog-timeout",
                       &parser_options->watch_dog_timeout_in_ms,
                       Usage);
      } else if (option.starts_with("-j")) {
        ParseJ(option);
      } else if (option.starts_with("--image=")) {
        image_filenames_.push_back(option.substr(strlen("--image=")).data());
      } else if (option.starts_with("--image-classes=")) {
        image_classes_filename_ = option.substr(strlen("--image-classes=")).data();
      } else if (option.starts_with("--image-classes-zip=")) {
        image_classes_zip_filename_ = option.substr(strlen("--image-classes-zip=")).data();
      } else if (option.starts_with("--image-format=")) {
        ParseImageFormat(option);
      } else if (option.starts_with("--compiled-classes=")) {
        compiled_classes_filename_ = option.substr(strlen("--compiled-classes=")).data();
      } else if (option.starts_with("--compiled-classes-zip=")) {
        compiled_classes_zip_filename_ = option.substr(strlen("--compiled-classes-zip=")).data();
      } else if (option.starts_with("--compiled-methods=")) {
        compiled_methods_filename_ = option.substr(strlen("--compiled-methods=")).data();
      } else if (option.starts_with("--compiled-methods-zip=")) {
        compiled_methods_zip_filename_ = option.substr(strlen("--compiled-methods-zip=")).data();
      } else if (option.starts_with("--run-passes=")) {
        passes_to_run_filename_ = option.substr(strlen("--run-passes=")).data();
      } else if (option.starts_with("--base=")) {
        ParseBase(option);
      } else if (option.starts_with("--boot-image=")) {
        parser_options->boot_image_filename = option.substr(strlen("--boot-image=")).data();
      } else if (option.starts_with("--android-root=")) {
        android_root_ = option.substr(strlen("--android-root=")).data();
      } else if (option.starts_with("--instruction-set=")) {
        ParseInstructionSet(option);
      } else if (option.starts_with("--instruction-set-variant=")) {
        ParseInstructionSetVariant(option, parser_options.get());
      } else if (option.starts_with("--instruction-set-features=")) {
        ParseInstructionSetFeatures(option, parser_options.get());
      } else if (option.starts_with("--compiler-backend=")) {
        ParseCompilerBackend(option, parser_options.get());
      } else if (option.starts_with("--profile-file=")) {
        profile_file_ = option.substr(strlen("--profile-file=")).ToString();
      } else if (option.starts_with("--profile-file-fd=")) {
        ParseUintOption(option, "--profile-file-fd", &profile_file_fd_, Usage);
      } else if (option == "--host") {
        is_host_ = true;
      } else if (option == "--runtime-arg") {
        if (++i >= argc) {
          Usage("Missing required argument for --runtime-arg");
        }
        if (log_options) {
          LOG(INFO) << "dex2oat: option[" << i << "]=" << argv[i];
        }
        runtime_args_.push_back(argv[i]);
      } else if (option == "--dump-timing") {
        dump_timing_ = true;
      } else if (option == "--dump-passes") {
        dump_passes_ = true;
      } else if (option == "--dump-stats") {
        dump_stats_ = true;
      } else if (option == "--avoid-storing-invocation") {
        avoid_storing_invocation_ = true;
      } else if (option.starts_with("--swap-file=")) {
        swap_file_name_ = option.substr(strlen("--swap-file=")).data();
      } else if (option.starts_with("--swap-fd=")) {
        ParseUintOption(option, "--swap-fd", &swap_fd_, Usage);
      } else if (option.starts_with("--swap-dex-size-threshold=")) {
        ParseUintOption(option,
                        "--swap-dex-size-threshold",
                        &min_dex_file_cumulative_size_for_swap_,
                        Usage);
      } else if (option.starts_with("--swap-dex-count-threshold=")) {
        ParseUintOption(option,
                        "--swap-dex-count-threshold",
                        &min_dex_files_for_swap_,
                        Usage);
      } else if (option.starts_with("--very-large-app-threshold=")) {
        ParseUintOption(option,
                        "--very-large-app-threshold",
                        &very_large_threshold_,
                        Usage);
      } else if (option.starts_with("--app-image-file=")) {
        app_image_file_name_ = option.substr(strlen("--app-image-file=")).data();
      } else if (option.starts_with("--app-image-fd=")) {
        ParseUintOption(option, "--app-image-fd", &app_image_fd_, Usage);
      } else if (option == "--multi-image") {
        multi_image_ = true;
      } else if (option.starts_with("--no-inline-from=")) {
        no_inline_from_string_ = option.substr(strlen("--no-inline-from=")).data();
      } else if (option == "--force-determinism") {
        if (!SupportsDeterministicCompilation()) {
          Usage("Option --force-determinism requires read barriers or a CMS/MS garbage collector");
        }
        force_determinism_ = true;
      } else if (option.starts_with("--classpath-dir=")) {
        classpath_dir_ = option.substr(strlen("--classpath-dir=")).data();
      } else if (option.starts_with("--class-loader-context=")) {
        class_loader_context_ = ClassLoaderContext::Create(
            option.substr(strlen("--class-loader-context=")).data());
        if (class_loader_context_ == nullptr) {
          Usage("Option --class-loader-context has an incorrect format: %s", option.data());
        }
      } else if (option.starts_with("--dirty-image-objects=")) {
        dirty_image_objects_filename_ = option.substr(strlen("--dirty-image-objects=")).data();
      } else if (option == "--checksum-rewriting") {
        rewrite_dex_location_checksums_ = true;
      } else if (!compiler_options_->ParseCompilerOption(option, Usage)) {
        Usage("Unknown argument %s", option.data());
      }
    }

    ProcessOptions(parser_options.get());

    // Insert some compiler things.
    InsertCompileOptions(argc, argv);
  }

  // Check whether the oat output files are writable, and open them for later. Also open a swap
  // file, if a name is given.
  bool OpenFile() {
    // Prune non-existent dex files now so that we don't create empty oat files for multi-image.
    PruneNonExistentDexFiles();

    // Expand oat and image filenames for multi image.
    if (IsBootImage() && multi_image_) {
      ExpandOatAndImageFilenames();
    }

    // OAT and VDEX file handling
    if (oat_fd_ == -1) {
      DCHECK(!oat_filenames_.empty());
      for (const char* oat_filename : oat_filenames_) {
        std::unique_ptr<File> oat_file(OS::CreateEmptyFile(oat_filename));
        if (oat_file == nullptr) {
          PLOG(ERROR) << "Failed to create oat file: " << oat_filename;
          return false;
        }
        if (fchmod(oat_file->Fd(), 0644) != 0) {
          PLOG(ERROR) << "Failed to make oat file world readable: " << oat_filename;
          oat_file->Erase();
          return false;
        }
        oat_files_.push_back(std::move(oat_file));
        DCHECK_EQ(input_vdex_fd_, -1);
        if (!input_vdex_.empty()) {
          std::string error_msg;
          input_vdex_file_ = VdexFile::Open(input_vdex_,
                                            /* writable */ false,
                                            /* low_4gb */ false,
                                            DoEagerUnquickeningOfVdex(),
                                            &error_msg);
        }

        DCHECK_EQ(output_vdex_fd_, -1);
        std::string vdex_filename = output_vdex_.empty()
            ? ReplaceFileExtension(oat_filename, "vdex")
            : output_vdex_;
        if (vdex_filename == input_vdex_ && output_vdex_.empty()) {
          update_input_vdex_ = true;
          std::unique_ptr<File> vdex_file(OS::OpenFileReadWrite(vdex_filename.c_str()));
          vdex_files_.push_back(std::move(vdex_file));
        } else {
          std::unique_ptr<File> vdex_file(OS::CreateEmptyFile(vdex_filename.c_str()));
          if (vdex_file == nullptr) {
            PLOG(ERROR) << "Failed to open vdex file: " << vdex_filename;
            return false;
          }
          if (fchmod(vdex_file->Fd(), 0644) != 0) {
            PLOG(ERROR) << "Failed to make vdex file world readable: " << vdex_filename;
            vdex_file->Erase();
            return false;
          }
          vdex_files_.push_back(std::move(vdex_file));
        }
      }
    } else {
      std::unique_ptr<File> oat_file(new File(oat_fd_, oat_location_, /* check_usage */ true));
      if (oat_file == nullptr) {
        PLOG(ERROR) << "Failed to create oat file: " << oat_location_;
        return false;
      }
      oat_file->DisableAutoClose();
      if (oat_file->SetLength(0) != 0) {
        PLOG(WARNING) << "Truncating oat file " << oat_location_ << " failed.";
        oat_file->Erase();
        return false;
      }
      oat_files_.push_back(std::move(oat_file));

      if (input_vdex_fd_ != -1) {
        struct stat s;
        int rc = TEMP_FAILURE_RETRY(fstat(input_vdex_fd_, &s));
        if (rc == -1) {
          PLOG(WARNING) << "Failed getting length of vdex file";
        } else {
          std::string error_msg;
          input_vdex_file_ = VdexFile::Open(input_vdex_fd_,
                                            s.st_size,
                                            "vdex",
                                            /* writable */ false,
                                            /* low_4gb */ false,
                                            DoEagerUnquickeningOfVdex(),
                                            &error_msg);
          // If there's any problem with the passed vdex, just warn and proceed
          // without it.
          if (input_vdex_file_ == nullptr) {
            PLOG(WARNING) << "Failed opening vdex file: " << error_msg;
          }
        }
      }

      DCHECK_NE(output_vdex_fd_, -1);
      std::string vdex_location = ReplaceFileExtension(oat_location_, "vdex");
      std::unique_ptr<File> vdex_file(new File(output_vdex_fd_, vdex_location, /* check_usage */ true));
      if (vdex_file == nullptr) {
        PLOG(ERROR) << "Failed to create vdex file: " << vdex_location;
        return false;
      }
      vdex_file->DisableAutoClose();
      if (input_vdex_file_ != nullptr && output_vdex_fd_ == input_vdex_fd_) {
        update_input_vdex_ = true;
      } else {
        if (vdex_file->SetLength(0) != 0) {
          PLOG(ERROR) << "Truncating vdex file " << vdex_location << " failed.";
          vdex_file->Erase();
          return false;
        }
      }
      vdex_files_.push_back(std::move(vdex_file));

      oat_filenames_.push_back(oat_location_.c_str());
    }

    // If we're updating in place a vdex file, be defensive and put an invalid vdex magic in case
    // dex2oat gets killed.
    // Note: we're only invalidating the magic data in the file, as dex2oat needs the rest of
    // the information to remain valid.
    if (update_input_vdex_) {
      std::unique_ptr<BufferedOutputStream> vdex_out = std::make_unique<BufferedOutputStream>(
          std::make_unique<FileOutputStream>(vdex_files_.back().get()));
      if (!vdex_out->WriteFully(&VdexFile::Header::kVdexInvalidMagic,
                                arraysize(VdexFile::Header::kVdexInvalidMagic))) {
        PLOG(ERROR) << "Failed to invalidate vdex header. File: " << vdex_out->GetLocation();
        return false;
      }

      if (!vdex_out->Flush()) {
        PLOG(ERROR) << "Failed to flush stream after invalidating header of vdex file."
                    << " File: " << vdex_out->GetLocation();
        return false;
      }
    }

    // Swap file handling
    //
    // If the swap fd is not -1, we assume this is the file descriptor of an open but unlinked file
    // that we can use for swap.
    //
    // If the swap fd is -1 and we have a swap-file string, open the given file as a swap file. We
    // will immediately unlink to satisfy the swap fd assumption.
    if (swap_fd_ == -1 && !swap_file_name_.empty()) {
      std::unique_ptr<File> swap_file(OS::CreateEmptyFile(swap_file_name_.c_str()));
      if (swap_file.get() == nullptr) {
        PLOG(ERROR) << "Failed to create swap file: " << swap_file_name_;
        return false;
      }
      swap_fd_ = swap_file->Fd();
      swap_file->MarkUnchecked();     // We don't we to track this, it will be unlinked immediately.
      swap_file->DisableAutoClose();  // We'll handle it ourselves, the File object will be
                                      // released immediately.
      unlink(swap_file_name_.c_str());
    }

    return true;
  }

  void EraseOutputFiles() {
    for (auto& files : { &vdex_files_, &oat_files_ }) {
      for (size_t i = 0; i < files->size(); ++i) {
        if ((*files)[i].get() != nullptr) {
          (*files)[i]->Erase();
          (*files)[i].reset();
        }
      }
    }
  }

  void LoadClassProfileDescriptors() {
    if (profile_compilation_info_ != nullptr && IsImage()) {
      Runtime* runtime = Runtime::Current();
      CHECK(runtime != nullptr);
      // Filter out class path classes since we don't want to include these in the image.
      image_classes_.reset(
          new std::unordered_set<std::string>(
              profile_compilation_info_->GetClassDescriptors(dex_files_)));
      VLOG(compiler) << "Loaded " << image_classes_->size()
                     << " image class descriptors from profile";
      if (VLOG_IS_ON(compiler)) {
        for (const std::string& s : *image_classes_) {
          LOG(INFO) << "Image class " << s;
        }
      }
    }
  }

  // Set up the environment for compilation. Includes starting the runtime and loading/opening the
  // boot class path.
  dex2oat::ReturnCode Setup() {
    TimingLogger::ScopedTiming t("dex2oat Setup", timings_);

    if (!PrepareImageClasses() || !PrepareCompiledClasses() || !PrepareCompiledMethods() ||
        !PrepareDirtyObjects()) {
      return dex2oat::ReturnCode::kOther;
    }

    // Verification results are null since we don't know if we will need them yet as the compler
    // filter may change.
    // This needs to be done before PrepareRuntimeOptions since the callbacks are passed to the
    // runtime.
    callbacks_.reset(new QuickCompilerCallbacks(
        IsBootImage() ?
            CompilerCallbacks::CallbackMode::kCompileBootImage :
            CompilerCallbacks::CallbackMode::kCompileApp));

    RuntimeArgumentMap runtime_options;
    if (!PrepareRuntimeOptions(&runtime_options)) {
      return dex2oat::ReturnCode::kOther;
    }

    if (dex_locations_.size() == dex_filenames_.size()) {
      for (uint32_t i = 0; i < dex_locations_.size(); i++) {
        VLOG(artist) << "DexLocation: " << dex_locations_.at(i) << " <> " << dex_filenames_.at(i);
        if (rewrite_dex_location_checksums_) {
          if (strcmp(dex_locations_.at(i), dex_filenames_.at(i)) != 0) {
            VLOG(artist) << "> Rewriting Dex LocationChecksum";
            rewrite_dex_location_checksums_ = true;
          } else {
            VLOG(artist) << "> NOT Rewriting Dex LocationChecksum";
          }
          break;
        }
      }
    }

    CreateOatWriters();
    if (!AddDexFileSources()) {
      return dex2oat::ReturnCode::kOther;
    }

    if (IsBootImage() && image_filenames_.size() > 1) {
      // If we're compiling the boot image, store the boot classpath into the Key-Value store.
      // We need this for the multi-image case.
      key_value_store_->Put(OatHeader::kBootClassPathKey,
                            gc::space::ImageSpace::GetMultiImageBootClassPath(dex_locations_,
                                                                              oat_filenames_,
                                                                              image_filenames_));
    }

    if (!IsBootImage()) {
      // When compiling an app, create the runtime early to retrieve
      // the image location key needed for the oat header.
      if (!CreateRuntime(std::move(runtime_options))) {
        return dex2oat::ReturnCode::kCreateRuntime;
      }

      if (CompilerFilter::DependsOnImageChecksum(compiler_options_->GetCompilerFilter())) {
        TimingLogger::ScopedTiming t3("Loading image checksum", timings_);
        std::vector<gc::space::ImageSpace*> image_spaces =
            Runtime::Current()->GetHeap()->GetBootImageSpaces();
        image_file_location_oat_checksum_ = image_spaces[0]->GetImageHeader().GetOatChecksum();
        image_file_location_oat_data_begin_ =
            reinterpret_cast<uintptr_t>(image_spaces[0]->GetImageHeader().GetOatDataBegin());
        image_patch_delta_ = image_spaces[0]->GetImageHeader().GetPatchDelta();
        // Store the boot image filename(s).
        std::vector<std::string> image_filenames;
        for (const gc::space::ImageSpace* image_space : image_spaces) {
          image_filenames.push_back(image_space->GetImageFilename());
        }
        std::string image_file_location = android::base::Join(image_filenames, ':');
        if (!image_file_location.empty()) {
          key_value_store_->Put(OatHeader::kImageLocationKey, image_file_location);
        }
      } else {
        image_file_location_oat_checksum_ = 0u;
        image_file_location_oat_data_begin_ = 0u;
        image_patch_delta_ = 0;
      }

      // Open dex files for class path.

      if (class_loader_context_ == nullptr) {
        // If no context was specified use the default one (which is an empty PathClassLoader).
        class_loader_context_ = std::unique_ptr<ClassLoaderContext>(ClassLoaderContext::Default());
      }

      DCHECK_EQ(oat_writers_.size(), 1u);

      // Note: Ideally we would reject context where the source dex files are also
      // specified in the classpath (as it doesn't make sense). However this is currently
      // needed for non-prebuild tests and benchmarks which expects on the fly compilation.
      // Also, for secondary dex files we do not have control on the actual classpath.
      // Instead of aborting, remove all the source location from the context classpaths.
      if (class_loader_context_->RemoveLocationsFromClassPaths(
            oat_writers_[0]->GetSourceLocations())) {
        LOG(WARNING) << "The source files to be compiled are also in the classpath.";
      }

      // We need to open the dex files before encoding the context in the oat file.
      // (because the encoding adds the dex checksum...)
      // TODO(calin): consider redesigning this so we don't have to open the dex files before
      // creating the actual class loader.
      if (!class_loader_context_->OpenDexFiles(runtime_->GetInstructionSet(), classpath_dir_)) {
        // Do not abort if we couldn't open files from the classpath. They might be
        // apks without dex files and right now are opening flow will fail them.
        LOG(WARNING) << "Failed to open classpath dex files";
      }

      // Store the class loader context in the oat header.
      key_value_store_->Put(OatHeader::kClassPathKey,
                            class_loader_context_->EncodeContextForOatFile(classpath_dir_));
    }

    // Now that we have finalized key_value_store_, start writing the oat file.
    {
      TimingLogger::ScopedTiming t_dex("Writing and opening dex files", timings_);
      rodata_.reserve(oat_writers_.size());
      for (size_t i = 0, size = oat_writers_.size(); i != size; ++i) {
        rodata_.push_back(elf_writers_[i]->StartRoData());
        // Unzip or copy dex files straight to the oat file.
        std::unique_ptr<MemMap> opened_dex_files_map;
        std::vector<std::unique_ptr<const DexFile>> opened_dex_files;
        // No need to verify the dex file for:
        // 1) Dexlayout since it does the verification. It also may not pass the verification since
        // we don't update the dex checksum.
        // 2) when we have a vdex file, which means it was already verified.
        const bool verify = !DoDexLayoutOptimizations() && (input_vdex_file_ == nullptr);
        if (!oat_writers_[i]->WriteAndOpenDexFiles(
            kIsVdexEnabled ? vdex_files_[i].get() : oat_files_[i].get(),
            rodata_.back(),
            instruction_set_,
            instruction_set_features_.get(),
            key_value_store_.get(),
            verify,
            update_input_vdex_,
            &opened_dex_files_map,
            &opened_dex_files)) {
          return dex2oat::ReturnCode::kOther;
        }
        dex_files_per_oat_file_.push_back(MakeNonOwningPointerVector(opened_dex_files));
        if (opened_dex_files_map != nullptr) {
          opened_dex_files_maps_.push_back(std::move(opened_dex_files_map));
          for (std::unique_ptr<const DexFile>& dex_file : opened_dex_files) {
            dex_file_oat_index_map_.emplace(dex_file.get(), i);
            opened_dex_files_.push_back(std::move(dex_file));
          }
        } else {
          DCHECK(opened_dex_files.empty());
        }
      }
    }

    dex_files_ = MakeNonOwningPointerVector(opened_dex_files_);

    // If we need to downgrade the compiler-filter for size reasons.
    if (!IsBootImage() && IsVeryLarge(dex_files_)) {
      // Disable app image to make sure dex2oat unloading is enabled.
      compiler_options_->DisableAppImage();

      // If we need to downgrade the compiler-filter for size reasons, do that early before we read
      // it below for creating verification callbacks.
      if (!CompilerFilter::IsAsGoodAs(kLargeAppFilter, compiler_options_->GetCompilerFilter())) {
        LOG(INFO) << "Very large app, downgrading to verify.";
        // Note: this change won't be reflected in the key-value store, as that had to be
        //       finalized before loading the dex files. This setup is currently required
        //       to get the size from the DexFile objects.
        // TODO: refactor. b/29790079
        compiler_options_->SetCompilerFilter(kLargeAppFilter);
      }
    }

    if (CompilerFilter::IsAnyCompilationEnabled(compiler_options_->GetCompilerFilter())) {
      // Only modes with compilation require verification results, do this here instead of when we
      // create the compilation callbacks since the compilation mode may have been changed by the
      // very large app logic.
      // Avoiding setting the verification results saves RAM by not adding the dex files later in
      // the function.
      verification_results_.reset(new VerificationResults(compiler_options_.get()));
      callbacks_->SetVerificationResults(verification_results_.get());
    }

    // We had to postpone the swap decision till now, as this is the point when we actually
    // know about the dex files we're going to use.

    // Make sure that we didn't create the driver, yet.
    CHECK(driver_ == nullptr);
    // If we use a swap file, ensure we are above the threshold to make it necessary.
    if (swap_fd_ != -1) {
      if (!UseSwap(IsBootImage(), dex_files_)) {
        close(swap_fd_);
        swap_fd_ = -1;
        VLOG(compiler) << "Decided to run without swap.";
      } else {
        LOG(INFO) << "Large app, accepted running with swap.";
      }
    }
    // Note that dex2oat won't close the swap_fd_. The compiler driver's swap space will do that.
    if (IsBootImage()) {
      // For boot image, pass opened dex files to the Runtime::Create().
      // Note: Runtime acquires ownership of these dex files.
      runtime_options.Set(RuntimeArgumentMap::BootClassPathDexList, &opened_dex_files_);
      if (!CreateRuntime(std::move(runtime_options))) {
        return dex2oat::ReturnCode::kOther;
      }
    }

    // If we're doing the image, override the compiler filter to force full compilation. Must be
    // done ahead of WellKnownClasses::Init that causes verification.  Note: doesn't force
    // compilation of class initializers.
    // Whilst we're in native take the opportunity to initialize well known classes.
    Thread* self = Thread::Current();
    WellKnownClasses::Init(self->GetJniEnv());

    if (!IsBootImage()) {
      constexpr bool kSaveDexInput = false;
      if (kSaveDexInput) {
        SaveDexInput();
      }
    }

    // Ensure opened dex files are writable for dex-to-dex transformations.
    for (const std::unique_ptr<MemMap>& map : opened_dex_files_maps_) {
      if (!map->Protect(PROT_READ | PROT_WRITE)) {
        PLOG(ERROR) << "Failed to make .dex files writeable.";
        return dex2oat::ReturnCode::kOther;
      }
    }

    // Verification results are only required for modes that have any compilation. Avoid
    // adding the dex files if possible to prevent allocating large arrays.
    if (verification_results_ != nullptr) {
      for (const auto& dex_file : dex_files_) {
        // Pre-register dex files so that we can access verification results without locks during
        // compilation and verification.
        verification_results_->AddDexFile(dex_file);
      }
    }

    return dex2oat::ReturnCode::kNoFailure;
  }

  // If we need to keep the oat file open for the image writer.
  bool ShouldKeepOatFileOpen() const {
    return IsImage() && oat_fd_ != kInvalidFd;
  }

  // Doesn't return the class loader since it's not meant to be used for image compilation.
  void CompileDexFilesIndividually() {
    CHECK(!IsImage()) << "Not supported with image";
    for (const DexFile* dex_file : dex_files_) {
      std::vector<const DexFile*> dex_files(1u, dex_file);
      VLOG(compiler) << "Compiling " << dex_file->GetLocation();
      jobject class_loader = CompileDexFiles(dex_files);
      CHECK(class_loader != nullptr);
      ScopedObjectAccess soa(Thread::Current());
      // Unload class loader to free RAM.
      jweak weak_class_loader = soa.Env()->vm->AddWeakGlobalRef(
          soa.Self(),
          soa.Decode<mirror::ClassLoader>(class_loader));
      soa.Env()->vm->DeleteGlobalRef(soa.Self(), class_loader);
      runtime_->GetHeap()->CollectGarbage(/*clear_soft_references*/ true);
      ObjPtr<mirror::ClassLoader> decoded_weak = soa.Decode<mirror::ClassLoader>(weak_class_loader);
      if (decoded_weak != nullptr) {
        LOG(FATAL) << "Failed to unload class loader, path from root set: "
                   << runtime_->GetHeap()->GetVerification()->FirstPathFromRootSet(decoded_weak);
      }
      VLOG(compiler) << "Unloaded classloader";
    }
  }

  bool ShouldCompileDexFilesIndividually() const {
    // Compile individually if we are:
    // 1. not building an image,
    // 2. not verifying a vdex file,
    // 3. using multidex,
    // 4. not doing any AOT compilation.
    // This means extract, no-vdex verify, and quicken, will use the individual compilation
    // mode (to reduce RAM used by the compiler).
    return !IsImage() &&
        !update_input_vdex_ &&
        dex_files_.size() > 1 &&
        !CompilerFilter::IsAotCompilationEnabled(compiler_options_->GetCompilerFilter());
  }

  // Set up and create the compiler driver and then invoke it to compile all the dex files.
  jobject Compile() {
    ClassLinker* const class_linker = Runtime::Current()->GetClassLinker();

    TimingLogger::ScopedTiming t("dex2oat Compile", timings_);
    compiler_phases_timings_.reset(new CumulativeLogger("compilation times"));

    // Find the dex files we should not inline from.
    std::vector<std::string> no_inline_filters;
    Split(no_inline_from_string_, ',', &no_inline_filters);

    // For now, on the host always have core-oj removed.
    const std::string core_oj = "core-oj";
    if (!kIsTargetBuild && !ContainsElement(no_inline_filters, core_oj)) {
      no_inline_filters.push_back(core_oj);
    }

    if (!no_inline_filters.empty()) {
      std::vector<const DexFile*> class_path_files;
      if (!IsBootImage()) {
        // The class loader context is used only for apps.
        class_path_files = class_loader_context_->FlattenOpenedDexFiles();
      }

      std::vector<const std::vector<const DexFile*>*> dex_file_vectors = {
          &class_linker->GetBootClassPath(),
          &class_path_files,
          &dex_files_
      };
      for (const std::vector<const DexFile*>* dex_file_vector : dex_file_vectors) {
        for (const DexFile* dex_file : *dex_file_vector) {
          for (const std::string& filter : no_inline_filters) {
            // Use dex_file->GetLocation() rather than dex_file->GetBaseLocation(). This
            // allows tests to specify <test-dexfile>!classes2.dex if needed but if the
            // base location passes the StartsWith() test, so do all extra locations.
            std::string dex_location = dex_file->GetLocation();
            if (filter.find('/') == std::string::npos) {
              // The filter does not contain the path. Remove the path from dex_location as well.
              size_t last_slash = dex_file->GetLocation().rfind('/');
              if (last_slash != std::string::npos) {
                dex_location = dex_location.substr(last_slash + 1);
              }
            }

            if (android::base::StartsWith(dex_location, filter.c_str())) {
              VLOG(compiler) << "Disabling inlining from " << dex_file->GetLocation();
              no_inline_from_dex_files_.push_back(dex_file);
              break;
            }
          }
        }
      }
      if (!no_inline_from_dex_files_.empty()) {
        compiler_options_->no_inline_from_ = &no_inline_from_dex_files_;
      }
    }

    driver_.reset(new CompilerDriver(compiler_options_.get(),
                                     verification_results_.get(),
                                     compiler_kind_,
                                     instruction_set_,
                                     instruction_set_features_.get(),
                                     image_classes_.release(),
                                     compiled_classes_.release(),
                                     compiled_methods_.release(),
                                     thread_count_,
                                     dump_stats_,
                                     dump_passes_,
                                     compiler_phases_timings_.get(),
                                     swap_fd_,
                                     profile_compilation_info_.get()));
    driver_->SetDexFilesForOatFile(dex_files_);

    const bool compile_individually = ShouldCompileDexFilesIndividually();
    if (compile_individually) {
      // Set the compiler driver in the callbacks so that we can avoid re-verification. This not
      // only helps performance but also prevents reverifying quickened bytecodes. Attempting
      // verify quickened bytecode causes verification failures.
      // Only set the compiler filter if we are doing separate compilation since there is a bit
      // of overhead when checking if a class was previously verified.
      callbacks_->SetDoesClassUnloading(true, driver_.get());
    }

    // Setup vdex for compilation.
    if (!DoEagerUnquickeningOfVdex() && input_vdex_file_ != nullptr) {
      callbacks_->SetVerifierDeps(
          new verifier::VerifierDeps(dex_files_, input_vdex_file_->GetVerifierDepsData()));

      // TODO: we unquicken unconditionally, as we don't know
      // if the boot image has changed. How exactly we'll know is under
      // experimentation.
      TimingLogger::ScopedTiming time_unquicken("Unquicken", timings_);
      VdexFile::Unquicken(dex_files_, input_vdex_file_->GetQuickeningInfo());
    } else {
      // Create the main VerifierDeps, here instead of in the compiler since we want to aggregate
      // the results for all the dex files, not just the results for the current dex file.
      callbacks_->SetVerifierDeps(new verifier::VerifierDeps(dex_files_));
    }
    // Invoke the compilation.
    if (compile_individually) {
      CompileDexFilesIndividually();
      // Return a null classloader since we already freed released it.
      return nullptr;
    }
    return CompileDexFiles(dex_files_);
  }

  // Create the class loader, use it to compile, and return.
  jobject CompileDexFiles(const std::vector<const DexFile*>& dex_files) {
    ClassLinker* const class_linker = Runtime::Current()->GetClassLinker();

    jobject class_loader = nullptr;
    if (!IsBootImage()) {
      class_loader = class_loader_context_->CreateClassLoader(dex_files_);
    }

    // Register dex caches and key them to the class loader so that they only unload when the
    // class loader unloads.
    for (const auto& dex_file : dex_files) {
      ScopedObjectAccess soa(Thread::Current());
      // Registering the dex cache adds a strong root in the class loader that prevents the dex
      // cache from being unloaded early.
      ObjPtr<mirror::DexCache> dex_cache = class_linker->RegisterDexFile(
          *dex_file,
          soa.Decode<mirror::ClassLoader>(class_loader));
      if (dex_cache == nullptr) {
        soa.Self()->AssertPendingException();
        LOG(FATAL) << "Failed to register dex file " << dex_file->GetLocation() << " "
                   << soa.Self()->GetException()->Dump();
      }
    }
    driver_->CompileAll(class_loader, dex_files, timings_);
    return class_loader;
  }

  // Notes on the interleaving of creating the images and oat files to
  // ensure the references between the two are correct.
  //
  // Currently we have a memory layout that looks something like this:
  //
  // +--------------+
  // | images       |
  // +--------------+
  // | oat files    |
  // +--------------+
  // | alloc spaces |
  // +--------------+
  //
  // There are several constraints on the loading of the images and oat files.
  //
  // 1. The images are expected to be loaded at an absolute address and
  // contain Objects with absolute pointers within the images.
  //
  // 2. There are absolute pointers from Methods in the images to their
  // code in the oat files.
  //
  // 3. There are absolute pointers from the code in the oat files to Methods
  // in the images.
  //
  // 4. There are absolute pointers from code in the oat files to other code
  // in the oat files.
  //
  // To get this all correct, we go through several steps.
  //
  // 1. We prepare offsets for all data in the oat files and calculate
  // the oat data size and code size. During this stage, we also set
  // oat code offsets in methods for use by the image writer.
  //
  // 2. We prepare offsets for the objects in the images and calculate
  // the image sizes.
  //
  // 3. We create the oat files. Originally this was just our own proprietary
  // file but now it is contained within an ELF dynamic object (aka an .so
  // file). Since we know the image sizes and oat data sizes and code sizes we
  // can prepare the ELF headers and we then know the ELF memory segment
  // layout and we can now resolve all references. The compiler provides
  // LinkerPatch information in each CompiledMethod and we resolve these,
  // using the layout information and image object locations provided by
  // image writer, as we're writing the method code.
  //
  // 4. We create the image files. They need to know where the oat files
  // will be loaded after itself. Originally oat files were simply
  // memory mapped so we could predict where their contents were based
  // on the file size. Now that they are ELF files, we need to inspect
  // the ELF files to understand the in memory segment layout including
  // where the oat header is located within.
  // TODO: We could just remember this information from step 3.
  //
  // 5. We fixup the ELF program headers so that dlopen will try to
  // load the .so at the desired location at runtime by offsetting the
  // Elf32_Phdr.p_vaddr values by the desired base address.
  // TODO: Do this in step 3. We already know the layout there.
  //
  // Steps 1.-3. are done by the CreateOatFile() above, steps 4.-5.
  // are done by the CreateImageFile() below.

  // Write out the generated code part. Calls the OatWriter and ElfBuilder. Also prepares the
  // ImageWriter, if necessary.
  // Note: Flushing (and closing) the file is the caller's responsibility, except for the failure
  //       case (when the file will be explicitly erased).
  bool WriteOutputFiles() {
    TimingLogger::ScopedTiming t("dex2oat Oat", timings_);

    // Sync the data to the file, in case we did dex2dex transformations.
    for (const std::unique_ptr<MemMap>& map : opened_dex_files_maps_) {
      if (!map->Sync()) {
        PLOG(ERROR) << "Failed to Sync() dex2dex output. Map: " << map->GetName();
        return false;
      }
    }

    if (IsImage()) {
      if (IsAppImage() && image_base_ == 0) {
        gc::Heap* const heap = Runtime::Current()->GetHeap();
        for (gc::space::ImageSpace* image_space : heap->GetBootImageSpaces()) {
          image_base_ = std::max(image_base_, RoundUp(
              reinterpret_cast<uintptr_t>(image_space->GetImageHeader().GetOatFileEnd()),
              kPageSize));
        }
        // The non moving space is right after the oat file. Put the preferred app image location
        // right after the non moving space so that we ideally get a continuous immune region for
        // the GC.
        // Use the default non moving space capacity since dex2oat does not have a separate non-
        // moving space. This means the runtime's non moving space space size will be as large
        // as the growth limit for dex2oat, but smaller in the zygote.
        const size_t non_moving_space_capacity = gc::Heap::kDefaultNonMovingSpaceCapacity;
        image_base_ += non_moving_space_capacity;
        VLOG(compiler) << "App image base=" << reinterpret_cast<void*>(image_base_);
      }

      image_writer_.reset(new ImageWriter(*driver_,
                                          image_base_,
                                          compiler_options_->GetCompilePic(),
                                          IsAppImage(),
                                          image_storage_mode_,
                                          oat_filenames_,
                                          dex_file_oat_index_map_,
                                          dirty_image_objects_.get()));

      // We need to prepare method offsets in the image address space for direct method patching.
      TimingLogger::ScopedTiming t2("dex2oat Prepare image address space", timings_);
      if (!image_writer_->PrepareImageAddressSpace()) {
        LOG(ERROR) << "Failed to prepare image address space.";
        return false;
      }
    }

    // Initialize the writers with the compiler driver, image writer, and their
    // dex files. The writers were created without those being there yet.
    for (size_t i = 0, size = oat_files_.size(); i != size; ++i) {
      std::unique_ptr<OatWriter>& oat_writer = oat_writers_[i];
      std::vector<const DexFile*>& dex_files = dex_files_per_oat_file_[i];
      oat_writer->Initialize(driver_.get(), image_writer_.get(), dex_files);
    }

    {
      TimingLogger::ScopedTiming t2("dex2oat Write VDEX", timings_);
      DCHECK(IsBootImage() || oat_files_.size() == 1u);
      verifier::VerifierDeps* verifier_deps = callbacks_->GetVerifierDeps();
      for (size_t i = 0, size = oat_files_.size(); i != size; ++i) {
        File* vdex_file = vdex_files_[i].get();
        std::unique_ptr<BufferedOutputStream> vdex_out =
            std::make_unique<BufferedOutputStream>(std::make_unique<FileOutputStream>(vdex_file));

        if (!oat_writers_[i]->WriteVerifierDeps(vdex_out.get(), verifier_deps)) {
          LOG(ERROR) << "Failed to write verifier dependencies into VDEX " << vdex_file->GetPath();
          return false;
        }

        if (!oat_writers_[i]->WriteQuickeningInfo(vdex_out.get())) {
          LOG(ERROR) << "Failed to write quickening info into VDEX " << vdex_file->GetPath();
          return false;
        }

        // VDEX finalized, seek back to the beginning and write checksums and the header.
        if (!oat_writers_[i]->WriteChecksumsAndVdexHeader(vdex_out.get())) {
          LOG(ERROR) << "Failed to write vdex header into VDEX " << vdex_file->GetPath();
          return false;
        }
      }
    }

    {
      TimingLogger::ScopedTiming t2("dex2oat Write ELF", timings_);
      linker::MultiOatRelativePatcher patcher(instruction_set_, instruction_set_features_.get());
      for (size_t i = 0, size = oat_files_.size(); i != size; ++i) {
        std::unique_ptr<ElfWriter>& elf_writer = elf_writers_[i];
        std::unique_ptr<OatWriter>& oat_writer = oat_writers_[i];

        oat_writer->PrepareLayout(&patcher);

        size_t rodata_size = oat_writer->GetOatHeader().GetExecutableOffset();
        size_t text_size = oat_writer->GetOatSize() - rodata_size;
        elf_writer->PrepareDynamicSection(rodata_size,
                                          text_size,
                                          oat_writer->GetBssSize(),
                                          oat_writer->GetBssMethodsOffset(),
                                          oat_writer->GetBssRootsOffset());

        if (IsImage()) {
          // Update oat layout.
          DCHECK(image_writer_ != nullptr);
          DCHECK_LT(i, oat_filenames_.size());
          image_writer_->UpdateOatFileLayout(i,
                                             elf_writer->GetLoadedSize(),
                                             oat_writer->GetOatDataOffset(),
                                             oat_writer->GetOatSize());
        }

        if (IsBootImage()) {
          // Have the image_file_location_oat_checksum_ for boot oat files
          // depend on the contents of all the boot oat files. This way only
          // the primary image checksum needs to be checked to determine
          // whether any of the images are out of date.
          image_file_location_oat_checksum_ ^= oat_writer->GetOatHeader().GetChecksum();
        }
      }

      for (size_t i = 0, size = oat_files_.size(); i != size; ++i) {
        std::unique_ptr<File>& oat_file = oat_files_[i];
        std::unique_ptr<ElfWriter>& elf_writer = elf_writers_[i];
        std::unique_ptr<OatWriter>& oat_writer = oat_writers_[i];

        oat_writer->AddMethodDebugInfos(debug::MakeTrampolineInfos(oat_writer->GetOatHeader()));

        // We need to mirror the layout of the ELF file in the compressed debug-info.
        // Therefore PrepareDebugInfo() relies on the SetLoadedSectionSizes() call further above.
        elf_writer->PrepareDebugInfo(oat_writer->GetMethodDebugInfo());

        OutputStream*& rodata = rodata_[i];
        DCHECK(rodata != nullptr);
        if (!oat_writer->WriteRodata(rodata)) {
          LOG(ERROR) << "Failed to write .rodata section to the ELF file " << oat_file->GetPath();
          return false;
        }
        elf_writer->EndRoData(rodata);
        rodata = nullptr;

        OutputStream* text = elf_writer->StartText();
        if (!oat_writer->WriteCode(text)) {
          LOG(ERROR) << "Failed to write .text section to the ELF file " << oat_file->GetPath();
          return false;
        }
        elf_writer->EndText(text);

        if (!oat_writer->WriteHeader(elf_writer->GetStream(),
                                     image_file_location_oat_checksum_,
                                     image_file_location_oat_data_begin_,
                                     image_patch_delta_)) {
          LOG(ERROR) << "Failed to write oat header to the ELF file " << oat_file->GetPath();
          return false;
        }

        if (IsImage()) {
          // Update oat header information.
          DCHECK(image_writer_ != nullptr);
          DCHECK_LT(i, oat_filenames_.size());
          image_writer_->UpdateOatFileHeader(i, oat_writer->GetOatHeader());
        }

        elf_writer->WriteDynamicSection();
        elf_writer->WriteDebugInfo(oat_writer->GetMethodDebugInfo());

        if (!elf_writer->End()) {
          LOG(ERROR) << "Failed to write ELF file " << oat_file->GetPath();
          return false;
        }

        if (!FlushOutputFile(&vdex_files_[i]) || !FlushOutputFile(&oat_files_[i])) {
          return false;
        }

        VLOG(compiler) << "Oat file written successfully: " << oat_filenames_[i];

        oat_writer.reset();
        elf_writer.reset();
      }
    }

    return true;
  }

  // If we are compiling an image, invoke the image creation routine. Else just skip.
  bool HandleImage() {
    if (IsImage()) {
      TimingLogger::ScopedTiming t("dex2oat ImageWriter", timings_);
      if (!CreateImageFile()) {
        return false;
      }
      VLOG(compiler) << "Images written successfully";
    }
    return true;
  }

  // Create a copy from stripped to unstripped.
  bool CopyStrippedToUnstripped() {
    for (size_t i = 0; i < oat_unstripped_.size(); ++i) {
      // If we don't want to strip in place, copy from stripped location to unstripped location.
      // We need to strip after image creation because FixupElf needs to use .strtab.
      if (strcmp(oat_unstripped_[i], oat_filenames_[i]) != 0) {
        // If the oat file is still open, flush it.
        if (oat_files_[i].get() != nullptr && oat_files_[i]->IsOpened()) {
          if (!FlushCloseOutputFile(&oat_files_[i])) {
            return false;
          }
        }

        TimingLogger::ScopedTiming t("dex2oat OatFile copy", timings_);
        std::unique_ptr<File> in(OS::OpenFileForReading(oat_filenames_[i]));
        std::unique_ptr<File> out(OS::CreateEmptyFile(oat_unstripped_[i]));
        int64_t in_length = in->GetLength();
        if (in_length < 0) {
          PLOG(ERROR) << "Failed to get the length of oat file: " << in->GetPath();
          return false;
        }
        if (!out->Copy(in.get(), 0, in_length)) {
          PLOG(ERROR) << "Failed to copy oat file to file: " << out->GetPath();
          return false;
        }
        if (out->FlushCloseOrErase() != 0) {
          PLOG(ERROR) << "Failed to flush and close copied oat file: " << oat_unstripped_[i];
          return false;
        }
        VLOG(compiler) << "Oat file copied successfully (unstripped): " << oat_unstripped_[i];
      }
    }
    return true;
  }

  bool FlushOutputFile(std::unique_ptr<File>* file) {
    if (file->get() != nullptr) {
      if (file->get()->Flush() != 0) {
        PLOG(ERROR) << "Failed to flush output file: " << file->get()->GetPath();
        return false;
      }
    }
    return true;
  }

  bool FlushCloseOutputFile(std::unique_ptr<File>* file) {
    if (file->get() != nullptr) {
      std::unique_ptr<File> tmp(file->release());
      if (tmp->FlushCloseOrErase() != 0) {
        PLOG(ERROR) << "Failed to flush and close output file: " << tmp->GetPath();
        return false;
      }
    }
    return true;
  }

  bool FlushOutputFiles() {
    TimingLogger::ScopedTiming t2("dex2oat Flush Output Files", timings_);
    for (auto& files : { &vdex_files_, &oat_files_ }) {
      for (size_t i = 0; i < files->size(); ++i) {
        if (!FlushOutputFile(&(*files)[i])) {
          return false;
        }
      }
    }
    return true;
  }

  bool FlushCloseOutputFiles() {
    bool result = true;
    for (auto& files : { &vdex_files_, &oat_files_ }) {
      for (size_t i = 0; i < files->size(); ++i) {
        result &= FlushCloseOutputFile(&(*files)[i]);
      }
    }
    return result;
  }

  void DumpTiming() {
    if (dump_timing_ || (dump_slow_timing_ && timings_->GetTotalNs() > MsToNs(1000))) {
      LOG(INFO) << Dumpable<TimingLogger>(*timings_);
    }
    if (dump_passes_) {
      LOG(INFO) << Dumpable<CumulativeLogger>(*driver_->GetTimingsLogger());
    }
  }

  bool IsImage() const {
    return IsAppImage() || IsBootImage();
  }

  bool IsAppImage() const {
    return compiler_options_->IsAppImage();
  }

  bool IsBootImage() const {
    return compiler_options_->IsBootImage();
  }

  bool IsHost() const {
    return is_host_;
  }

  bool UseProfile() const {
    return profile_file_fd_ != -1 || !profile_file_.empty();
  }

  bool DoProfileGuidedOptimizations() const {
    return UseProfile();
  }

  bool DoDexLayoutOptimizations() const {
    return DoProfileGuidedOptimizations();
  }

  bool DoEagerUnquickeningOfVdex() const {
    // DexLayout can invalidate the vdex metadata, so we need to unquicken
    // the vdex file eagerly, before passing it to dexlayout.
    return DoDexLayoutOptimizations();
  }

  bool LoadProfile() {
    DCHECK(UseProfile());
    // TODO(calin): We should be using the runtime arena pool (instead of the
    // default profile arena). However the setup logic is messy and needs
    // cleaning up before that (e.g. the oat writers are created before the
    // runtime).
    profile_compilation_info_.reset(new ProfileCompilationInfo());
    ScopedFlock profile_file;
    std::string error;
    if (profile_file_fd_ != -1) {
      profile_file = LockedFile::DupOf(profile_file_fd_, "profile",
                                       true /* read_only_mode */, &error);
    } else if (profile_file_ != "") {
      profile_file = LockedFile::Open(profile_file_.c_str(), O_RDONLY, true, &error);
    }

    // Return early if we're unable to obtain a lock on the profile.
    if (profile_file.get() == nullptr) {
      LOG(ERROR) << "Cannot lock profiles: " << error;
      return false;
    }

    if (!profile_compilation_info_->Load(profile_file->Fd())) {
      profile_compilation_info_.reset(nullptr);
      return false;
    }

    return true;
  }

 private:
  bool UseSwap(bool is_image, const std::vector<const DexFile*>& dex_files) {
    if (is_image) {
      // Don't use swap, we know generation should succeed, and we don't want to slow it down.
      return false;
    }
    if (dex_files.size() < min_dex_files_for_swap_) {
      // If there are less dex files than the threshold, assume it's gonna be fine.
      return false;
    }
    size_t dex_files_size = 0;
    for (const auto* dex_file : dex_files) {
      dex_files_size += dex_file->GetHeader().file_size_;
    }
    return dex_files_size >= min_dex_file_cumulative_size_for_swap_;
  }

  bool IsVeryLarge(std::vector<const DexFile*>& dex_files) {
    size_t dex_files_size = 0;
    for (const auto* dex_file : dex_files) {
      dex_files_size += dex_file->GetHeader().file_size_;
    }
    return dex_files_size >= very_large_threshold_;
  }

  std::vector<std::string> GetClassPathLocations(const std::string& class_path) {
    // This function is used only for apps and for an app we have exactly one oat file.
    DCHECK(!IsBootImage());
    DCHECK_EQ(oat_writers_.size(), 1u);
    std::vector<std::string> dex_files_canonical_locations;
    for (const std::string& location : oat_writers_[0]->GetSourceLocations()) {
      dex_files_canonical_locations.push_back(DexFile::GetDexCanonicalLocation(location.c_str()));
    }

    std::vector<std::string> parsed;
    Split(class_path, ':', &parsed);
    auto kept_it = std::remove_if(parsed.begin(),
                                  parsed.end(),
                                  [dex_files_canonical_locations](const std::string& location) {
      return ContainsElement(dex_files_canonical_locations,
                             DexFile::GetDexCanonicalLocation(location.c_str()));
    });
    parsed.erase(kept_it, parsed.end());
    return parsed;
  }

  bool PrepareImageClasses() {
    // If --image-classes was specified, calculate the full list of classes to include in the image.
    if (image_classes_filename_ != nullptr) {
      image_classes_ =
          ReadClasses(image_classes_zip_filename_, image_classes_filename_, "image");
      if (image_classes_ == nullptr) {
        return false;
      }
    } else if (IsBootImage()) {
      image_classes_.reset(new std::unordered_set<std::string>);
    }
    return true;
  }

  bool PrepareCompiledClasses() {
    // If --compiled-classes was specified, calculate the full list of classes to compile in the
    // image.
    if (compiled_classes_filename_ != nullptr) {
      compiled_classes_ =
          ReadClasses(compiled_classes_zip_filename_, compiled_classes_filename_, "compiled");
      if (compiled_classes_ == nullptr) {
        return false;
      }
    } else {
      compiled_classes_.reset(nullptr);  // By default compile everything.
    }
    return true;
  }

  static std::unique_ptr<std::unordered_set<std::string>> ReadClasses(const char* zip_filename,
                                                                      const char* classes_filename,
                                                                      const char* tag) {
    std::unique_ptr<std::unordered_set<std::string>> classes;
    std::string error_msg;
    if (zip_filename != nullptr) {
      classes.reset(ReadImageClassesFromZip(zip_filename, classes_filename, &error_msg));
    } else {
      classes.reset(ReadImageClassesFromFile(classes_filename));
    }
    if (classes == nullptr) {
      LOG(ERROR) << "Failed to create list of " << tag << " classes from '"
                 << classes_filename << "': " << error_msg;
    }
    return classes;
  }

  bool PrepareCompiledMethods() {
    // If --compiled-methods was specified, read the methods to compile from the given file(s).
    if (compiled_methods_filename_ != nullptr) {
      std::string error_msg;
      if (compiled_methods_zip_filename_ != nullptr) {
        compiled_methods_.reset(ReadCommentedInputFromZip<std::unordered_set<std::string>>(
            compiled_methods_zip_filename_,
            compiled_methods_filename_,
            nullptr,            // No post-processing.
            &error_msg));
      } else {
        compiled_methods_.reset(ReadCommentedInputFromFile<std::unordered_set<std::string>>(
            compiled_methods_filename_,
            nullptr));          // No post-processing.
      }
      if (compiled_methods_.get() == nullptr) {
        LOG(ERROR) << "Failed to create list of compiled methods from '"
            << compiled_methods_filename_ << "': " << error_msg;
        return false;
      }
    } else {
      compiled_methods_.reset(nullptr);  // By default compile everything.
    }
    return true;
  }

  bool PrepareDirtyObjects() {
    if (dirty_image_objects_filename_ != nullptr) {
      dirty_image_objects_.reset(ReadCommentedInputFromFile<std::unordered_set<std::string>>(
          dirty_image_objects_filename_,
          nullptr));
      if (dirty_image_objects_ == nullptr) {
        LOG(ERROR) << "Failed to create list of dirty objects from '"
            << dirty_image_objects_filename_ << "'";
        return false;
      }
    } else {
      dirty_image_objects_.reset(nullptr);
    }
    return true;
  }

  void PruneNonExistentDexFiles() {
    DCHECK_EQ(dex_filenames_.size(), dex_locations_.size());
    size_t kept = 0u;
    for (size_t i = 0, size = dex_filenames_.size(); i != size; ++i) {
      if (!OS::FileExists(dex_filenames_[i])) {
        LOG(WARNING) << "Skipping non-existent dex file '" << dex_filenames_[i] << "'";
      } else {
        dex_filenames_[kept] = dex_filenames_[i];
        dex_locations_[kept] = dex_locations_[i];
        ++kept;
      }
    }
    dex_filenames_.resize(kept);
    dex_locations_.resize(kept);
  }

  bool AddDexFileSources() {
    TimingLogger::ScopedTiming t2("AddDexFileSources", timings_);
    if (input_vdex_file_ != nullptr) {
      DCHECK_EQ(oat_writers_.size(), 1u);
      const std::string& name = zip_location_.empty() ? dex_locations_[0] : zip_location_;
      DCHECK(!name.empty());
      if (!oat_writers_[0]->AddVdexDexFilesSource(*input_vdex_file_.get(), name.c_str())) {
        return false;
      }
    } else if (zip_fd_ != -1) {
      DCHECK_EQ(oat_writers_.size(), 1u);
      if (!oat_writers_[0]->AddZippedDexFilesSource(File(zip_fd_, /* check_usage */ false),
                                                    zip_location_.c_str())) {
        return false;
      }
    } else if (oat_writers_.size() > 1u) {
      // Multi-image.
      DCHECK_EQ(oat_writers_.size(), dex_filenames_.size());
      DCHECK_EQ(oat_writers_.size(), dex_locations_.size());
      for (size_t i = 0, size = oat_writers_.size(); i != size; ++i) {
        if (!oat_writers_[i]->AddDexFileSource(dex_filenames_[i], dex_locations_[i])) {
          return false;
        }
      }
    } else {
      DCHECK_EQ(oat_writers_.size(), 1u);
      DCHECK_EQ(dex_filenames_.size(), dex_locations_.size());
      DCHECK_NE(dex_filenames_.size(), 0u);
      for (size_t i = 0; i != dex_filenames_.size(); ++i) {
        if (!oat_writers_[0]->AddDexFileSource(dex_filenames_[i], dex_locations_[i])) {
          return false;
        }
      }
    }
    return true;
  }

  void CreateOatWriters() {
    TimingLogger::ScopedTiming t2("CreateOatWriters", timings_);
    elf_writers_.reserve(oat_files_.size());
    oat_writers_.reserve(oat_files_.size());
    for (const std::unique_ptr<File>& oat_file : oat_files_) {
      elf_writers_.emplace_back(CreateElfWriterQuick(instruction_set_,
                                                     instruction_set_features_.get(),
                                                     compiler_options_.get(),
                                                     oat_file.get()));
      elf_writers_.back()->Start();
      const bool do_dexlayout = DoDexLayoutOptimizations();

      if (rewrite_dex_location_checksums_) {
          oat_writers_.emplace_back(new OatWriter(
                  IsBootImage(), timings_, do_dexlayout ? profile_compilation_info_.get() : nullptr, &dex_locations_));
      } else {
          oat_writers_.emplace_back(new OatWriter(
                  IsBootImage(), timings_, do_dexlayout ? profile_compilation_info_.get() : nullptr));
      }
    }
  }

  void SaveDexInput() {
    for (size_t i = 0; i < dex_files_.size(); ++i) {
      const DexFile* dex_file = dex_files_[i];
      std::string tmp_file_name(StringPrintf("/data/local/tmp/dex2oat.%d.%zd.dex",
                                             getpid(), i));
      std::unique_ptr<File> tmp_file(OS::CreateEmptyFile(tmp_file_name.c_str()));
      if (tmp_file.get() == nullptr) {
        PLOG(ERROR) << "Failed to open file " << tmp_file_name
            << ". Try: adb shell chmod 777 /data/local/tmp";
        continue;
      }
      // This is just dumping files for debugging. Ignore errors, and leave remnants.
      UNUSED(tmp_file->WriteFully(dex_file->Begin(), dex_file->Size()));
      UNUSED(tmp_file->Flush());
      UNUSED(tmp_file->Close());
      LOG(INFO) << "Wrote input to " << tmp_file_name;
    }
  }

  bool PrepareRuntimeOptions(RuntimeArgumentMap* runtime_options) {
    RuntimeOptions raw_options;
    if (boot_image_filename_.empty()) {
      std::string boot_class_path = "-Xbootclasspath:";
      boot_class_path += android::base::Join(dex_filenames_, ':');
      raw_options.push_back(std::make_pair(boot_class_path, nullptr));
      std::string boot_class_path_locations = "-Xbootclasspath-locations:";
      boot_class_path_locations += android::base::Join(dex_locations_, ':');
      raw_options.push_back(std::make_pair(boot_class_path_locations, nullptr));
    } else {
      std::string boot_image_option = "-Ximage:";
      boot_image_option += boot_image_filename_;
      raw_options.push_back(std::make_pair(boot_image_option, nullptr));
    }
    for (size_t i = 0; i < runtime_args_.size(); i++) {
      raw_options.push_back(std::make_pair(runtime_args_[i], nullptr));
    }

    raw_options.push_back(std::make_pair("compilercallbacks", callbacks_.get()));
    raw_options.push_back(
        std::make_pair("imageinstructionset", GetInstructionSetString(instruction_set_)));

    // Only allow no boot image for the runtime if we're compiling one. When we compile an app,
    // we don't want fallback mode, it will abort as we do not push a boot classpath (it might
    // have been stripped in preopting, anyways).
    if (!IsBootImage()) {
      raw_options.push_back(std::make_pair("-Xno-dex-file-fallback", nullptr));
    }
    // Never allow implicit image compilation.
    raw_options.push_back(std::make_pair("-Xnoimage-dex2oat", nullptr));
    // Disable libsigchain. We don't don't need it during compilation and it prevents us
    // from getting a statically linked version of dex2oat (because of dlsym and RTLD_NEXT).
    raw_options.push_back(std::make_pair("-Xno-sig-chain", nullptr));
    // Disable Hspace compaction to save heap size virtual space.
    // Only need disable Hspace for OOM becasue background collector is equal to
    // foreground collector by default for dex2oat.
    raw_options.push_back(std::make_pair("-XX:DisableHSpaceCompactForOOM", nullptr));

    if (compiler_options_->IsForceDeterminism()) {
      // If we're asked to be deterministic, ensure non-concurrent GC for determinism.
      //
      // Note that with read barriers, this option is ignored, because Runtime::Init
      // overrides the foreground GC to be gc::kCollectorTypeCC when instantiating
      // gc::Heap. This is fine, as concurrent GC requests are not honored in dex2oat,
      // which uses an unstarted runtime.
      raw_options.push_back(std::make_pair("-Xgc:nonconcurrent", nullptr));

      // The default LOS implementation (map) is not deterministic. So disable it.
      raw_options.push_back(std::make_pair("-XX:LargeObjectSpace=disabled", nullptr));

      // We also need to turn off the nonmoving space. For that, we need to disable HSpace
      // compaction (done above) and ensure that neither foreground nor background collectors
      // are concurrent.
      //
      // Likewise, this option is ignored with read barriers because Runtime::Init
      // overrides the background GC to be gc::kCollectorTypeCCBackground, but that's
      // fine too, for the same reason (see above).
      raw_options.push_back(std::make_pair("-XX:BackgroundGC=nonconcurrent", nullptr));

      // To make identity hashcode deterministic, set a known seed.
      mirror::Object::SetHashCodeSeed(987654321U);
    }

    if (!Runtime::ParseOptions(raw_options, false, runtime_options)) {
      LOG(ERROR) << "Failed to parse runtime options";
      return false;
    }
    return true;
  }

  // Create a runtime necessary for compilation.
  bool CreateRuntime(RuntimeArgumentMap&& runtime_options) {
    TimingLogger::ScopedTiming t_runtime("Create runtime", timings_);
    if (!Runtime::Create(std::move(runtime_options))) {
      LOG(ERROR) << "Failed to create runtime";
      return false;
    }

    // Runtime::Init will rename this thread to be "main". Prefer "dex2oat" so that "top" and
    // "ps -a" don't change to non-descript "main."
    SetThreadName(kIsDebugBuild ? "dex2oatd" : "dex2oat");

    runtime_.reset(Runtime::Current());
    runtime_->SetInstructionSet(instruction_set_);
    for (uint32_t i = 0; i < static_cast<uint32_t>(CalleeSaveType::kLastCalleeSaveType); ++i) {
      CalleeSaveType type = CalleeSaveType(i);
      if (!runtime_->HasCalleeSaveMethod(type)) {
        runtime_->SetCalleeSaveMethod(runtime_->CreateCalleeSaveMethod(), type);
      }
    }

    // Initialize maps for unstarted runtime. This needs to be here, as running clinits needs this
    // set up.
    interpreter::UnstartedRuntime::Initialize();

    runtime_->GetClassLinker()->RunRootClinits();

    // Runtime::Create acquired the mutator_lock_ that is normally given away when we
    // Runtime::Start, give it away now so that we don't starve GC.
    Thread* self = Thread::Current();
    self->TransitionFromRunnableToSuspended(kNative);

    return true;
  }

  // Let the ImageWriter write the image files. If we do not compile PIC, also fix up the oat files.
  bool CreateImageFile()
      REQUIRES(!Locks::mutator_lock_) {
    CHECK(image_writer_ != nullptr);
    if (!IsBootImage()) {
      CHECK(image_filenames_.empty());
      image_filenames_.push_back(app_image_file_name_.c_str());
    }
    if (!image_writer_->Write(app_image_fd_,
                              image_filenames_,
                              oat_filenames_)) {
      LOG(ERROR) << "Failure during image file creation";
      return false;
    }

    // We need the OatDataBegin entries.
    dchecked_vector<uintptr_t> oat_data_begins;
    for (size_t i = 0, size = oat_filenames_.size(); i != size; ++i) {
      oat_data_begins.push_back(image_writer_->GetOatDataBegin(i));
    }
    // Destroy ImageWriter before doing FixupElf.
    image_writer_.reset();

    for (size_t i = 0, size = oat_filenames_.size(); i != size; ++i) {
      const char* oat_filename = oat_filenames_[i];
      // Do not fix up the ELF file if we are --compile-pic or compiling the app image
      if (!compiler_options_->GetCompilePic() && IsBootImage()) {
        std::unique_ptr<File> oat_file(OS::OpenFileReadWrite(oat_filename));
        if (oat_file.get() == nullptr) {
          PLOG(ERROR) << "Failed to open ELF file: " << oat_filename;
          return false;
        }

        if (!ElfWriter::Fixup(oat_file.get(), oat_data_begins[i])) {
          oat_file->Erase();
          LOG(ERROR) << "Failed to fixup ELF file " << oat_file->GetPath();
          return false;
        }

        if (oat_file->FlushCloseOrErase()) {
          PLOG(ERROR) << "Failed to flush and close fixed ELF file " << oat_file->GetPath();
          return false;
        }
      }
    }

    return true;
  }

  // Reads the class names (java.lang.Object) and returns a set of descriptors (Ljava/lang/Object;)
  static std::unordered_set<std::string>* ReadImageClassesFromFile(
      const char* image_classes_filename) {
    std::function<std::string(const char*)> process = DotToDescriptor;
    return ReadCommentedInputFromFile<std::unordered_set<std::string>>(image_classes_filename,
                                                                       &process);
  }

  // Reads the class names (java.lang.Object) and returns a set of descriptors (Ljava/lang/Object;)
  static std::unordered_set<std::string>* ReadImageClassesFromZip(
        const char* zip_filename,
        const char* image_classes_filename,
        std::string* error_msg) {
    std::function<std::string(const char*)> process = DotToDescriptor;
    return ReadCommentedInputFromZip<std::unordered_set<std::string>>(zip_filename,
                                                                      image_classes_filename,
                                                                      &process,
                                                                      error_msg);
  }

  // Read lines from the given file, dropping comments and empty lines. Post-process each line with
  // the given function.
  template <typename T>
  static T* ReadCommentedInputFromFile(
      const char* input_filename, std::function<std::string(const char*)>* process) {
    std::unique_ptr<std::ifstream> input_file(new std::ifstream(input_filename, std::ifstream::in));
    if (input_file.get() == nullptr) {
      LOG(ERROR) << "Failed to open input file " << input_filename;
      return nullptr;
    }
    std::unique_ptr<T> result(
        ReadCommentedInputStream<T>(*input_file, process));
    input_file->close();
    return result.release();
  }

  // Read lines from the given file from the given zip file, dropping comments and empty lines.
  // Post-process each line with the given function.
  template <typename T>
  static T* ReadCommentedInputFromZip(
      const char* zip_filename,
      const char* input_filename,
      std::function<std::string(const char*)>* process,
      std::string* error_msg) {
    std::unique_ptr<ZipArchive> zip_archive(ZipArchive::Open(zip_filename, error_msg));
    if (zip_archive.get() == nullptr) {
      return nullptr;
    }
    std::unique_ptr<ZipEntry> zip_entry(zip_archive->Find(input_filename, error_msg));
    if (zip_entry.get() == nullptr) {
      *error_msg = StringPrintf("Failed to find '%s' within '%s': %s", input_filename,
                                zip_filename, error_msg->c_str());
      return nullptr;
    }
    std::unique_ptr<MemMap> input_file(zip_entry->ExtractToMemMap(zip_filename,
                                                                  input_filename,
                                                                  error_msg));
    if (input_file.get() == nullptr) {
      *error_msg = StringPrintf("Failed to extract '%s' from '%s': %s", input_filename,
                                zip_filename, error_msg->c_str());
      return nullptr;
    }
    const std::string input_string(reinterpret_cast<char*>(input_file->Begin()),
                                   input_file->Size());
    std::istringstream input_stream(input_string);
    return ReadCommentedInputStream<T>(input_stream, process);
  }

  // Read lines from the given stream, dropping comments and empty lines. Post-process each line
  // with the given function.
  template <typename T>
  static T* ReadCommentedInputStream(
      std::istream& in_stream,
      std::function<std::string(const char*)>* process) {
    std::unique_ptr<T> output(new T());
    while (in_stream.good()) {
      std::string dot;
      std::getline(in_stream, dot);
      if (android::base::StartsWith(dot, "#") || dot.empty()) {
        continue;
      }
      if (process != nullptr) {
        std::string descriptor((*process)(dot.c_str()));
        output->insert(output->end(), descriptor);
      } else {
        output->insert(output->end(), dot);
      }
    }
    return output.release();
  }

  void LogCompletionTime() {
    // Note: when creation of a runtime fails, e.g., when trying to compile an app but when there
    //       is no image, there won't be a Runtime::Current().
    // Note: driver creation can fail when loading an invalid dex file.
    LOG(INFO) << "dex2oat took "
              << PrettyDuration(NanoTime() - start_ns_)
              << " (" << PrettyDuration(ProcessCpuNanoTime() - start_cputime_ns_) << " cpu)"
              << " (threads: " << thread_count_ << ") "
              << ((Runtime::Current() != nullptr && driver_ != nullptr) ?
                  driver_->GetMemoryUsageString(kIsDebugBuild || VLOG_IS_ON(compiler)) :
                  "");
  }

  std::string StripIsaFrom(const char* image_filename, InstructionSet isa) {
    std::string res(image_filename);
    size_t last_slash = res.rfind('/');
    if (last_slash == std::string::npos || last_slash == 0) {
      return res;
    }
    size_t penultimate_slash = res.rfind('/', last_slash - 1);
    if (penultimate_slash == std::string::npos) {
      return res;
    }
    // Check that the string in-between is the expected one.
    if (res.substr(penultimate_slash + 1, last_slash - penultimate_slash - 1) !=
            GetInstructionSetString(isa)) {
      LOG(WARNING) << "Unexpected string when trying to strip isa: " << res;
      return res;
    }
    return res.substr(0, penultimate_slash) + res.substr(last_slash);
  }

  std::unique_ptr<CompilerOptions> compiler_options_;
  Compiler::Kind compiler_kind_;

  InstructionSet instruction_set_;
  std::unique_ptr<const InstructionSetFeatures> instruction_set_features_;

  uint32_t image_file_location_oat_checksum_;
  uintptr_t image_file_location_oat_data_begin_;
  int32_t image_patch_delta_;
  std::unique_ptr<SafeMap<std::string, std::string> > key_value_store_;

  std::unique_ptr<VerificationResults> verification_results_;

  std::unique_ptr<QuickCompilerCallbacks> callbacks_;

  std::unique_ptr<Runtime> runtime_;

  // The spec describing how the class loader should be setup for compilation.
  std::unique_ptr<ClassLoaderContext> class_loader_context_;

  size_t thread_count_;
  uint64_t start_ns_;
  uint64_t start_cputime_ns_;
  std::unique_ptr<WatchDog> watchdog_;
  std::vector<std::unique_ptr<File>> oat_files_;
  std::vector<std::unique_ptr<File>> vdex_files_;
  std::string oat_location_;
  std::vector<const char*> oat_filenames_;
  std::vector<const char*> oat_unstripped_;
  int oat_fd_;
  int input_vdex_fd_;
  int output_vdex_fd_;
  std::string input_vdex_;
  std::string output_vdex_;
  std::unique_ptr<VdexFile> input_vdex_file_;
  std::vector<const char*> dex_filenames_;
  std::vector<const char*> dex_locations_;

  int zip_fd_;
  std::string zip_location_;
  std::string boot_image_filename_;
  std::vector<const char*> runtime_args_;
  std::vector<const char*> image_filenames_;
  uintptr_t image_base_;
  const char* image_classes_zip_filename_;
  const char* image_classes_filename_;
  ImageHeader::StorageMode image_storage_mode_;
  const char* compiled_classes_zip_filename_;
  const char* compiled_classes_filename_;
  const char* compiled_methods_zip_filename_;
  const char* compiled_methods_filename_;
  const char* passes_to_run_filename_;
  const char* dirty_image_objects_filename_;
  std::unique_ptr<std::unordered_set<std::string>> image_classes_;
  std::unique_ptr<std::unordered_set<std::string>> compiled_classes_;
  std::unique_ptr<std::unordered_set<std::string>> compiled_methods_;
  std::unique_ptr<std::unordered_set<std::string>> dirty_image_objects_;
  std::unique_ptr<std::vector<std::string>> passes_to_run_;
  bool multi_image_;
  bool is_host_;
  std::string android_root_;
  // Dex files we are compiling, does not include the class path dex files.
  std::vector<const DexFile*> dex_files_;
  std::string no_inline_from_string_;

  std::vector<std::unique_ptr<ElfWriter>> elf_writers_;
  std::vector<std::unique_ptr<OatWriter>> oat_writers_;
  std::vector<OutputStream*> rodata_;
  std::vector<std::unique_ptr<OutputStream>> vdex_out_;
  std::unique_ptr<ImageWriter> image_writer_;
  std::unique_ptr<CompilerDriver> driver_;

  bool rewrite_dex_location_checksums_;

  std::vector<std::unique_ptr<MemMap>> opened_dex_files_maps_;
  std::vector<std::unique_ptr<const DexFile>> opened_dex_files_;

  // Note that this might contain pointers owned by class_loader_context_.
  std::vector<const DexFile*> no_inline_from_dex_files_;

  bool dump_stats_;
  bool dump_passes_;
  bool dump_timing_;
  bool dump_slow_timing_;
  bool avoid_storing_invocation_;
  std::string swap_file_name_;
  int swap_fd_;
  size_t min_dex_files_for_swap_ = kDefaultMinDexFilesForSwap;
  size_t min_dex_file_cumulative_size_for_swap_ = kDefaultMinDexFileCumulativeSizeForSwap;
  size_t very_large_threshold_ = std::numeric_limits<size_t>::max();
  std::string app_image_file_name_;
  int app_image_fd_;
  std::string profile_file_;
  int profile_file_fd_;
  std::unique_ptr<ProfileCompilationInfo> profile_compilation_info_;
  TimingLogger* timings_;
  std::unique_ptr<CumulativeLogger> compiler_phases_timings_;
  std::vector<std::vector<const DexFile*>> dex_files_per_oat_file_;
  std::unordered_map<const DexFile*, size_t> dex_file_oat_index_map_;

  // Backing storage.
  std::vector<std::string> char_backing_storage_;

  // See CompilerOptions.force_determinism_.
  bool force_determinism_;

  // Directory of relative classpaths.
  std::string classpath_dir_;

  // Whether the given input vdex is also the output.
  bool update_input_vdex_ = false;

  DISALLOW_IMPLICIT_CONSTRUCTORS(Dex2Oat);
};

static void b13564922() {
#if defined(__linux__) && defined(__arm__)
  int major, minor;
  struct utsname uts;
  if (uname(&uts) != -1 &&
      sscanf(uts.release, "%d.%d", &major, &minor) == 2 &&
      ((major < 3) || ((major == 3) && (minor < 4)))) {
    // Kernels before 3.4 don't handle the ASLR well and we can run out of address
    // space (http://b/13564922). Work around the issue by inhibiting further mmap() randomization.
    int old_personality = personality(0xffffffff);
    if ((old_personality & ADDR_NO_RANDOMIZE) == 0) {
      int new_personality = personality(old_personality | ADDR_NO_RANDOMIZE);
      if (new_personality == -1) {
        LOG(WARNING) << "personality(. | ADDR_NO_RANDOMIZE) failed.";
      }
    }
  }
#endif
}

class ScopedGlobalRef {
 public:
  explicit ScopedGlobalRef(jobject obj) : obj_(obj) {}
  ~ScopedGlobalRef() {
    if (obj_ != nullptr) {
      ScopedObjectAccess soa(Thread::Current());
      soa.Env()->vm->DeleteGlobalRef(soa.Self(), obj_);
    }
  }

 private:
  jobject obj_;
};

static dex2oat::ReturnCode CompileImage(Dex2Oat& dex2oat) {
  dex2oat.LoadClassProfileDescriptors();
  // Keep the class loader that was used for compilation live for the rest of the compilation
  // process.
  ScopedGlobalRef class_loader(dex2oat.Compile());

  if (!dex2oat.WriteOutputFiles()) {
    dex2oat.EraseOutputFiles();
    return dex2oat::ReturnCode::kOther;
  }

  // Flush boot.oat. We always expect the output file by name, and it will be re-opened from the
  // unstripped name. Do not close the file if we are compiling the image with an oat fd since the
  // image writer will require this fd to generate the image.
  if (dex2oat.ShouldKeepOatFileOpen()) {
    if (!dex2oat.FlushOutputFiles()) {
      dex2oat.EraseOutputFiles();
      return dex2oat::ReturnCode::kOther;
    }
  } else if (!dex2oat.FlushCloseOutputFiles()) {
    return dex2oat::ReturnCode::kOther;
  }

  // Creates the boot.art and patches the oat files.
  if (!dex2oat.HandleImage()) {
    return dex2oat::ReturnCode::kOther;
  }

  // When given --host, finish early without stripping.
  if (dex2oat.IsHost()) {
    if (!dex2oat.FlushCloseOutputFiles()) {
      return dex2oat::ReturnCode::kOther;
    }
    dex2oat.DumpTiming();
    return dex2oat::ReturnCode::kNoFailure;
  }

  // Copy stripped to unstripped location, if necessary.
  if (!dex2oat.CopyStrippedToUnstripped()) {
    return dex2oat::ReturnCode::kOther;
  }

  // FlushClose again, as stripping might have re-opened the oat files.
  if (!dex2oat.FlushCloseOutputFiles()) {
    return dex2oat::ReturnCode::kOther;
  }

  dex2oat.DumpTiming();
  return dex2oat::ReturnCode::kNoFailure;
}

static dex2oat::ReturnCode CompileApp(Dex2Oat& dex2oat) {
  // Keep the class loader that was used for compilation live for the rest of the compilation
  // process.
  ScopedGlobalRef class_loader(dex2oat.Compile());

  if (!dex2oat.WriteOutputFiles()) {
    dex2oat.EraseOutputFiles();
    return dex2oat::ReturnCode::kOther;
  }

  // Do not close the oat files here. We might have gotten the output file by file descriptor,
  // which we would lose.

  // When given --host, finish early without stripping.
  if (dex2oat.IsHost()) {
    if (!dex2oat.FlushCloseOutputFiles()) {
      return dex2oat::ReturnCode::kOther;
    }

    dex2oat.DumpTiming();
    return dex2oat::ReturnCode::kNoFailure;
  }

  // Copy stripped to unstripped location, if necessary. This will implicitly flush & close the
  // stripped versions. If this is given, we expect to be able to open writable files by name.
  if (!dex2oat.CopyStrippedToUnstripped()) {
    return dex2oat::ReturnCode::kOther;
  }

  // Flush and close the files.
  if (!dex2oat.FlushCloseOutputFiles()) {
    return dex2oat::ReturnCode::kOther;
  }

  dex2oat.DumpTiming();
  return dex2oat::ReturnCode::kNoFailure;
}

static dex2oat::ReturnCode Dex2oat(int argc, char** argv) {
  b13564922();

  TimingLogger timings("compiler", false, false);

  // Allocate `dex2oat` on the heap instead of on the stack, as Clang
  // might produce a stack frame too large for this function or for
  // functions inlining it (such as main), that would not fit the
  // requirements of the `-Wframe-larger-than` option.
  std::unique_ptr<Dex2Oat> dex2oat = std::make_unique<Dex2Oat>(&timings);

  // Parse arguments. Argument mistakes will lead to exit(EXIT_FAILURE) in UsageError.
  dex2oat->ParseArgs(argc, argv);

  // If needed, process profile information for profile guided compilation.
  // This operation involves I/O.
  if (dex2oat->UseProfile()) {
    if (!dex2oat->LoadProfile()) {
      LOG(ERROR) << "Failed to process profile file";
      return dex2oat::ReturnCode::kOther;
    }
  }

  art::MemMap::Init();  // For ZipEntry::ExtractToMemMap, and vdex.

  // Check early that the result of compilation can be written
  if (!dex2oat->OpenFile()) {
    return dex2oat::ReturnCode::kOther;
  }

  // Print the complete line when any of the following is true:
  //   1) Debug build
  //   2) Compiling an image
  //   3) Compiling with --host
  //   4) Compiling on the host (not a target build)
  // Otherwise, print a stripped command line.
  if (kIsDebugBuild || dex2oat->IsBootImage() || dex2oat->IsHost() || !kIsTargetBuild) {
    LOG(INFO) << CommandLine();
  } else {
    LOG(INFO) << StrippedCommandLine();
  }

  dex2oat::ReturnCode setup_code = dex2oat->Setup();
  if (setup_code != dex2oat::ReturnCode::kNoFailure) {
    dex2oat->EraseOutputFiles();
    return setup_code;
  }

  // Helps debugging on device. Can be used to determine which dalvikvm instance invoked a dex2oat
  // instance. Used by tools/bisection_search/bisection_search.py.
  VLOG(compiler) << "Running dex2oat (parent PID = " << getppid() << ")";

  dex2oat::ReturnCode result;
  if (dex2oat->IsImage()) {
    result = CompileImage(*dex2oat);
  } else {
    result = CompileApp(*dex2oat);
  }

  return result;
}
}  // namespace art

int main(int argc, char** argv) {
  int result = static_cast<int>(art::Dex2oat(argc, argv));
  // Everything was done, do an explicit exit here to avoid running Runtime destructors that take
  // time (bug 10645725) unless we're a debug build or running on valgrind. Note: The Dex2Oat class
  // should not destruct the runtime in this case.
  if (!art::kIsDebugBuild && (RUNNING_ON_MEMORY_TOOL == 0)) {
    _exit(result);
  }
  return result;
}
