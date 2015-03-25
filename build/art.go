package art

import (
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
	artModule
}

func ArtCCLibraryFactory() (blueprint.Module, []interface{}) {
	module := &artCCLibrary{}

	module.LibraryProperties.BuildShared = true
	module.LibraryProperties.BuildStatic = true

	return cc.NewCCLibrary(&module.CCLibrary, module, common.HostAndDeviceSupported)
}

func (a *artCCLibrary) Flags(ctx common.AndroidModuleContext, flags cc.CCFlags) cc.CCFlags {
	flags = a.CCLibrary.Flags(ctx, flags)
	flags = a.artModule.Flags(ctx, flags)
	return flags
}

type artCCBinary struct {
	cc.CCBinary
	artModule
}

func ArtCCBinaryFactory() (blueprint.Module, []interface{}) {
	module := &artCCBinary{}

	return cc.NewCCBinary(&module.CCBinary, module, common.HostAndDeviceSupported)
}

func (a *artCCBinary) DepNames(ctx common.AndroidBaseContext, depNames cc.CCDeps) cc.CCDeps {
	depNames = a.CCBinary.DepNames(ctx, depNames)
	depNames.WholeStaticLibs = append(depNames.WholeStaticLibs, "libsigchain")

	if ctx.Device() {
		depNames.SharedLibs = append(depNames.SharedLibs, "libdl")
	}

	if ctx.Debug() {
		depNames.SharedLibs = append(depNames.SharedLibs, "libartd")
	} else {
		depNames.SharedLibs = append(depNames.SharedLibs, "libart")
	}

	return depNames
}

func (a *artCCBinary) Flags(ctx common.AndroidModuleContext, flags cc.CCFlags) cc.CCFlags {
	flags = a.CCBinary.Flags(ctx, flags)
	flags = a.artModule.Flags(ctx, flags)

	flags.IncludeDirs = append(flags.IncludeDirs,
		"${SrcDir}/art/runtime",
		"${SrcDir}/art/cmdline",
	)

	if ctx.Debug() {
		a.BinaryProperties.Stem = ctx.ModuleName() + "d"
	}

	if !(ctx.Host() && runtime.GOOS == "darwin") {
		// Mac OS linker doesn't understand --export-dynamic.
		flags.LdFlags = append(flags.LdFlags, "-Wl,--export-dynamic")
	}

	if ctx.Host() {
		flags.LdLibs = append(flags.LdLibs, "-lpthread", "-ldl")
	}

	return flags
}

func (a *artModule) Flags(ctx common.AndroidModuleContext, flags cc.CCFlags) cc.CCFlags {
	var baseAddress string
	var instructionSetFeatures string

	if ctx.Host() {
		baseAddress = libartImgHostBaseAddress
		instructionSetFeatures = "default"
		flags.Clang = true // TODO: allow this to be disabled in a Blueprints file?

		if flags.Clang {
			// Bug: 15446488. We don't omit the frame pointer to work around
			// clang/libunwind bugs that cause SEGVs in run-test-004-ThreadStress.
			flags.CFlags = append(flags.CFlags, "-fno-omit-frame-pointer")
		}

		if ctx.Debug() {
			flags.CFlags = append(flags.CFlags,
				// TODO: depends on ART_COVERAGE/NATIVE_COVERAGE/SANTIIZE_HOST
				"-Wframe-larger-than=2700",
			)
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
		flags.CFlags = append(flags.CFlags,
			"-DART_TARGET",
			// To use oprofile_android --callgraph, uncomment this and recompile with "mmm art -B -j16"
			// "-fno-omit-frame-pointer", "-marm", "-mapcs",
		)

		if ctx.Debug() {
			flags.CFlags = append(flags.CFlags,
				// TODO: depends on ART_COVERAGE/NATIVE_COVERAGE
				"-Wframe-larger-than=1728",
			)
		}
	}

	if ctx.Arch().ArchType == common.Arm64 && flags.Clang {
		flags.CFlags = append(flags.CFlags,
			// These are necessary for Clang ARM64 ART builds. TODO: remove.
			"-DNVALGRIND",
			// FIXME: upstream LLVM has a vectorizer bug that needs to be fixed
			"-fno-vectorize",
		)
	}

	if flags.Clang {
		flags.CFlags = append(flags.CFlags,
			// Warn about thread safety violations with clang.
			"-Wthread-safety",
			// Warn if switch fallthroughs aren't annotated.
			"-Wimplicit-fallthrough",
			// Enable float equality warnings.
			"-Wfloat-equal",
			// Enable warning of converting ints to void*.
			"-Wint-to-void-pointer-cast",
		)
	} else {
		flags.CFlags = append(flags.CFlags,
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

	flags.CFlags = append(flags.CFlags,
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
	)

	flags.CFlags = append(flags.CFlags,
		"-DART_BASE_ADDRESS="+baseAddress, // TODO: configurable?
		// TODO: configure based on ctx.Arch()
		"-DART_DEFAULT_INSTRUCTION_SET_FEATURES="+instructionSetFeatures,
	)

	if ctx.Debug() {
		flags.CFlags = append(flags.CFlags,
			"-O2",
			"-DDYNAMIC_ANNOTATIONS_ENABLED=1",
			"-DVIXL_DEBUG",
			"-UNDEBUG",
		)
	} else {
		flags.CFlags = append(flags.CFlags,
			"-O3",
		)
	}

	// TODO: these should all be replaced with exported includes
	flags.IncludeDirs = append(flags.IncludeDirs,
		"${SrcDir}/external/gtest/include",
		"${SrcDir}/external/valgrind/main/include",
		"${SrcDir}/external/valgrind/main",
		"${SrcDir}/external/vixl/src",
		"${SrcDir}/external/zlib",
	)

	// TODO: environment based cflags
	flags.CFlags = append(flags.CFlags,
		// TODO: ART_DEFAULT_GC_TYPE environment variable?
		"-DART_DEFAULT_GC_TYPE_IS_CMS",

		// TODO: ART_IMT_SIZE envirnoment variable?
		"-DIMT_SIZE=64",

		// TODO: ART_USE_OPTIMIZING_COMPILER environment variable?
		// "-DART_USE_OPTIMIZING_COMPILER=1",

		// TODO: ART_HEAP_POISONING environment variable?
		// "-DART_HEAP_POISONING=1",

		// TODO: ART_USE_READ_BARRIER environment variable?
		// "-DART_USE_READ_BARRIER=1",

		// Missing declarations: too many at the moment, as we use "extern" quite a bit.
		// "-Wmissing-declarations",
	)

	return flags
}
