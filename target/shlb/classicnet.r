/* cfrg resource for the ClassicNet CFM shared library.
   The fragment name must match the CMake library target name (ClassicNet). */
#include "Processes.r"
#include "CodeFragments.r"

resource 'cfrg' (0) {
	{
		kPowerPCCFragArch, kIsCompleteCFrag, kNoVersionNum, kNoVersionNum,
		kDefaultStackSize, kNoAppSubFolder,
		kImportLibraryCFrag, kDataForkCFragLocator, kZeroOffset, kCFragGoesToEOF,
		"ClassicNet"
	}
};
