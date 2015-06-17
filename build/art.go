package art

import (
	"android/soong"
	"android/soong/cc"
	"android/soong/common"
	"runtime"

	"github.com/google/blueprint"
)

const (
	libartImgHostBaseAddress   = "0x60000000"
	libartImgTargetBaseAddress = "0x70000000"
)

type artModule struct{}

type artCCLibrary struct {
	cc.CCLibrary
}

func init() {
	soong.RegisterModuleType("art_cc_library", ArtCCLibraryFactory)
	soong.RegisterModuleType("art_cc_binary", ArtCCBinaryFactory)
}

func ArtCCLibraryFactory() (blueprint.Module, []interface{}) {
	module := &artCCLibrary{}

	module.LibraryProperties.BuildShared = true
	module.LibraryProperties.BuildStatic = true

	return cc.NewCCLibrary(&module.CCLibrary, module, common.HostAndDeviceSupported)
}

func (a *artCCLibrary) ModifyProperties(ctx common.AndroidBaseContext) {
	a.CCLibrary.ModifyProperties(ctx)
	artModifyProperties(ctx, &a.CCBase)
}

type artCCBinary struct {
	cc.CCBinary
}

func ArtCCBinaryFactory() (blueprint.Module, []interface{}) {
	module := &artCCBinary{}

	return cc.NewCCBinary(&module.CCBinary, module, common.HostAndDeviceSupported)
}

func (a *artCCBinary) ModifyProperties(ctx common.AndroidBaseContext) {
	a.CCBinary.ModifyProperties(ctx)
	artModifyProperties(ctx, &a.CCBase)

	a.Properties.Whole_static_libs = append(a.Properties.Whole_static_libs, "libsigchain")

	if ctx.Device() {
		a.Properties.Shared_libs = append(a.Properties.Shared_libs, "libdl")
	}

	if ctx.Debug() {
		a.Properties.Shared_libs = append(a.Properties.Shared_libs, "libartd")
	} else {
		a.Properties.Shared_libs = append(a.Properties.Shared_libs, "libart")
	}

	a.Properties.Include_dirs = append(a.Properties.Include_dirs,
		"art/runtime",
		"art/cmdline")

	if ctx.Debug() {
		a.BinaryProperties.Stem = ctx.ModuleName() + "d"
	}

	if !(ctx.Host() && runtime.GOOS == "darwin") {
		// Mac OS linker doesn't understand --export-dynamic.
		a.Properties.Ldflags = append(a.Properties.Ldflags, "-Wl,--export-dynamic")
	}

	if ctx.Host() {
		a.Properties.Ldflags = append(a.Properties.Ldflags, "-lpthread", "-ldl")
	}
}

func artModifyProperties(ctx common.AndroidBaseContext, base *cc.CCBase) {
	var baseAddress string
	var instructionSetFeatures string

	if ctx.Host() {
		baseAddress = libartImgHostBaseAddress
		instructionSetFeatures = "default"
		if !ctx.ContainsProperty("clang") {
			base.Properties.Clang = true
		}

		if base.Properties.Clang {
			// Bug: 15446488. We don't omit the frame pointer to work around
			// clang/libunwind bugs that cause SEGVs in run-test-004-ThreadStress.
			base.Properties.Cflags = append(base.Properties.Cflags, "-fno-omit-frame-pointer")
		}
	} else {
		baseAddress = libartImgTargetBaseAddress
		switch ctx.Arch().CpuVariant {
		case "cortex-a15", "denver", "krait":
			instructionSetFeatures = "atomic_ldrd_strd,div"
		case "cortex-a7":
			instructionSetFeatures = "div"
		default:
			instructionSetFeatures = "default"
		}
		base.Properties.Cflags = append(base.Properties.Cflags,
			"-DART_TARGET",
			// To use oprofile_android --callgraph, uncomment this and recompile with "mmm art -B -j16"
			// "-fno-omit-frame-pointer", "-marm", "-mapcs",
		)
	}

	if !ctx.Debug() {
		if ctx.Device() {
			// TODO: depends on ART_COVERAGE/NATIVE_COVERAGE
			base.Properties.Cflags = append(base.Properties.Cflags, "-Wframe-larger-than=1728")
		} else {
			// Larger frame-size for host clang builds today
			// TODO: depends on ART_COVERAGE/NATIVE_COVERAGE/SANTIIZE_HOST
			base.Properties.Cflags = append(base.Properties.Cflags, "-Wframe-larger-than=2700")
		}
	}

	if ctx.Arch().ArchType == common.Arm64 && base.Properties.Clang {
		base.Properties.Cflags = append(base.Properties.Cflags,
			// These are necessary for Clang ARM64 ART builds. TODO: remove.
			"-DNVALGRIND",
			// FIXME: upstream LLVM has a vectorizer bug that needs to be fixed
			"-fno-vectorize",
		)
	}

	if base.Properties.Clang {
		base.Properties.Cflags = append(base.Properties.Cflags,
			// Warn about thread safety violations with clang.
			"-Wthread-safety",
			// Warn if switch fallthroughs aren't annotated.
			"-Wimplicit-fallthrough",
			// Enable float equality warnings.
			"-Wfloat-equal",
			// Enable warning of converting ints to void*.
			"-Wint-to-void-pointer-cast",
			// Enable warning of wrong unused annotations.
			"-Wused-but-marked-unused",

			// Enable warning for deprecated language features.
			"-Wdeprecated",

			// Enable warning for unreachable break & return.
			"-Wunreachable-code-break",
			"-Wunreachable-code-return",
		)

		if !(ctx.Host() && runtime.GOOS == "darwin") {
			// Enable missing-noreturn only on non-Mac. As lots of things are not implemented for
			// Apple, it's a pain.
			base.Properties.Cflags = append(base.Properties.Cflags, "-Wmissing-noreturn")
		}
	} else {
		base.Properties.Cflags = append(base.Properties.Cflags,
			// GCC-only warnings.
			"-Wunused-but-set-parameter",
			// Suggest const: too many false positives, but good for a trial run.
			//"-Wsuggest-attribute=const",
			// Useless casts: too many, as we need to be 32/64 agnostic, but the compiler knows.
			//"-Wuseless-cast",
			// Zero-as-null: Have to convert all NULL and "diagnostic ignore" all includes like libnativehelper
			// that are still stuck pre-C++11.
			//"-Wzero-as-null-pointer-constant",
			// Suggest final: Have to move to a more recent GCC.
			//"-Wsuggest-final-types",
		)
	}

	base.Properties.Cflags = append(base.Properties.Cflags,
		// Base set of cflags used by all things ART.
		"-fno-rtti",
		"-std=gnu++11",
		"-ggdb3",
		"-Wall",
		"-Werror",
		"-Wextra",
		"-Wstrict-aliasing",
		"-fstrict-aliasing",
		"-Wunreachable-code",
		"-Wredundant-decls",
		"-Wshadow",
		"-Wunused",
		"-fvisibility=protected",

		"-DART_BASE_ADDRESS_MIN_DELTA=-0x1000000",
		"-DART_BASE_ADDRESS_MAX_DELTA=0x1000000",

		// Missing declarations: too many at the moment, as we use "extern" quite a bit.
		// "-Wmissing-declarations",
	)

	base.Properties.Cflags = append(base.Properties.Cflags,
		"-DART_BASE_ADDRESS="+baseAddress, // TODO: configurable?
		"-DART_DEFAULT_INSTRUCTION_SET_FEATURES="+instructionSetFeatures,
	)

	if ctx.Debug() {
		base.Properties.Cflags = append(base.Properties.Cflags,
			"-O2",
			"-DDYNAMIC_ANNOTATIONS_ENABLED=1",
			"-DVIXL_DEBUG",
			"-UNDEBUG",
		)
	} else {
		base.Properties.Cflags = append(base.Properties.Cflags,
			"-O3",
		)
	}

	// TODO: these should all be replaced with exported includes
	base.Properties.Include_dirs = append(base.Properties.Include_dirs,
		"external/gtest/include",
		"external/icu/icu4c/source/common",
		"external/valgrind/main/include",
		"external/valgrind/main",
		"external/vixl/src",
		"external/zlib",
	)

	config := ctx.Config().(common.Config)

	gcType := config.Getenv("ART_DEFAULT_GC_TYPE")
	if gcType == "" {
		gcType = "CMS"
	}
	base.Properties.Cflags = append(base.Properties.Cflags, "-DART_DEFAULT_GC_TYPE_IS_"+gcType)

	imtSize := config.Getenv("ART_IMT_SIZE")
	if imtSize == "" {
		imtSize = "64"
	}
	base.Properties.Cflags = append(base.Properties.Cflags, "-DIMT_SIZE="+imtSize)

	if config.Getenv("ART_USE_OPTIMIZING_COMPILER") == "true" {
		base.Properties.Cflags = append(base.Properties.Cflags, "-DART_USE_OPTIMIZING_COMPILER=1")
	}

	if config.Getenv("ART_HEAP_POISONING") == "true" {
		base.Properties.Cflags = append(base.Properties.Cflags, "-DART_HEAP_POISONING=1")
	}

	if config.Getenv("ART_USE_READ_BARRIER") == "true" {
		base.Properties.Cflags = append(base.Properties.Cflags, "-DART_USE_READ_BARRIER=1")
	}

	if config.Getenv("ART_USE_TLAB") == "true" {
		base.Properties.Cflags = append(base.Properties.Cflags, "-DART_USE_TLAB=1")
	}
}
